#include "bacnet.hpp"

#include <sstream>

extern "C" {
#include "bytebuf.h"
}

namespace bacnet {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// ---- thin RAII-free wrappers over the C helpers --------------------------------
class Writer {
 public:
  Writer(std::uint8_t* b, std::size_t cap) : b_(b), cap_(cap) {}
  void u8(std::uint8_t v) { off_ = bb_put_u8(b_, cap_, off_, v); }
  void u16(std::uint16_t v) { off_ = bb_put_u16(b_, cap_, off_, v); }
  void u32(std::uint32_t v) { off_ = bb_put_u32(b_, cap_, off_, v); }
  void f32(float v) { off_ = bb_put_f32(b_, cap_, off_, v); }
  // Variable-length big-endian unsigned: writes tag octet (base | len) then len bytes.
  void uint_tag(std::uint8_t base, std::uint32_t v) {
    const int len = v <= 0xFF ? 1 : v <= 0xFFFF ? 2 : v <= 0xFFFFFF ? 3 : 4;
    u8(static_cast<std::uint8_t>(base | len));
    for (int i = len - 1; i >= 0; --i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
  }
  bool ok() const { return off_ != BB_ERR; }
  std::size_t size() const { return off_; }

 private:
  std::uint8_t* b_;
  std::size_t cap_;
  std::size_t off_ = 0;
};

class Reader {
 public:
  Reader(const std::uint8_t* b, std::size_t len) : b_(b), len_(len) {}
  bool u8(std::uint8_t& v) { return step(bb_get_u8(b_, len_, off_, &v)); }
  bool u16(std::uint16_t& v) { return step(bb_get_u16(b_, len_, off_, &v)); }
  bool u32(std::uint32_t& v) { return step(bb_get_u32(b_, len_, off_, &v)); }
  bool f32(float& v) { return step(bb_get_f32(b_, len_, off_, &v)); }
  bool skip(std::size_t n) {
    if (off_ == BB_ERR || n > len_ - off_) return fail();
    off_ += n;
    return true;
  }
  bool uint_n(std::uint32_t n, std::uint32_t& v) {
    if (n == 0 || n > 4) return fail();
    v = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      std::uint8_t b;
      if (!u8(b)) return false;
      v = (v << 8) | b;
    }
    return true;
  }
  std::size_t remaining() const { return off_ == BB_ERR ? 0 : len_ - off_; }

 private:
  bool step(std::size_t n) {
    if (n == BB_ERR) return fail();
    off_ = n;
    return true;
  }
  bool fail() {
    off_ = BB_ERR;
    return false;
  }
  const std::uint8_t* b_;
  std::size_t len_;
  std::size_t off_ = 0;
};

struct Tag {
  std::uint8_t num = 0;
  bool context = false, opening = false, closing = false;
  std::uint32_t len = 0;
};

bool read_tag(Reader& r, Tag& t) {
  std::uint8_t b;
  if (!r.u8(b)) return false;
  t = Tag{};
  t.num = b >> 4;
  t.context = (b & 0x08) != 0;
  std::uint32_t len = b & 0x07;
  if (t.num == 0x0F) return false;  // extended tag numbers not supported
  if (t.context && len == 6) {
    t.opening = true;
    len = 0;
  } else if (t.context && len == 7) {
    t.closing = true;
    len = 0;
  } else if (len == 5) {
    std::uint8_t ext;
    if (!r.u8(ext)) return false;
    len = ext;
  }
  t.len = len;
  return true;
}

bool fail(std::string* err, const char* why) {
  if (err) *err = why;
  return false;
}

// ---- encoding -------------------------------------------------------------------
constexpr std::uint8_t kBvlcType = 0x81;
constexpr std::uint8_t kBvlcUnicast = 0x0A, kBvlcBroadcast = 0x0B, kBvlcForwarded = 0x04;
constexpr std::uint8_t kPduConfirmed = 0, kPduUnconfirmed = 1, kPduComplexAck = 3, kPduError = 5;
constexpr std::uint8_t kSvcReadProperty = 12, kSvcIAm = 0, kSvcWhoIs = 8;

void put_object_id(Writer& w, std::uint32_t raw) {
  w.u8(0xC4);  // application tag 12 (Object Identifier), length 4
  w.u32(raw);
}
void put_ctx_object_id(Writer& w, std::uint32_t raw) {
  w.u8(0x0C);  // context tag 0, length 4
  w.u32(raw);
}

}  // namespace

