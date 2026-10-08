// net.hpp - small cross-platform (POSIX + Winsock) socket layer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

namespace net {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

// RAII for WSAStartup/WSACleanup (no-op on POSIX). Create one at the top of main().
class Init {
 public:
  Init();
  ~Init();
  Init(const Init&) = delete;
  Init& operator=(const Init&) = delete;
};

struct Endpoint {
  std::string ip = "127.0.0.1";
  std::uint16_t port = 0;
  std::string str() const { return ip + ":" + std::to_string(port); }
};

enum class LineStatus { Ok, Closed, Timeout, TooLong, Error };

// Move-only owning socket handle.
class Socket {
 public:
  Socket() = default;
  explicit Socket(socket_t s) : s_(s) {}
  ~Socket() { close(); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& o) noexcept : s_(o.s_), buf_(std::move(o.buf_)) { o.s_ = kInvalidSocket; }
  Socket& operator=(Socket&& o) noexcept;

  bool valid() const { return s_ != kInvalidSocket; }
  socket_t native() const { return s_; }
  void close();

  // true if readable (or accept-able) within timeout_ms.
  bool wait_readable(int timeout_ms) const;

  // --- UDP ---
  bool send_to(const Endpoint& to, const std::uint8_t* data, std::size_t len) const;
  // >0 bytes received, 0 timeout, -1 error.
  int recv_from(std::uint8_t* buf, std::size_t cap, Endpoint& from, int timeout_ms) const;

  // --- TCP ---
  Socket accept(int timeout_ms, Endpoint* peer = nullptr) const;  // invalid Socket on timeout
  bool send_all(const std::string& data) const;
  LineStatus recv_line(std::string& line, int timeout_ms, std::size_t max_len = 256);

  std::uint16_t local_port() const;

 private:
  socket_t s_ = kInvalidSocket;
  std::string buf_;  // buffered TCP input for recv_line
};

// All of these throw std::runtime_error on failure.
Socket udp_socket(std::uint16_t bind_port, bool broadcast, const std::string& bind_ip = "0.0.0.0");
Socket tcp_listen(std::uint16_t port, const std::string& bind_ip = "0.0.0.0");
Socket tcp_connect(const Endpoint& to);

}  // namespace net
