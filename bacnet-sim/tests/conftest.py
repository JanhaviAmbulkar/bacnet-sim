import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""


def find_binary(name):
    """Looks in $BUILD_DIR, then the usual CMake/Make output folders."""
    candidates = []
    if os.environ.get("BUILD_DIR"):
        candidates.append(Path(os.environ["BUILD_DIR"]))
    candidates += [ROOT / d for d in ("build-make", "build", "build/Release", "build/Debug")]
    for d in candidates:
        p = (d if d.is_absolute() else ROOT / d) / (name + EXE)
        if p.exists():
            return str(p)
    pytest.fail(f"cannot find {name}; build first (make, or cmake --build build) or set BUILD_DIR")


class Proc:
    """A child process whose stdout is drained by a thread so it can never block."""

    def __init__(self, args):
        self.p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines = []
        self._q = queue.Queue()
        self._t = threading.Thread(target=self._drain, daemon=True)
        self._t.start()

    def _drain(self):
        for line in self.p.stdout:
            line = line.rstrip("\n")
            self.lines.append(line)
            self._q.put(line)

    def wait_line(self, prefix, timeout=5.0):
        deadline = time.monotonic() + timeout
        for line in list(self.lines):
            if line.startswith(prefix):
                return line
        while True:
            try:
                line = self._q.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                raise AssertionError(f"no line starting with {prefix!r}; got {self.lines}")
            if line.startswith(prefix):
                return line

    def wait_contains(self, text, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(text in l for l in self.lines):
                return True
            time.sleep(0.05)
        return False

    def stop(self, sig=None, timeout=5.0):
        if self.p.poll() is None:
            if sig is not None and os.name != "nt":
                self.p.send_signal(sig)
            else:
                self.p.terminate()
            try:
                self.p.wait(timeout)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self._t.join(timeout=2)
        return self.p.returncode


class Sim(Proc):
    def __init__(self, *extra):
        super().__init__([find_binary("bacnet_sim"), "--udp-port", "0", "--admin-port", "0",
                          "--bind-ip", "127.0.0.1", *extra])
        ready = self.wait_line("READY")
        kv = dict(item.split("=") for item in ready.split()[1:])
        self.udp, self.admin, self.device = int(kv["udp"]), int(kv["admin"]), int(kv["device"])


@pytest.fixture
def sim():
    s = Sim("--no-drift", "--points", "16", "--device-id", "4321")
    yield s
    s.stop()


@pytest.fixture
def make_sim():
    made = []

    def _make(*args):
        s = Sim(*args)
        made.append(s)
        return s

    yield _make
    for s in made:
        s.stop()


@pytest.fixture
def binary():
    return find_binary
