# What you can truthfully say about this project

Rule of thumb: claim a skill only if (a) the repo really demonstrates it **and** (b) you can explain the code and
answer follow-up questions in an interview. Read the code until you can.

## Verified by tests / tools on Linux — safe to claim once you understand it
* **C and Modern C++ (C++17):** `std::variant`/`std::visit`, `std::optional`, RAII, smart pointers, structured bindings, `shared_mutex`; plain C helper library.
* **OOP & design patterns:** abstract `EventSink`/`ChangeObserver` interfaces; Observer (device model), Factory (`make_ipc_sink`).
* **TCP/IP, UDP, client-server, low-level networking:** BACnet/IP over UDP, TCP admin server, hand-written frame/tag encoder & parser, bounds-checked parsing, `poll`.
* **BACnet (subset):** Who-Is / I-Am / ReadProperty / Error. Say "subset", never "full BACnet stack".
* **Multithreading & synchronization:** thread pool, mutex, condition_variable, shared_mutex, atomics; ThreadSanitizer clean.
* **IPC:** Unix-domain datagram sockets (Linux).
* **System programming:** sockets, signal handling and graceful shutdown, process management in tests.
* **Makefile and CMake:** both build the project; CMake also drives `ctest`.
* **Test automation / regression:** pytest suite (14 tests, independent Python codec), unit-test binary, sanitizers, mutation sanity check.
* **Debugging with GDB, incl. core-dump and deadlock analysis:** exercises in `docs/DEBUGGING.md` (verified on Linux).
* **Perl:** `tools/log_summary.pl` (a small log-analysis script — say exactly that).

## Only after YOU verify it on Windows
Status: the code **cross-compiles** with MinGW-w64 (verified), but has never been *run* on Windows.
* **Windows support / cross-platform:** build with MSVC or MinGW, run `ctest` and the pytest suite (the Unix-socket IPC test is skipped on Windows).
* **Windows named-pipe IPC**, **WinDbg / minidump analysis**, **CI on Linux and Windows** (push to GitHub and get the Actions run green).
  The workflow is in `.github/workflows/ci.yml`; I could not run it.

## Do NOT claim from this project
* **Performance optimisation:** only a baseline benchmark exists. If you profile (`perf`) and improve it, put your real before/after numbers in.
* **Agile / Scrum:** nothing here shows it. Add only if you genuinely worked in sprints elsewhere.
* **Desktop application development, building-automation product experience:** not covered.
* **5+ years of experience:** a project cannot supply this. ATS "years of experience" screens will still flag it.
* **Security hardening:** the admin port has no authentication.
