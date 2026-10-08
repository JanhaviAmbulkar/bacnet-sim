// cli.hpp - tiny helpers for command-line parsing shared by the executables.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace cli {

inline bool parse_u32(const char* s, std::uint32_t& out) {
  if (!s || !*s || *s == '-') return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long v = std::strtoul(s, &end, 10);
  if (errno != 0 || *end != '\0' || v > 0xFFFFFFFFul) return false;
  out = static_cast<std::uint32_t>(v);
  return true;
}

inline bool parse_port(const char* s, std::uint16_t& out) {
  std::uint32_t v;
  if (!parse_u32(s, v) || v > 65535) return false;
  out = static_cast<std::uint16_t>(v);
  return true;
}

inline bool parse_float(const std::string& s, float& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || *end != '\0' || v != v) return false;  // rejects NaN and trailing junk
  out = static_cast<float>(v);
  return true;
}

}  // namespace cli
