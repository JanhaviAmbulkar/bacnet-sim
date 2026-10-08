// bacnet.hpp - codec for a subset of BACnet/IP (ASHRAE 135 Annex J):
//   Who-Is, I-Am, ReadProperty (request / ack) and Error PDUs.
// Messages are a std::variant; encode()/decode() are pure functions so they are
// trivial to unit-test and fuzz.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace bacnet {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kDefaultPort = 47808;  // 0xBAC0
constexpr std::size_t kMaxFrame = 1500;
constexpr std::uint16_t kObjAnalogInput = 0;
constexpr std::uint16_t kObjDevice = 8;
constexpr std::uint32_t kPropPresentValue = 85;
constexpr std::uint8_t kSegmentationNone = 3;

struct ObjectId {
  std::uint16_t type = 0;
  std::uint32_t instance = 0;
  std::uint32_t raw() const { return (std::uint32_t(type) << 22) | (instance & 0x3FFFFF); }
  static ObjectId from_raw(std::uint32_t r) {
    return ObjectId{static_cast<std::uint16_t>(r >> 22), r & 0x3FFFFF};
  }
  bool operator==(const ObjectId& o) const { return type == o.type && instance == o.instance; }
};

struct WhoIs {};
struct IAm {
  ObjectId device;
  std::uint16_t max_apdu = 1476;
  std::uint8_t segmentation = kSegmentationNone;
  std::uint16_t vendor_id = 0;
};
struct ReadPropertyRequest {
  std::uint8_t invoke_id = 0;
  ObjectId object;
  std::uint32_t property = kPropPresentValue;
};
struct ReadPropertyAck {
  std::uint8_t invoke_id = 0;
  ObjectId object;
  std::uint32_t property = kPropPresentValue;
  float value = 0.0f;
};
struct ErrorPdu {
  std::uint8_t invoke_id = 0;
  std::uint8_t service = 12;  // confirmed service choice (12 = ReadProperty)
  std::uint8_t error_class = 0;
  std::uint8_t error_code = 0;
};

using Message = std::variant<WhoIs, IAm, ReadPropertyRequest, ReadPropertyAck, ErrorPdu>;

// Encodes a complete BVLC+NPDU+APDU frame. `broadcast` selects the BVLC
// "Original-Broadcast-NPDU" function and a global-broadcast NPDU destination.
// Returns an empty vector if the message does not fit in kMaxFrame.
Bytes encode(const Message& msg, bool broadcast = false);

// Decodes a frame. Returns nullopt (and fills *err) on malformed/unsupported input.
// Never reads out of bounds and never throws for any byte sequence.
std::optional<Message> decode(const std::uint8_t* data, std::size_t len, std::string* err = nullptr);

std::string describe(const Message& msg);

}  // namespace bacnet