Bytes encode(const Message& msg, bool broadcast) {
  std::uint8_t buf[kMaxFrame];
  Writer w(buf, sizeof buf);
  // BVLC header; length patched below.
  w.u8(kBvlcType);
  w.u8(broadcast ? kBvlcBroadcast : kBvlcUnicast);
  w.u16(0);
  // NPDU
  w.u8(1);  // protocol version
  if (broadcast) {
    w.u8(0x20);  // destination specifier present
    w.u16(0xFFFF);  // DNET: global broadcast
    w.u8(0);        // DLEN = 0 (broadcast MAC)
    w.u8(0xFF);     // hop count
  } else {
    w.u8(0x00);
  }
  // APDU
  std::visit(
      Overloaded{
          [&](const WhoIs&) {
            w.u8(kPduUnconfirmed << 4);
            w.u8(kSvcWhoIs);
          },
          [&](const IAm& m) {
            w.u8(kPduUnconfirmed << 4);
            w.u8(kSvcIAm);
            put_object_id(w, m.device.raw());
            w.uint_tag(2 << 4, m.max_apdu);  // application tag 2: unsigned
            w.u8(0x91);                       // application tag 9: enumerated, length 1
            w.u8(m.segmentation);
            w.uint_tag(2 << 4, m.vendor_id);
          },
          [&](const ReadPropertyRequest& m) {
            w.u8(kPduConfirmed << 4);
            w.u8(0x05);  // max segments unspecified / max APDU 1476
            w.u8(m.invoke_id);
            w.u8(kSvcReadProperty);
            put_ctx_object_id(w, m.object.raw());
            w.uint_tag((1 << 4) | 0x08, m.property);  // context tag 1: property id
          },
          [&](const ReadPropertyAck& m) {
            w.u8(kPduComplexAck << 4);
            w.u8(m.invoke_id);
            w.u8(kSvcReadProperty);
            put_ctx_object_id(w, m.object.raw());
            w.uint_tag((1 << 4) | 0x08, m.property);
            w.u8(0x3E);  // opening tag 3
            w.u8(0x44);  // application tag 4: real, length 4
            w.f32(m.value);
            w.u8(0x3F);  // closing tag 3
          },
          [&](const ErrorPdu& m) {
            w.u8(kPduError << 4);
            w.u8(m.invoke_id);
            w.u8(m.service);
            w.u8(0x91);
            w.u8(m.error_class);
            w.u8(0x91);
            w.u8(m.error_code);
          },
      },
      msg);
  if (!w.ok()) return {};
  const std::size_t total = w.size();
  bb_put_u16(buf, sizeof buf, 2, static_cast<std::uint16_t>(total));  // patch BVLC length
  return Bytes(buf, buf + total);
}

// ---- decoding -------------------------------------------------------------------
namespace {

bool decode_apdu(Reader& r, Message& out, std::string* err) {
  std::uint8_t b0;
  if (!r.u8(b0)) return fail(err, "empty APDU");
  const std::uint8_t type = b0 >> 4;
  switch (type) {
    case kPduUnconfirmed: {
      std::uint8_t svc;
      if (!r.u8(svc)) return fail(err, "truncated unconfirmed request");
      if (svc == kSvcWhoIs) {
        out = WhoIs{};  // optional device-range limits are ignored
        return true;
      }
      if (svc == kSvcIAm) {
        Tag t;
        std::uint32_t raw, max_apdu, seg, vendor;
        if (!read_tag(r, t) || t.context || t.num != 12 || t.len != 4 || !r.u32(raw))
          return fail(err, "bad I-Am object id");
        if (!read_tag(r, t) || t.context || t.num != 2 || !r.uint_n(t.len, max_apdu))
          return fail(err, "bad I-Am max APDU");
        if (!read_tag(r, t) || t.context || t.num != 9 || !r.uint_n(t.len, seg))
          return fail(err, "bad I-Am segmentation");
        if (!read_tag(r, t) || t.context || t.num != 2 || !r.uint_n(t.len, vendor))
          return fail(err, "bad I-Am vendor id");
        if (max_apdu > 0xFFFF || vendor > 0xFFFF || seg > 0xFF) return fail(err, "I-Am value out of range");
        out = IAm{ObjectId::from_raw(raw), static_cast<std::uint16_t>(max_apdu),
                  static_cast<std::uint8_t>(seg), static_cast<std::uint16_t>(vendor)};
        return true;
      }
      return fail(err, "unsupported unconfirmed service");
    }
    case kPduConfirmed: {
      if (b0 & 0x08) return fail(err, "segmented requests unsupported");
      std::uint8_t maxseg, invoke, svc;
      if (!r.u8(maxseg) || !r.u8(invoke) || !r.u8(svc)) return fail(err, "truncated confirmed request");
      if (svc != kSvcReadProperty) return fail(err, "unsupported confirmed service");
      Tag t;
      std::uint32_t raw, prop;
      if (!read_tag(r, t) || !t.context || t.num != 0 || t.len != 4 || !r.u32(raw))
        return fail(err, "bad ReadProperty object id");
      if (!read_tag(r, t) || !t.context || t.num != 1 || !r.uint_n(t.len, prop))
        return fail(err, "bad ReadProperty property id");
      out = ReadPropertyRequest{invoke, ObjectId::from_raw(raw), prop};  // optional array index ignored
      return true;
    }
    case kPduComplexAck: {
      std::uint8_t invoke, svc;
      if (!r.u8(invoke) || !r.u8(svc)) return fail(err, "truncated ack");
      if (svc != kSvcReadProperty) return fail(err, "unsupported ack service");
      Tag t;
      std::uint32_t raw, prop;
      float value;
      if (!read_tag(r, t) || !t.context || t.num != 0 || t.len != 4 || !r.u32(raw))
        return fail(err, "bad ack object id");
      if (!read_tag(r, t) || !t.context || t.num != 1 || !r.uint_n(t.len, prop))
        return fail(err, "bad ack property id");
      if (!read_tag(r, t) || !t.opening || t.num != 3) return fail(err, "missing opening tag 3");
      if (!read_tag(r, t) || t.context || t.num != 4 || t.len != 4 || !r.f32(value))
        return fail(err, "ack value is not REAL");
      if (!read_tag(r, t) || !t.closing || t.num != 3) return fail(err, "missing closing tag 3");
      out = ReadPropertyAck{invoke, ObjectId::from_raw(raw), prop, value};
      return true;
    }
    case kPduError: {
      std::uint8_t invoke, svc, cls, code;
      Tag t;
      std::uint32_t c1, c2;
      if (!r.u8(invoke) || !r.u8(svc)) return fail(err, "truncated error PDU");
      if (!read_tag(r, t) || t.context || t.num != 9 || !r.uint_n(t.len, c1)) return fail(err, "bad error class");
      if (!read_tag(r, t) || t.context || t.num != 9 || !r.uint_n(t.len, c2)) return fail(err, "bad error code");
      cls = static_cast<std::uint8_t>(c1);
      code = static_cast<std::uint8_t>(c2);
      out = ErrorPdu{invoke, svc, cls, code};
      return true;
    }
    default:
      return fail(err, "unsupported PDU type");
  }
}

}  // namespace

