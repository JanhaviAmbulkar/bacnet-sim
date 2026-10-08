"""End-to-end regression tests: launch the real bacnet_sim binary and talk to it
over UDP (BACnet/IP) and TCP (admin protocol)."""
import os
import random
import shutil
import signal
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

import pytest

import bacnet_py as bp
from conftest import Proc, ROOT, find_binary

posix_only = pytest.mark.skipif(os.name == "nt", reason="Unix-domain sockets / POSIX signals")


class Admin:
    """Minimal line-protocol client for the TCP admin port."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.f = self.s.makefile("rw", newline="\n")

    def cmd(self, line):
        self.f.write(line + "\n")
        self.f.flush()
        return self.f.readline().rstrip("\n")

    def close(self):
        self.s.close()


def test_whois_unicast_returns_iam(sim):
    r = bp.request(sim.udp, bp.who_is())
    assert r["type"] == "iam"
    assert r["device"] == 4321 and r["obj_type"] == 8
    assert r["max_apdu"] == 1476 and r["seg"] == 3


def test_whois_broadcast_format_is_accepted(sim):
    r = bp.request(sim.udp, bp.who_is(broadcast=True))
    assert r["type"] == "iam" and r["device"] == 4321


def test_read_present_value_reflects_admin_set(sim):
    a = Admin(sim.admin)
    assert a.cmd("SET 3 42.5") == "OK"
    r = bp.request(sim.udp, bp.read_property(17, 0, 3), want_invoke=17)
    assert r["type"] == "ack" and r["instance"] == 3 and r["prop"] == 85
    assert r["value"] == pytest.approx(42.5)
    a.close()


def test_unknown_object_and_property_return_bacnet_errors(sim):
    r = bp.request(sim.udp, bp.read_property(5, 0, 999), want_invoke=5)  # no such point
    assert (r["type"], r["error_class"], r["error_code"]) == ("error", 1, 31)
    r = bp.request(sim.udp, bp.read_property(6, 1, 1), want_invoke=6)  # analog-output not modelled
    assert (r["type"], r["error_class"], r["error_code"]) == ("error", 1, 31)
    r = bp.request(sim.udp, bp.read_property(7, 0, 1, prop=77), want_invoke=7)  # object-name
    assert (r["type"], r["error_class"], r["error_code"]) == ("error", 2, 32)


def test_malformed_packets_do_not_kill_the_server(sim):
    rng = random.Random(7)
    good = bp.read_property(1, 0, 1)
    junk = [b"", b"\x00", b"hello", b"\x81", b"\x81\x0a\x00\x04", good[:-1], good[:5], b"\x81\x0a\x00\x11" + b"\xff" * 13]
    junk += [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 60))) for _ in range(200)]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        for j in junk:
            s.sendto(j, ("127.0.0.1", sim.udp))
    # still alive and answering
    assert bp.request(sim.udp, bp.who_is())["type"] == "iam"
    a = Admin(sim.admin)
    stats = dict(kv.split("=") for kv in a.cmd("STATS").split()[1:])
    assert int(stats["errors"]) >= 100  # bad frames were counted, not crashed on
    a.close()


def test_admin_protocol_commands_and_validation(sim):
    a = Admin(sim.admin)
    assert a.cmd("GET 1") == "OK 21.00"
    assert a.cmd("get 2") == "OK 22.00"  # commands are case-insensitive
    assert a.cmd("GET 999").startswith("ERR")
    assert a.cmd("GET abc").startswith("ERR usage")
    assert a.cmd("SET 1").startswith("ERR usage")
    assert a.cmd("SET 1 abc").startswith("ERR usage")
    assert a.cmd("SET 1 nan").startswith("ERR usage")
    assert a.cmd("SET 1 2 3").startswith("ERR usage")
    assert a.cmd("SET 999 1.0") == "ERR unknown point"
    assert a.cmd("BOGUS").startswith("ERR unknown")
    assert a.cmd("LIST").startswith("OK 1=21.00 2=22.00 3=23.00")
    assert a.cmd("QUIT") == "OK bye"
    a.close()


def test_concurrent_admin_clients_more_than_workers(make_sim):
    sim = make_sim("--no-drift", "--points", "16", "--workers", "4")
    errors = []

    def worker(i):
        try:
            a = Admin(sim.admin)
            for k in range(50):
                v = i * 100 + k
                assert a.cmd(f"SET {i + 1} {v}") == "OK"
                assert a.cmd(f"GET {i + 1}") == f"OK {v:.2f}"
            a.cmd("QUIT")
            a.close()
        except Exception as e:  # noqa: BLE001
            errors.append(repr(e))

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(16)]  # 16 clients, 4 workers
    [t.start() for t in threads]
    [t.join(timeout=30) for t in threads]
    assert not errors, errors


def test_concurrent_udp_readers_get_correct_values(sim):
    a = Admin(sim.admin)
    for i in range(1, 9):
        assert a.cmd(f"SET {i} {i * 10}") == "OK"
    a.close()
    errors = []

    def reader(i):
        try:
            for k in range(100):
                inv = (i * 31 + k) % 256
                r = bp.request(sim.udp, bp.read_property(inv, 0, i), want_invoke=inv)
                assert r["type"] == "ack" and r["value"] == pytest.approx(i * 10)
        except Exception as e:  # noqa: BLE001
            errors.append(repr(e))

    threads = [threading.Thread(target=reader, args=(i,)) for i in range(1, 9)]
    [t.start() for t in threads]
    [t.join(timeout=30) for t in threads]
    assert not errors, errors


def test_stats_count_traffic(sim):
    for k in range(5):
        bp.request(sim.udp, bp.read_property(k, 0, 1), want_invoke=k)
    a = Admin(sim.admin)
    # udp_tx is bumped just after sendto() returns, so a very fast client can ask for STATS a
    # moment before the counter moves: poll briefly instead of asserting on the first sample.
    deadline = time.monotonic() + 2.0
    while True:
        stats = {k: int(v) for k, v in (kv.split("=") for kv in a.cmd("STATS").split()[1:])}
        if (stats["udp_rx"], stats["udp_tx"]) == (5, 5) or time.monotonic() > deadline:
            break
        time.sleep(0.02)
    assert stats["udp_rx"] == 5 and stats["udp_tx"] == 5 and stats["errors"] == 0, stats
    a.close()


def test_fault_injection_is_disabled_by_default(sim):
    a = Admin(sim.admin)
    assert a.cmd("FAULT CRASH").startswith("ERR fault injection disabled")
    assert sim.p.poll() is None
    a.close()


@posix_only
def test_graceful_shutdown_on_sigterm(make_sim):
    sim = make_sim("--no-drift")
    assert sim.stop(signal.SIGTERM) == 0
    assert any("clean shutdown" in l for l in sim.lines)


@posix_only
def test_ipc_events_reach_logger_process(make_sim):
    sock = tempfile.mktemp(prefix="bsim", suffix=".sock", dir="/tmp")  # short path: sun_path is ~108 bytes
    logger = Proc([find_binary("bacnet_logger"), "--ipc", sock])
    try:
        logger.wait_line("LOGGER listening")
        sim = make_sim("--no-drift", "--ipc", sock)
        bp.request(sim.udp, bp.who_is())
        bp.request(sim.udp, bp.read_property(1, 0, 2), want_invoke=1)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.sendto(b"not a bacnet frame", ("127.0.0.1", sim.udp))
        a = Admin(sim.admin)
        a.cmd("SET 2 9.5")
        a.close()
        assert logger.wait_contains("evt=whois"), logger.lines
        assert logger.wait_contains("evt=readprop"), logger.lines
        assert logger.wait_contains("evt=bad_packet"), logger.lines
        assert logger.wait_contains("evt=point_change ai=2 value=9.50"), logger.lines
    finally:
        logger.stop(signal.SIGTERM)
    assert not os.path.exists(sock)  # logger unlinked its socket file on exit


def test_client_cli_whois_read_and_bench(sim):
    client = find_binary("bacnet_client")
    base = [client]
    out = subprocess.run(base + ["whois", "--port", str(sim.udp)], capture_output=True, text=True, timeout=10)
    assert out.returncode == 0 and "I-Am device=4321" in out.stdout
    out = subprocess.run(base + ["read", "--ai", "2", "--port", str(sim.udp)], capture_output=True, text=True, timeout=10)
    assert out.returncode == 0 and "present-value = 22.00" in out.stdout
    out = subprocess.run(base + ["read", "--ai", "999", "--port", str(sim.udp)], capture_output=True, text=True, timeout=10)
    assert out.returncode == 2 and "Error" in out.stdout
    out = subprocess.run(base + ["bench", "--ai", "1", "--count", "300", "--port", str(sim.udp)],
                         capture_output=True, text=True, timeout=30)
    assert out.returncode == 0 and "ok=300" in out.stdout and "rps=" in out.stdout


@pytest.mark.skipif(shutil.which("perl") is None, reason="perl not installed")
def test_perl_log_summary(tmp_path):
    log = tmp_path / "events.log"
    log.write_text(
        "ts=1 evt=whois src=127.0.0.1:5000\n"
        "ts=2 evt=readprop src=127.0.0.1:5000 ai=1 value=20.00\n"
        "ts=3 evt=readprop src=127.0.0.1:5000 ai=1 value=22.00\n"
        'ts=4 evt=bad_packet src=127.0.0.1:5000 reason="BVLC length mismatch"\n'
        "ts=5 evt=point_change ai=2 value=7.50\n"
    )
    out = subprocess.run(["perl", str(ROOT / "tools" / "log_summary.pl"), str(log)], capture_output=True, text=True, timeout=10)
    assert out.returncode == 0
    assert "total events: 5" in out.stdout
    assert "readprop" in out.stdout and "BVLC length mismatch" in out.stdout
    assert "ai=1" in out.stdout and "mean=21.00" in out.stdout
