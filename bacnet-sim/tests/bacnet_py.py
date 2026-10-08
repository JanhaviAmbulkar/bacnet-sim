"""Independent pure-Python BACnet/IP helpers used to cross-check the C++ codec.

Deliberately does NOT share any code with the C++ implementation: frames are
assembled/parsed by hand with struct so a bug in one side shows up as a test failure.
"""
import socket
import struct

BVLC_UNICAST, BVLC_BROADCAST = 0x0A, 0x0B


def _frame(func, npdu, apdu):
    total = 4 + len(npdu) + len(apdu)
    return struct.pack(">BBH", 0x81, func, total) + npdu + apdu


def who_is(broadcast=False):
    if broadcast:
        return _frame(BVLC_BROADCAST, b"\x01\x20\xff\xff\x00\xff", b"\x10\x08")
    return _frame(BVLC_UNICAST, b"\x01\x00", b"\x10\x08")


def read_property(invoke, obj_type, instance, prop=85):
    obj = (obj_type << 22) | instance
    apdu = bytes([0x00, 0x05, invoke, 0x0C, 0x0C]) + struct.pack(">I", obj) + bytes([0x19, prop])
    return _frame(BVLC_UNICAST, b"\x01\x00", apdu)


def _apdu(data):
    assert data[0] == 0x81 and struct.unpack(">H", data[2:4])[0] == len(data), "bad BVLC"
    assert data[4] == 0x01, "bad NPDU version"
    i = 6
    if data[5] & 0x20:
        i += 3 + data[8]  # DNET(2) DLEN(1) DADR(dlen)
        i += 1  # hop count
    return data[i:]


def parse(data):
    """Returns a dict describing the reply, e.g. {'type': 'iam', ...}."""
    a = _apdu(data)
    kind = a[0] >> 4
    if kind == 1 and a[1] == 0:  # I-Am
        assert a[2] == 0xC4
        (obj,) = struct.unpack(">I", a[3:7])
        assert a[7] & 0xF0 == 0x20
        n = a[7] & 0x07
        max_apdu = int.from_bytes(a[8:8 + n], "big")
        j = 8 + n
        assert a[j] == 0x91
        seg = a[j + 1]
        j += 2
        assert a[j] & 0xF0 == 0x20
        m = a[j] & 0x07
        vendor = int.from_bytes(a[j + 1:j + 1 + m], "big")
        return dict(type="iam", device=obj & 0x3FFFFF, obj_type=obj >> 22, max_apdu=max_apdu, seg=seg, vendor=vendor)
    if kind == 3:  # ComplexAck ReadProperty
        invoke, svc = a[1], a[2]
        assert svc == 12 and a[3] == 0x0C
        (obj,) = struct.unpack(">I", a[4:8])
        assert a[8] & 0xF8 == 0x18
        n = a[8] & 0x07
        prop = int.from_bytes(a[9:9 + n], "big")
        j = 9 + n
        assert a[j] == 0x3E and a[j + 1] == 0x44 and a[j + 6] == 0x3F
        (value,) = struct.unpack(">f", a[j + 2:j + 6])
        return dict(type="ack", invoke=invoke, instance=obj & 0x3FFFFF, prop=prop, value=value)
    if kind == 5:  # Error
        assert a[3] == 0x91 and a[5] == 0x91
        return dict(type="error", invoke=a[1], service=a[2], error_class=a[4], error_code=a[6])
    raise AssertionError(f"unexpected APDU {a.hex()}")


def request(port, payload, timeout=2.0, host="127.0.0.1", want_invoke=None):
    """Send one datagram and return the first parsed reply (matching invoke id if given)."""
    import time

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(payload, (host, port))
        deadline = time.monotonic() + timeout
        while True:
            s.settimeout(max(0.01, deadline - time.monotonic()))
            data, _ = s.recvfrom(2048)
            r = parse(data)
            if want_invoke is None or r.get("invoke") == want_invoke:
                return r