std::optional<Message> decode(const std::uint8_t* data, std::size_t len, std::string* err) {
  Reader r(data, len);
  std::uint8_t type, func, version, control;
  std::uint16_t bvlc_len;
  if (!r.u8(type) || !r.u8(func) || !r.u16(bvlc_len)) {
    fail(err, "truncated BVLC header");
    return std::nullopt;
  }
  if (type != kBvlcType) {
    fail(err, "not a BACnet/IP frame");
    return std::nullopt;
  }
  if (bvlc_len != len) {
    fail(err, "BVLC length mismatch");
    return std::nullopt;
  }
  if (func == kBvlcForwarded) {
    if (!r.skip(6)) {  // originating B/IP address
      fail(err, "truncated forwarded NPDU");
      return std::nullopt;
    }
  } else if (func != kBvlcUnicast && func != kBvlcBroadcast) {
    fail(err, "unsupported BVLC function");
    return std::nullopt;
  }
  // NPDU
  if (!r.u8(version) || !r.u8(control) || version != 1) {
    fail(err, "bad NPDU header");
    return std::nullopt;
  }
  if (control & 0x80) {
    fail(err, "network-layer messages unsupported");
    return std::nullopt;
  }
  const bool has_dest = (control & 0x20) != 0, has_src = (control & 0x08) != 0;
  std::uint16_t net;
  std::uint8_t mac_len, hop;
  if (has_dest && (!r.u16(net) || !r.u8(mac_len) || !r.skip(mac_len))) {
    fail(err, "bad NPDU destination");
    return std::nullopt;
  }
  if (has_src && (!r.u16(net) || !r.u8(mac_len) || !r.skip(mac_len))) {
    fail(err, "bad NPDU source");
    return std::nullopt;
  }
  if (has_dest && !r.u8(hop)) {
    fail(err, "missing hop count");
    return std::nullopt;
  }
  Message msg;
  if (!decode_apdu(r, msg, err)) return std::nullopt;
  return msg;
}

std::string describe(const Message& msg) {
  std::ostringstream o;
  std::visit(Overloaded{
                 [&](const WhoIs&) { o << "Who-Is"; },
                 [&](const IAm& m) {
                   o << "I-Am device=" << m.device.instance << " max_apdu=" << m.max_apdu
                     << " vendor=" << m.vendor_id;
                 },
                 [&](const ReadPropertyRequest& m) {
                   o << "ReadProperty-Request invoke=" << int(m.invoke_id) << " object=" << m.object.type << ":"
                     << m.object.instance << " property=" << m.property;
                 },
                 [&](const ReadPropertyAck& m) {
                   o << "ReadProperty-Ack invoke=" << int(m.invoke_id) << " object=" << m.object.type << ":"
                     << m.object.instance << " value=" << m.value;
                 },
                 [&](const ErrorPdu& m) {
                   o << "Error invoke=" << int(m.invoke_id) << " class=" << int(m.error_class)
                     << " code=" << int(m.error_code);
                 },
             },
             msg);
  return o.str();
}

}  // namespace bacnet
