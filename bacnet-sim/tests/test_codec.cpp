// Unit tests for the BACnet codec and the C byte helpers (no framework needed).
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "bacnet.hpp"
#include "device.hpp"
#include "thread_pool.hpp"

extern "C" {
#include "bytebuf.h"
}

static int g_failed = 0, g_run = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_run;                                                                 \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                        \
  } while (0)

using namespace bacnet;

static void test_known_frames() {
  // The canonical Who-Is broadcast frame seen on every BACnet/IP network.
  const Bytes who = encode(WhoIs{}, true);
  const std::uint8_t expect[] = {0x81, 0x0b, 0x00, 0x0c, 0x01, 0x20, 0xff, 0xff, 0x00, 0xff, 0x10, 0x08};
  CHECK(who.size() == sizeof expect && std::memcmp(who.data(), expect, sizeof expect) == 0);

  // Hand-assembled ReadProperty(analog-input 1, present-value), invoke id 7.
  const std::uint8_t rp[] = {0x81, 0x0a, 0x00, 0x11, 0x01, 0x00, 0x00, 0x05, 0x07, 0x0c, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x19, 0x55};
  const Bytes enc = encode(ReadPropertyRequest{7, {kObjAnalogInput, 1}, 85});
  CHECK(enc.size() == sizeof rp && std::memcmp(enc.data(), rp, sizeof rp) == 0);
}

static void test_roundtrips() {
  {
    const IAm in{{kObjDevice, 260001}, 1476, kSegmentationNone, 999};
    const Bytes b = encode(in);
    const auto m = decode(b.data(), b.size());
    const auto* out = m ? std::get_if<IAm>(&*m) : nullptr;
    CHECK(out && out->device == in.device && out->max_apdu == 1476 && out->vendor_id == 999);
  }
  {
    const Bytes b = encode(ReadPropertyAck{200, {kObjAnalogInput, 4194303}, 85, -12.75f});
    const auto m = decode(b.data(), b.size());
    const auto* out = m ? std::get_if<ReadPropertyAck>(&*m) : nullptr;
    CHECK(out && out->invoke_id == 200 && out->object.instance == 4194303 && out->value == -12.75f);
  }
  {
    const Bytes b = encode(ErrorPdu{9, 12, 1, 31});
    const auto m = decode(b.data(), b.size());
    const auto* out = m ? std::get_if<ErrorPdu>(&*m) : nullptr;
    CHECK(out && out->invoke_id == 9 && out->error_class == 1 && out->error_code == 31);
  }
  {
    // Broadcast Who-Is must also decode (NPDU destination fields are skipped correctly).
    const Bytes b = encode(WhoIs{}, true);
    const auto m = decode(b.data(), b.size());
    CHECK(m && std::get_if<WhoIs>(&*m));
  }
  {
    // Property ids above 255 need a 2-byte unsigned.
    const Bytes b = encode(ReadPropertyRequest{1, {kObjAnalogInput, 70000}, 300});
    const auto m = decode(b.data(), b.size());
    const auto* out = m ? std::get_if<ReadPropertyRequest>(&*m) : nullptr;
    CHECK(out && out->property == 300 && out->object.instance == 70000);
  }
}

static void test_rejects_bad_input() {
  const Bytes good = encode(ReadPropertyRequest{1, {kObjAnalogInput, 1}, 85});
  std::string err;
  // Every strict prefix must be rejected cleanly.
  for (std::size_t n = 0; n < good.size(); ++n) CHECK(!decode(good.data(), n, &err));
  // Wrong BVLC type / length.
  Bytes bad = good;
  bad[0] = 0x82;
  CHECK(!decode(bad.data(), bad.size(), &err));
  bad = good;
  bad[3] = 0x20;
  CHECK(!decode(bad.data(), bad.size(), &err) && err.find("length") != std::string::npos);
  // Network-layer message bit set.
  bad = good;
  bad[5] = 0x80;
  CHECK(!decode(bad.data(), bad.size(), &err));
  CHECK(!decode(nullptr, 0, &err));
}

static void test_random_input_never_crashes() {
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> byte(0, 255), len(0, 64);
  int decoded = 0;
  for (int i = 0; i < 200000; ++i) {
    Bytes b(static_cast<std::size_t>(len(rng)));
    for (auto& x : b) x = static_cast<std::uint8_t>(byte(rng));
    if (b.size() >= 4 && (i & 1)) {  // make half of them look like plausible frames
      b[0] = 0x81;
      b[1] = 0x0a;
      b[2] = 0;
      b[3] = static_cast<std::uint8_t>(b.size());
    }
    if (decode(b.data(), b.size())) ++decoded;
  }
  CHECK(decoded >= 0);  // reaching here without a crash/UB report is the real assertion
}

static void test_c_helpers() {
  std::uint8_t buf[6];
  std::size_t off = bb_put_u16(buf, sizeof buf, 0, 0xABCD);
  off = bb_put_u32(buf, sizeof buf, off, 0x01020304);
  CHECK(off == 6 && buf[0] == 0xAB && buf[1] == 0xCD && buf[5] == 0x04);
  CHECK(bb_put_u8(buf, sizeof buf, 6, 1) == BB_ERR);              // full
  CHECK(bb_put_u32(buf, sizeof buf, 3, 1) == BB_ERR);             // would overflow
  CHECK(bb_put_u8(buf, sizeof buf, BB_ERR, 1) == BB_ERR);         // error is sticky
  std::uint32_t v = 0;
  CHECK(bb_get_u32(buf, sizeof buf, 2, &v) == 6 && v == 0x01020304);
  CHECK(bb_get_u32(buf, sizeof buf, 3, &v) == BB_ERR);
  float f = 0;
  std::uint8_t fb[4];
  bb_put_f32(fb, 4, 0, 21.5f);
  CHECK(bb_get_f32(fb, 4, 0, &f) == 4 && f == 21.5f);
  char hex[16];
  bb_hex(fb, 4, hex, sizeof hex);
  CHECK(std::strcmp(hex, "41 ac 00 00") == 0);
}

struct CountingObserver : ChangeObserver {
  std::atomic<int> n{0};
  void on_point_changed(std::uint32_t, float) override { ++n; }
};

static void test_device_model_threads() {
  DeviceModel m(1);
  for (std::uint32_t i = 1; i <= 8; ++i) m.add_point(i, 0.0f);
  auto obs = std::make_shared<CountingObserver>();
  m.add_observer(obs);
  {
    ThreadPool pool(8);
    for (int t = 0; t < 8; ++t)
      pool.submit([&m, t] {
        for (int i = 0; i < 5000; ++i) {
          m.write(static_cast<std::uint32_t>(t + 1), static_cast<float>(i));
          (void)m.read(static_cast<std::uint32_t>((t + 3) % 8 + 1));
          (void)m.snapshot();
        }
      });
  }  // pool destructor drains and joins
  CHECK(obs->n == 8 * 5000);
  CHECK(m.read(1) == 4999.0f);
  CHECK(!m.read(99).has_value());
  CHECK(!m.write(99, 1.0f));
}

int main() {
  test_known_frames();
  test_roundtrips();
  test_rejects_bad_input();
  test_random_input_never_crashes();
  test_c_helpers();
  test_device_model_threads();
  std::printf("%d checks, %d failed\n", g_run, g_failed);
  return g_failed == 0 ? 0 : 1;
}
