// ipc.hpp - one-way event channel from the simulator to a separate logger process.
//   Linux/POSIX : Unix-domain datagram socket   (default /tmp/bacnet_sim.sock)
//   Windows     : named pipe                    (default \\.\pipe\bacnet_sim)
// EventSink is an abstract interface; make_ipc_sink() is a Factory that picks the
// implementation for the current platform.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace ipc {

struct EventSink {
  virtual ~EventSink() = default;
  virtual void emit(const std::string& line) = 0;  // best effort, never throws, never blocks long
};

std::string default_endpoint();

// Empty endpoint -> a sink that discards everything.
std::unique_ptr<EventSink> make_ipc_sink(const std::string& endpoint);

// Blocks, calling on_line for each received event until `running` becomes false
// (POSIX) or the process is terminated (Windows named-pipe server blocks in ConnectNamedPipe).
// on_ready (optional) is invoked once the endpoint exists, i.e. when senders can safely connect.
void run_logger(const std::string& endpoint, const std::atomic<bool>& running,
                const std::function<void(const std::string&)>& on_line,
                const std::function<void()>& on_ready = {});

}  // namespace ipc
