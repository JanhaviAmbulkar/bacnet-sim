#include "net.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define NET_POLL WSAPoll
#define NET_CLOSE closesocket
using nfds_type = ULONG;
static std::string err_text() { return "winsock error " + std::to_string(WSAGetLastError()); }
static constexpr int kSendFlags = 0;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#define NET_POLL ::poll
#define NET_CLOSE ::close
using nfds_type = nfds_t;
static std::string err_text() { return std::strerror(errno); }
#ifdef MSG_NOSIGNAL
static constexpr int kSendFlags = MSG_NOSIGNAL;
#else
static constexpr int kSendFlags = 0;
#endif
#endif

namespace net {
namespace {

[[noreturn]] void die(const std::string& what) { throw std::runtime_error(what + ": " + err_text()); }

sockaddr_in make_addr(const std::string& ip, std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) throw std::runtime_error("bad IPv4 address: " + ip);
  return a;
}

Endpoint to_endpoint(const sockaddr_in& a) {
  char text[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &a.sin_addr, text, sizeof text);
  return Endpoint{text, ntohs(a.sin_port)};
}

void set_reuse(socket_t s) {
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
}

}  // namespace

Init::Init() {
#ifdef _WIN32
  WSADATA d;
  if (WSAStartup(MAKEWORD(2, 2), &d) != 0) throw std::runtime_error("WSAStartup failed");
#endif
}
Init::~Init() {
#ifdef _WIN32
  WSACleanup();
#endif
}

Socket& Socket::operator=(Socket&& o) noexcept {
  if (this != &o) {
    close();
    s_ = o.s_;
    buf_ = std::move(o.buf_);
    o.s_ = kInvalidSocket;
  }
  return *this;
}

void Socket::close() {
  if (s_ != kInvalidSocket) {
    NET_CLOSE(s_);
    s_ = kInvalidSocket;
  }
  buf_.clear();
}

bool Socket::wait_readable(int timeout_ms) const {
  if (!valid()) return false;
#ifdef _WIN32
  WSAPOLLFD p{};
#else
  pollfd p{};
#endif
  p.fd = s_;
  p.events = POLLIN;
  const int rc = NET_POLL(&p, static_cast<nfds_type>(1), timeout_ms);
  return rc > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
}

bool Socket::send_to(const Endpoint& to, const std::uint8_t* data, std::size_t len) const {
  const sockaddr_in a = make_addr(to.ip, to.port);
  const auto n = sendto(s_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                        reinterpret_cast<const sockaddr*>(&a), sizeof a);
  return n >= 0 && static_cast<std::size_t>(n) == len;
}

int Socket::recv_from(std::uint8_t* buf, std::size_t cap, Endpoint& from, int timeout_ms) const {
  if (!wait_readable(timeout_ms)) return 0;
  sockaddr_in a{};
#ifdef _WIN32
  int alen = sizeof a;
#else
  socklen_t alen = sizeof a;
#endif
  const auto n = recvfrom(s_, reinterpret_cast<char*>(buf), static_cast<int>(cap), 0,
                          reinterpret_cast<sockaddr*>(&a), &alen);
  if (n < 0) return -1;
  from = to_endpoint(a);
  return static_cast<int>(n);
}

Socket Socket::accept(int timeout_ms, Endpoint* peer) const {
  if (!wait_readable(timeout_ms)) return Socket{};
  sockaddr_in a{};
#ifdef _WIN32
  int alen = sizeof a;
#else
  socklen_t alen = sizeof a;
#endif
  const socket_t c = ::accept(s_, reinterpret_cast<sockaddr*>(&a), &alen);
  if (c == kInvalidSocket) return Socket{};
  if (peer) *peer = to_endpoint(a);
  return Socket{c};
}

bool Socket::send_all(const std::string& data) const {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto n = send(s_, data.data() + sent, static_cast<int>(data.size() - sent), kSendFlags);
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

LineStatus Socket::recv_line(std::string& line, int timeout_ms, std::size_t max_len) {
  for (;;) {
    const auto nl = buf_.find('\n');
    if (nl != std::string::npos) {
      line = buf_.substr(0, nl);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      buf_.erase(0, nl + 1);
      return LineStatus::Ok;
    }
    if (buf_.size() > max_len) return LineStatus::TooLong;
    if (!wait_readable(timeout_ms)) return LineStatus::Timeout;
    char chunk[512];
    const auto n = recv(s_, chunk, sizeof chunk, 0);
    if (n == 0) return LineStatus::Closed;
    if (n < 0) return LineStatus::Error;
    buf_.append(chunk, static_cast<std::size_t>(n));
  }
}

std::uint16_t Socket::local_port() const {
  sockaddr_in a{};
#ifdef _WIN32
  int alen = sizeof a;
#else
  socklen_t alen = sizeof a;
#endif
  if (getsockname(s_, reinterpret_cast<sockaddr*>(&a), &alen) != 0) return 0;
  return ntohs(a.sin_port);
}

Socket udp_socket(std::uint16_t bind_port, bool broadcast, const std::string& bind_ip) {
  Socket s{::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
  if (!s.valid()) die("socket(udp)");
  if (broadcast) {
    int yes = 1;
    if (setsockopt(s.native(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes) != 0)
      die("setsockopt(SO_BROADCAST)");
  }
  set_reuse(s.native());
  const sockaddr_in a = make_addr(bind_ip, bind_port);
  if (bind(s.native(), reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) die("bind(udp)");
  return s;
}

Socket tcp_listen(std::uint16_t port, const std::string& bind_ip) {
  Socket s{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  if (!s.valid()) die("socket(tcp)");
  set_reuse(s.native());
  const sockaddr_in a = make_addr(bind_ip, port);
  if (bind(s.native(), reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) die("bind(tcp)");
  if (listen(s.native(), 64) != 0) die("listen");
  return s;
}

Socket tcp_connect(const Endpoint& to) {
  Socket s{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
  if (!s.valid()) die("socket(tcp)");
  const sockaddr_in a = make_addr(to.ip, to.port);
  if (connect(s.native(), reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) die("connect");
  return s;
}

}  // namespace net
