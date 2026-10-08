// bacnet_sim - a BACnet/IP device simulator.
//   * UDP  :47808  BACnet/IP (Who-Is -> I-Am, ReadProperty(present-value) on Analog-Inputs)
//   * TCP  :47809  line-based admin protocol served by a thread pool
//   * IPC          optional event stream to bacnet_logger (Unix socket / named pipe)
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include "bacnet.hpp"
#include "cli.hpp"
#include "device.hpp"
#include "ipc.hpp"
#include "net.hpp"
#include "thread_pool.hpp"

namespace {

using namespace std::chrono_literals;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Config {
  std::string bind_ip = "0.0.0.0";
  std::uint16_t udp_port = bacnet::kDefaultPort;
  std::uint16_t admin_port = bacnet::kDefaultPort + 1;
  std::uint32_t device_id = 1234;
  std::uint32_t points = 4;
  std::uint32_t workers = 4;
  std::string ipc;
  bool faults = false;
  bool drift = true;
};

void usage() {
  std::puts(
      "usage: bacnet_sim [options]\n"
      "  --udp-port N       BACnet/IP UDP port        (default 47808, 0 = ephemeral)\n"
      "  --admin-port N     TCP admin port            (default 47809, 0 = ephemeral)\n"
      "  --bind-ip A.B.C.D  interface to bind         (default 0.0.0.0)\n"
      "  --device-id N      device instance           (default 1234)\n"
      "  --points N         analog-input points       (default 4)\n"
      "  --workers N        admin worker threads      (default 4)\n"
      "  --ipc ENDPOINT     send events to bacnet_logger (e.g. /tmp/bacnet_sim.sock)\n"
      "  --no-drift         keep point values constant\n"
      "  --enable-fault-injection  allow FAULT CRASH / FAULT DEADLOCK (debugging exercises)");
}

bool parse_args(int argc, char** argv, Config& c) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (a == "--udp-port") {
      if (!cli::parse_port(next(), c.udp_port)) return false;
    } else if (a == "--admin-port") {
      if (!cli::parse_port(next(), c.admin_port)) return false;
    } else if (a == "--bind-ip") {
      const char* v = next();
      if (!v) return false;
      c.bind_ip = v;
    } else if (a == "--device-id") {
      if (!cli::parse_u32(next(), c.device_id) || c.device_id > 0x3FFFFF) return false;
    } else if (a == "--points") {
      if (!cli::parse_u32(next(), c.points) || c.points > 1000) return false;
    } else if (a == "--workers") {
      if (!cli::parse_u32(next(), c.workers) || c.workers == 0 || c.workers > 256) return false;
    } else if (a == "--ipc") {
      const char* v = next();
      if (!v) return false;
      c.ipc = v;
    } else if (a == "--no-drift") {
      c.drift = false;
    } else if (a == "--enable-fault-injection") {
      c.faults = true;
    } else {
      return false;
    }
  }
  return true;
}

std::string event(const char* name, const std::string& fields) {
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  return "ts=" + std::to_string(ms) + " evt=" + name + (fields.empty() ? "" : " " + fields);
}

// Observer: forwards point changes to the IPC sink.
class EventPublisher : public ChangeObserver {
 public:
  explicit EventPublisher(ipc::EventSink& s) : sink_(s) {}
  void on_point_changed(std::uint32_t ai, float v) override {
    std::ostringstream o;
    o << "ai=" << ai << " value=" << std::fixed << std::setprecision(2) << v;
    sink_.emit(event("point_change", o.str()));
  }

 private:
  ipc::EventSink& sink_;
};

// ---- deliberate faults for the debugging exercise (docs/DEBUGGING.md) ---------------
void fault_crash() {
  volatile int* p = nullptr;
  *p = 42;  // SIGSEGV on purpose
}

void fault_deadlock() {
  static std::mutex* a = new std::mutex;  // leaked on purpose: threads below never finish
  static std::mutex* b = new std::mutex;
  std::thread([] {
    std::lock_guard<std::mutex> l1(*a);
    std::this_thread::sleep_for(100ms);
    std::lock_guard<std::mutex> l2(*b);
  }).detach();
  std::thread([] {
    std::lock_guard<std::mutex> l1(*b);
    std::this_thread::sleep_for(100ms);
    std::lock_guard<std::mutex> l2(*a);
  }).detach();
}

