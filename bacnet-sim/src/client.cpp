// bacnet_client - talk to a BACnet/IP device.
//   bacnet_client whois [--host H] [--port P] [--broadcast] [--timeout MS]
//   bacnet_client read  --ai N [--host H] [--port P] [--timeout MS]
//   bacnet_client bench --ai N --count C [--host H] [--port P]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "bacnet.hpp"
#include "cli.hpp"
#include "net.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = bacnet::kDefaultPort;
  std::uint32_t ai = 1, count = 1000, timeout_ms = 1000;
  bool broadcast = false;
};

void usage() {
  std::puts(
      "usage: bacnet_client <whois|read|bench> [options]\n"
      "  --host A.B.C.D   target (default 127.0.0.1; with --broadcast default 255.255.255.255)\n"
      "  --port N         UDP port (default 47808)\n"
      "  --ai N           analog-input instance (read/bench)\n"
      "  --count N        requests for bench (default 1000)\n"
      "  --timeout MS     reply timeout (default 1000)\n"
      "  --broadcast      send Who-Is as a broadcast frame");
}

bool parse(int argc, char** argv, Options& o) {
  bool host_given = false;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--host") {
      const char* v = next();
      if (!v) return false;
      o.host = v;
      host_given = true;
    } else if (a == "--port") {
      if (!cli::parse_port(next(), o.port)) return false;
    } else if (a == "--ai") {
      if (!cli::parse_u32(next(), o.ai)) return false;
    } else if (a == "--count") {
      if (!cli::parse_u32(next(), o.count) || o.count == 0) return false;
    } else if (a == "--timeout") {
      if (!cli::parse_u32(next(), o.timeout_ms)) return false;
    } else if (a == "--broadcast") {
      o.broadcast = true;
    } else {
      return false;
    }
  }
  if (o.broadcast && !host_given) o.host = "255.255.255.255";
  return true;
}

int ms_left(Clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return left > 0 ? static_cast<int>(left) : 0;
}

int cmd_whois(const Options& o) {
  net::Socket s = net::udp_socket(0, o.broadcast);
  const auto frame = bacnet::encode(bacnet::WhoIs{}, o.broadcast);
  if (!s.send_to({o.host, o.port}, frame.data(), frame.size())) {
    std::fprintf(stderr, "send failed\n");
    return 1;
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds(o.timeout_ms);
  int found = 0;
  std::uint8_t buf[bacnet::kMaxFrame + 64];
  for (;;) {
    const int wait = ms_left(deadline);
    if (wait == 0) break;
    net::Endpoint from;
    const int n = s.recv_from(buf, sizeof buf, from, wait);
    if (n <= 0) continue;
    const auto m = bacnet::decode(buf, static_cast<std::size_t>(n));
    if (m && std::get_if<bacnet::IAm>(&*m)) {
      std::printf("%s from %s\n", bacnet::describe(*m).c_str(), from.str().c_str());
      ++found;
    }
  }
  return found > 0 ? 0 : 1;
}

// Sends one ReadProperty and waits for the matching reply. Returns 0 ok, 1 timeout, 2 BACnet error.
int read_once(net::Socket& s, const Options& o, std::uint8_t invoke, float& value, std::string& what) {
  const auto frame = bacnet::encode(bacnet::ReadPropertyRequest{invoke, {bacnet::kObjAnalogInput, o.ai}});
  if (!s.send_to({o.host, o.port}, frame.data(), frame.size())) return 1;
  const auto deadline = Clock::now() + std::chrono::milliseconds(o.timeout_ms);
  std::uint8_t buf[bacnet::kMaxFrame + 64];
  for (;;) {
    const int wait = ms_left(deadline);
    if (wait == 0) return 1;
    net::Endpoint from;
    const int n = s.recv_from(buf, sizeof buf, from, wait);
    if (n <= 0) continue;
    const auto m = bacnet::decode(buf, static_cast<std::size_t>(n));
    if (!m) continue;
    if (const auto* ack = std::get_if<bacnet::ReadPropertyAck>(&*m)) {
      if (ack->invoke_id != invoke) continue;  // stale reply from an earlier request
      value = ack->value;
      return 0;
    }
    if (const auto* err = std::get_if<bacnet::ErrorPdu>(&*m)) {
      if (err->invoke_id != invoke) continue;
      what = bacnet::describe(*m);
      return 2;
    }
  }
}

int cmd_read(const Options& o) {
  net::Socket s = net::udp_socket(0, false);
  float v = 0;
  std::string what;
  switch (read_once(s, o, 1, v, what)) {
    case 0:
      std::printf("analog-input %u present-value = %.2f\n", o.ai, v);
      return 0;
    case 2:
      std::printf("%s\n", what.c_str());
      return 2;
    default:
      std::fprintf(stderr, "timeout\n");
      return 1;
  }
}

int cmd_bench(const Options& o) {
  net::Socket s = net::udp_socket(0, false);
  std::vector<double> us;
  us.reserve(o.count);
  std::uint32_t lost = 0;
  const auto t_start = Clock::now();
  for (std::uint32_t i = 0; i < o.count; ++i) {
    float v;
    std::string what;
    const auto t0 = Clock::now();
    const int rc = read_once(s, o, static_cast<std::uint8_t>(i & 0xFF), v, what);
    const auto t1 = Clock::now();
    if (rc == 0) {
      us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    } else {
      ++lost;
    }
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - t_start).count();
  if (us.empty()) {
    std::fprintf(stderr, "no replies\n");
    return 1;
  }
  std::sort(us.begin(), us.end());
  auto pct = [&](double p) { return us[static_cast<std::size_t>(p / 100.0 * static_cast<double>(us.size() - 1))]; };
  std::printf("requests=%u ok=%zu lost=%u elapsed_s=%.3f rps=%.0f p50_us=%.0f p95_us=%.0f p99_us=%.0f max_us=%.0f\n",
              o.count, us.size(), lost, elapsed, static_cast<double>(us.size()) / elapsed, pct(50), pct(95), pct(99),
              us.back());
  return lost == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (argc < 2 || !parse(argc, argv, o)) {
    usage();
    return 64;
  }
  const std::string cmd = argv[1];
  try {
    net::Init winsock;
    if (cmd == "whois") return cmd_whois(o);
    if (cmd == "read") return cmd_read(o);
    if (cmd == "bench") return cmd_bench(o);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  usage();
  return 64;
}
