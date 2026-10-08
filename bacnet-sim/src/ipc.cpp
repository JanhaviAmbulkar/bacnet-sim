#include "ipc.hpp"

#include <cstring>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ipc {
namespace {

struct NullSink : EventSink {
  void emit(const std::string&) override {}
};

#ifdef _WIN32

struct NamedPipeSink : EventSink {
  explicit NamedPipeSink(std::string name) : name_(std::move(name)) {}
  ~NamedPipeSink() override {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }
  void emit(const std::string& line) override {
    std::lock_guard<std::mutex> lk(m_);
    if (h_ == INVALID_HANDLE_VALUE) {
      h_ = CreateFileA(name_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h_ == INVALID_HANDLE_VALUE) return;  // logger not running: drop event
    }
    const std::string msg = line + "\n";
    DWORD written = 0;
    if (!WriteFile(h_, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr)) {
      CloseHandle(h_);  // logger went away; reconnect on next event
      h_ = INVALID_HANDLE_VALUE;
    }
  }
  std::string name_;
  std::mutex m_;
  HANDLE h_ = INVALID_HANDLE_VALUE;
};

#else

struct UnixDgramSink : EventSink {
  explicit UnixDgramSink(const std::string& path) {
    if (path.size() >= sizeof(addr_.sun_path)) throw std::runtime_error("IPC path too long");
    fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket(AF_UNIX) failed");
    addr_.sun_family = AF_UNIX;
    std::strncpy(addr_.sun_path, path.c_str(), sizeof(addr_.sun_path) - 1);
  }
  ~UnixDgramSink() override {
    if (fd_ >= 0) ::close(fd_);
  }
  void emit(const std::string& line) override {
    // Datagram sockets are safe to use from several threads; failure just means no logger.
    (void)::sendto(fd_, line.data(), line.size(), 0, reinterpret_cast<const sockaddr*>(&addr_), sizeof addr_);
  }
  int fd_ = -1;
  sockaddr_un addr_{};
};

#endif

}  // namespace

std::string default_endpoint() {
#ifdef _WIN32
  return R"(\\.\pipe\bacnet_sim)";
#else
  return "/tmp/bacnet_sim.sock";
#endif
}

std::unique_ptr<EventSink> make_ipc_sink(const std::string& endpoint) {
  if (endpoint.empty()) return std::make_unique<NullSink>();
#ifdef _WIN32
  return std::make_unique<NamedPipeSink>(endpoint);
#else
  return std::make_unique<UnixDgramSink>(endpoint);
#endif
}

void run_logger(const std::string& endpoint, const std::atomic<bool>& running,
                const std::function<void(const std::string&)>& on_line,
                const std::function<void()>& on_ready) {
#ifdef _WIN32
  bool announced = false;
  while (running) {
    HANDLE pipe = CreateNamedPipeA(endpoint.c_str(), PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("CreateNamedPipe failed");
    if (on_ready && !announced) {
      on_ready();  // pipe instance exists: clients can now open it
      announced = true;
    }
    if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
      std::string pending;
      char chunk[1024];
      DWORD n = 0;
      while (ReadFile(pipe, chunk, sizeof chunk, &n, nullptr) && n > 0) {
        pending.append(chunk, n);
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
          on_line(pending.substr(0, nl));
          pending.erase(0, nl + 1);
        }
      }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
#else
  if (endpoint.size() >= sizeof(sockaddr_un{}.sun_path)) throw std::runtime_error("IPC path too long");
  const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error("socket(AF_UNIX) failed");
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);
  ::unlink(endpoint.c_str());  // remove a stale socket file from a previous run
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    throw std::runtime_error("bind(AF_UNIX) failed for " + endpoint);
  }
  if (on_ready) on_ready();  // bound: datagrams sent from now on are queued, not dropped
  char buf[2048];
  while (running) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, 200) <= 0) continue;
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n > 0) on_line(std::string(buf, static_cast<std::size_t>(n)));
  }
  ::close(fd);
  ::unlink(endpoint.c_str());
#endif
}

}  // namespace ipc
