# bacnet-sim — cross-platform BACnet/IP device simulator (C++17 / C / Python / Perl)

A small but complete system-level project: a **BACnet/IP device simulator**, a **client**, and a **logger**,
written in Modern C++ (C++17) with a plain-C helper library, built with **CMake and Makefile**, tested with a
**C++ unit-test binary** and a **Python (pytest) regression suite**, with a **Perl** log-analysis tool.
Targets Linux and Windows (POSIX sockets / Winsock behind one small abstraction).

BACnet is the building-automation protocol (HVAC, lighting, access control). The simulator implements the
subset needed to behave like a real device on the wire:

| Service | Direction | Notes |
|---|---|---|
| Who-Is (unconfirmed) | client -> device | unicast and global-broadcast NPDU forms |
| I-Am (unconfirmed) | device -> client | device object id, max-APDU 1476, segmentation, vendor id |
| ReadProperty (confirmed) | client -> device | `present-value` of Analog-Input objects |
| ReadProperty-Ack / Error | device -> client | BACnet error class/code for unknown object/property |

Not implemented (on purpose): segmentation, WriteProperty, COV subscriptions, routing, BBMD/foreign devices.

## Architecture

```
            UDP :47808 (BACnet/IP)                   TCP :47809 (admin, line protocol)
  bacnet_client ───────────────┐                       nc / scripts / pytest
                               ▼                                   ▼
                       ┌──────────────────────── bacnet_sim ────────────────────────┐
                       │  UDP thread      →  codec (bacnet.cpp + C bytebuf)          │
                       │  accept loop     →  ThreadPool workers (mutex + condvar)    │
                       │  drift thread    →  random walk of point values             │
                       │          DeviceModel (std::shared_mutex)  ──Observer──┐     │
                       └───────────────────────────────────────────────────────┼─────┘
                                                      IPC (Unix datagram socket│/ Windows named pipe)
                                                                               ▼
                                                                        bacnet_logger → events.log → tools/log_summary.pl
```

| Path | What it is |
|---|---|
| `c/bytebuf.[ch]` | Plain C, bounds-checked big-endian pack/unpack + hex dump (called from C++ via `extern "C"`) |
| `include/bacnet.hpp`, `src/bacnet.cpp` | Codec: `std::variant` messages, BVLC/NPDU/APDU framing, tag parsing; never reads out of bounds |
| `include/net.hpp`, `src/net.cpp` | RAII sockets, `poll`/`WSAPoll`, UDP + TCP, buffered line reads |
| `include/thread_pool.hpp` | Worker pool (mutex, condition_variable, graceful drain) |
| `include/device.hpp`, `src/device.cpp` | Thread-safe point model: `shared_mutex`, atomic counters, **Observer** pattern |
| `include/ipc.hpp`, `src/ipc.cpp` | **Factory** for the platform IPC sink: Unix datagram socket / Windows named pipe |
| `src/simulator.cpp` | `bacnet_sim` main: threads, signals, admin protocol, fault injection (debug exercises) |
| `src/client.cpp` | `bacnet_client whois | read | bench` |
| `src/logger.cpp` | `bacnet_logger` (IPC receiver) |
| `tests/test_codec.cpp` | Unit tests: golden frames, round-trips, truncation, 200k random inputs, C helpers, threaded model |
| `tests/test_integration.py` | pytest regression suite against the real binaries; `tests/bacnet_py.py` is an **independent** Python codec |
| `tools/log_summary.pl` | Perl event-log summariser |
| `docs/DEBUGGING.md` | GDB / WinDbg crash, core/minidump and deadlock exercises |
| `docs/RESUME_CLAIMS.md` | What this repo does and does not support — read before putting it on a resume |

## Build

CMake (Linux, macOS, Windows/MSVC or MinGW):
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
Makefile (Linux/macOS):
```
make            # build into build-make/
make test       # unit tests + pytest
make debug      # -g -O0 build into build-debug/ (for gdb)
```
Python tests need `pip install pytest`. Point them at your binaries with `BUILD_DIR=build` (default looks in
`build-make`, `build`, `build/Release`, `build/Debug`).

## Run

```
# terminal 1 – optional event logger
./build/bacnet_logger --ipc /tmp/bacnet_sim.sock | tee events.log      # Windows: omit --ipc

# terminal 2 – the device
./build/bacnet_sim --device-id 1234 --points 4 --ipc /tmp/bacnet_sim.sock

# terminal 3 – talk to it
./build/bacnet_client whois                    # prints: I-Am device=1234 ...
./build/bacnet_client read --ai 2              # analog-input 2 present-value = 22.xx
./build/bacnet_client bench --ai 1 --count 20000
printf 'LIST\nSET 1 55.5\nGET 1\nSTATS\nQUIT\n' | nc 127.0.0.1 47809

perl tools/log_summary.pl events.log
```
Broadcast discovery: `bacnet_client whois --broadcast` sends the global-broadcast form to 255.255.255.255 (the
simulator answers the requester directly rather than broadcasting its I-Am, to keep the demo simple).
Port 47808 is 0xBAC0, the standard BACnet/IP port. Vendor id `999` is a placeholder, not an ASHRAE-assigned id.

### Admin protocol (TCP, one command per line)
`GET <ai>` · `SET <ai> <value>` · `LIST` · `STATS` · `QUIT` · `FAULT CRASH|DEADLOCK` (only with `--enable-fault-injection`).

## Testing & verification done on Linux (GCC 13, Ubuntu 24.04)
* 41 unit checks pass, also under **ASan + UBSan** and **ThreadSanitizer** (no reports).
* 14 pytest integration tests pass against both the Makefile and the CMake builds.
* Mutation check: corrupting one tag byte in the encoder is caught by both the C++ and the Python tests.
* Crash backtrace, core-dump post-mortem and deadlock inspection verified with GDB 15.
* Windows: the whole project **cross-compiles** (CMake + MinGW-w64 GCC 13, all 4 executables, no warnings). The `.exe` files were **not run** — no Windows machine was available.

## Known limitations (be upfront about these)
* Windows: the Winsock and named-pipe code compiles under MinGW but has **never been executed**. Run the build, `ctest`
  and pytest on a real Windows machine (or via the CI workflow) before claiming Windows support (see `docs/RESUME_CLAIMS.md`).
* The IPC channel is best-effort telemetry: if the logger is absent or slow, events are dropped.
* `bacnet_client bench` measures loopback request/response only; no optimisation work has been done.
* Single process, IPv4 only, no authentication on the admin port (bind to 127.0.0.1 in any shared environment).
