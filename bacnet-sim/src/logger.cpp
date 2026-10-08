// bacnet_logger - receives events from bacnet_sim over IPC and prints them (one per line).
#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>

#include "ipc.hpp"

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }
}  // namespace

int main(int argc, char** argv) {
  std::string endpoint = ipc::default_endpoint();
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--ipc" && i + 1 < argc) {
      endpoint = argv[++i];
    } else {
      std::puts("usage: bacnet_logger [--ipc ENDPOINT]");
      return 2;
    }
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  try {
    ipc::run_logger(
        endpoint, g_running,
        [](const std::string& line) {
          std::printf("%s\n", line.c_str());
          std::fflush(stdout);
        },
        [&endpoint] {  // only announce once the endpoint really exists
          std::printf("LOGGER listening on %s\n", endpoint.c_str());
          std::fflush(stdout);
        });
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
  return 0;
}