// ---- UDP / BACnet ---------------------------------------------------------------------
void udp_loop(net::Socket& sock, DeviceModel& model, ipc::EventSink& sink) {
  std::uint8_t buf[bacnet::kMaxFrame + 64];
  while (!g_stop) {
    net::Endpoint from;
    const int n = sock.recv_from(buf, sizeof buf, from, 200);
    if (n == 0) continue;
    if (n < 0) {
      std::this_thread::sleep_for(10ms);  // e.g. ICMP unreachable surfaced as an error on Windows
      continue;
    }
    model.stats.udp_rx++;
    std::string err;
    const auto msg = bacnet::decode(buf, static_cast<std::size_t>(n), &err);
    if (!msg) {
      model.stats.errors++;
      sink.emit(event("bad_packet", "src=" + from.str() + " reason=\"" + err + "\""));
      continue;
    }
    auto reply = [&](const bacnet::Message& m) {
      const auto bytes = bacnet::encode(m);
      if (!bytes.empty() && sock.send_to(from, bytes.data(), bytes.size())) model.stats.udp_tx++;
    };
    if (std::get_if<bacnet::WhoIs>(&*msg)) {
      sink.emit(event("whois", "src=" + from.str()));
      // Real devices broadcast the I-Am; we answer the requester directly for simplicity.
      reply(bacnet::IAm{{bacnet::kObjDevice, model.instance()}, 1476, bacnet::kSegmentationNone, 999});
    } else if (const auto* rp = std::get_if<bacnet::ReadPropertyRequest>(&*msg)) {
      bacnet::ErrorPdu e{rp->invoke_id, 12, 0, 0};
      if (rp->object.type != bacnet::kObjAnalogInput) {
        e.error_class = 1;  // object
        e.error_code = 31;  // unknown-object
        model.stats.errors++;
        reply(e);
      } else if (rp->property != bacnet::kPropPresentValue) {
        e.error_class = 2;  // property
        e.error_code = 32;  // unknown-property
        model.stats.errors++;
        reply(e);
      } else if (const auto v = model.read(rp->object.instance)) {
        std::ostringstream o;
        o << "src=" << from.str() << " ai=" << rp->object.instance << " value=" << std::fixed
          << std::setprecision(2) << *v;
        sink.emit(event("readprop", o.str()));
        reply(bacnet::ReadPropertyAck{rp->invoke_id, rp->object, rp->property, *v});
      } else {
        e.error_class = 1;
        e.error_code = 31;
        model.stats.errors++;
        reply(e);
      }
    }
    // I-Am / Ack / Error PDUs arriving at a device are ignored.
  }
}

// ---- TCP admin ------------------------------------------------------------------------
struct AdminContext {
  DeviceModel& model;
  bool faults;
};

std::string handle_admin(const std::string& line, AdminContext& ctx, bool& quit) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  for (auto& ch : cmd) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  ctx.model.stats.tcp_cmds++;
  std::ostringstream out;
  if (cmd == "GET") {
    std::string a;
    std::uint32_t ai;
    if (!(in >> a) || !cli::parse_u32(a.c_str(), ai)) return "ERR usage: GET <ai>";
    const auto v = ctx.model.read(ai);
    if (!v) return "ERR unknown point";
    out << "OK " << std::fixed << std::setprecision(2) << *v;
    return out.str();
  }
  if (cmd == "SET") {
    std::string a, b, extra;
    std::uint32_t ai;
    float v;
    if (!(in >> a >> b) || (in >> extra) || !cli::parse_u32(a.c_str(), ai) || !cli::parse_float(b, v))
      return "ERR usage: SET <ai> <value>";
    return ctx.model.write(ai, v) ? "OK" : "ERR unknown point";
  }
  if (cmd == "LIST") {
    out << "OK";
    for (const auto& [ai, v] : ctx.model.snapshot()) out << ' ' << ai << '=' << std::fixed << std::setprecision(2) << v;
    return out.str();
  }
  if (cmd == "STATS") {
    const auto& s = ctx.model.stats;
    out << "OK udp_rx=" << s.udp_rx << " udp_tx=" << s.udp_tx << " tcp_cmds=" << s.tcp_cmds
        << " errors=" << s.errors;
    return out.str();
  }
  if (cmd == "QUIT") {
    quit = true;
    return "OK bye";
  }
  if (cmd == "FAULT") {
    std::string what;
    in >> what;
    if (!ctx.faults) return "ERR fault injection disabled (start with --enable-fault-injection)";
    if (what == "CRASH") {
      fault_crash();
      return "OK";  // not reached
    }
    if (what == "DEADLOCK") {
      fault_deadlock();
      return "OK deadlock triggered";
    }
    return "ERR usage: FAULT CRASH|DEADLOCK";
  }
  ctx.model.stats.errors++;
  return "ERR unknown command";
}

