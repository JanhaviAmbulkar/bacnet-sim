# Debugging exercises (GDB on Linux, WinDbg on Windows)

`bacnet_sim --enable-fault-injection` adds two deliberate faults so you can practise real post-mortem debugging:

* `FAULT CRASH` — null-pointer write in a worker thread (SIGSEGV / access violation)
* `FAULT DEADLOCK` — two threads take two mutexes in opposite order (classic lock-order inversion)

Build with symbols and without optimisation: `make debug` (-> `build-debug/`) or
`cmake -S . -B build-dbg -DCMAKE_BUILD_TYPE=Debug`.

## 1. Crash under GDB (verified on Linux)
```
gdb --args build-debug/bacnet_sim --enable-fault-injection --admin-port 45902 --udp-port 0
(gdb) run
# from another shell:  printf 'FAULT CRASH\n' | nc 127.0.0.1 45902
Thread 3 "bacnet_sim" received signal SIGSEGV
(gdb) bt
#0 fault_crash ()        at src/simulator.cpp:116
#1 handle_admin (...)    at src/simulator.cpp:240
#2 serve_connection (...)
(gdb) frame 1 ; info locals ; print line
```
Reading the trace: the crash is on a *worker-pool* thread, reached from the admin command handler.

## 2. Core dump, post-mortem (verified on Linux)
```
ulimit -c unlimited
build-debug/bacnet_sim --enable-fault-injection ...        # trigger FAULT CRASH
gdb build-debug/bacnet_sim core                             # file may be named core.<pid> depending on core_pattern
(gdb) bt
(gdb) thread apply all bt
```
If no `core` appears, check `cat /proc/sys/kernel/core_pattern` (systemd/apport may redirect it; `coredumpctl gdb` on systemd).

## 3. Deadlock (verified on Linux)
```
build-debug/bacnet_sim --enable-fault-injection --admin-port 45904 --udp-port 0 &
printf 'FAULT DEADLOCK\n' | nc 127.0.0.1 45904
gdb -p $(pidof bacnet_sim)
(gdb) info threads
(gdb) thread apply all bt 6
```
Two threads are parked in `pthread_mutex_lock`. Switch to each (`thread N`, `frame K`) and note which mutex each one
holds and which it waits for — that cycle is the deadlock. (If ptrace attach is refused:
`echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope`.) The fix is a consistent lock order or `std::scoped_lock(a, b)`.

## 4. WinDbg on Windows (steps to run yourself — NOT verified by the author)
1. Build the Debug configuration with MSVC (produces `.pdb` symbols), or RelWithDebInfo.
2. Run `bacnet_sim.exe --enable-fault-injection`; trigger `FAULT CRASH`.
3. Capture a dump: `procdump -ma -e bacnet_sim.exe` (Sysinternals) or enable Windows Error Reporting LocalDumps
   (`HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps`).
4. Open the `.dmp` in WinDbg, then:
```
.symfix ; .reload
!analyze -v          # exception record + faulting stack
~*k                  # stacks of all threads
.ecxr ; k            # switch to the exception context
```
5. For the deadlock: take a dump while hung (`procdump -ma <pid>`), run `~*k`, and look for threads waiting in
   `RtlpWaitOnCriticalSection` / `WaitForSingleObject` / `SleepConditionVariable*`; `!locks` shows critical-section owners.

Only claim WinDbg on your resume after you have actually done step 4 and can explain the output.