void serve_connection(const std::shared_ptr<net::Socket>& conn, AdminContext ctx) {
  std::string line;
  int idle_ticks = 0;
  while (!g_stop) {
    const auto st = conn->recv_line(line, 500);
    if (st == net::LineStatus::Timeout) {
      if (++idle_ticks >= 20) break;  // ~10 s idle -> free the worker
      continue;
    }
    idle_ticks = 0;
    if (st != net::LineStatus::Ok) break;  // closed, too long, or error
    bool quit = false;
    const std::string reply = handle_admin(line, ctx, quit);
    if (!conn->send_all(reply + "\n") || quit) break;
  }
}

// ---- point simulation ------------------------------------------------------------------
struct StopSignal {
  std::mutex m;
  std::condition_variable cv;
  bool stopped = false;
};

void point_drift(DeviceModel& model, StopSignal& stop) {
  std::mt19937 rng{std::random_device{}()};
  std::uniform_real_distribution<float> step(-0.2f, 0.2f);
  for (;;) {
    std::unique_lock<std::mutex> lk(stop.m);
    if (stop.cv.wait_for(lk, 500ms, [&] { return stop.stopped; })) return;
    lk.unlock();
    for (const auto& [ai, v] : model.snapshot()) model.write(ai, v + step(rng));
  }
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parse_args(argc, argv, cfg)) {
    usage();
    return 2;
  }
  try {
    net::Init winsock;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    DeviceModel model(cfg.device_id);
    for (std::uint32_t i = 1; i <= cfg.points; ++i) model.add_point(i, 20.0f + static_cast<float>(i));

    std::unique_ptr<ipc::EventSink> sink;
    try {
      sink = ipc::make_ipc_sink(cfg.ipc);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "warning: IPC disabled: %s\n", e.what());
      sink = ipc::make_ipc_sink("");
    }
    model.add_observer(std::make_shared<EventPublisher>(*sink));

    net::Socket udp = net::udp_socket(cfg.udp_port, false, cfg.bind_ip);
    net::Socket admin = net::tcp_listen(cfg.admin_port, cfg.bind_ip);
    std::printf("READY udp=%u admin=%u device=%u\n", udp.local_port(), admin.local_port(), cfg.device_id);
    std::fflush(stdout);

    StopSignal stop;
    std::thread udp_thread(udp_loop, std::ref(udp), std::ref(model), std::ref(*sink));
    std::thread drift_thread;
    if (cfg.drift) drift_thread = std::thread(point_drift, std::ref(model), std::ref(stop));

    {
      ThreadPool pool(cfg.workers);
      AdminContext ctx{model, cfg.faults};
      while (!g_stop) {
        net::Endpoint peer;
        net::Socket c = admin.accept(200, &peer);
        if (!c.valid()) continue;
        auto conn = std::make_shared<net::Socket>(std::move(c));  // std::function needs copyable captures
        pool.submit([conn, ctx] { serve_connection(conn, ctx); });
      }
    }  // pool destructor joins workers

    {
      std::lock_guard<std::mutex> lk(stop.m);
      stop.stopped = true;
    }
    stop.cv.notify_all();
    udp_thread.join();
    if (drift_thread.joinable()) drift_thread.join();
    std::puts("bacnet_sim: clean shutdown");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
    return 1;
  }
}
