#include "bytebuf.h"

#include <string.h>

static int room(size_t total, size_t off, size_t need) {
  return off <= total && total - off >= need;
}

size_t bb_put_u8(uint8_t *buf, size_t cap, size_t off, uint8_t v) {
  if (off == BB_ERR || !room(cap, off, 1)) return BB_ERR;
  buf[off] = v;
  return off + 1;
}

size_t bb_put_u16(uint8_t *buf, size_t cap, size_t off, uint16_t v) {
  if (off == BB_ERR || !room(cap, off, 2)) return BB_ERR;
  buf[off] = (uint8_t)(v >> 8);
  buf[off + 1] = (uint8_t)v;
  return off + 2;
}

size_t bb_put_u32(uint8_t *buf, size_t cap, size_t off, uint32_t v) {
  if (off == BB_ERR || !room(cap, off, 4)) return BB_ERR;
  buf[off] = (uint8_t)(v >> 24);
  buf[off + 1] = (uint8_t)(v >> 16);
  buf[off + 2] = (uint8_t)(v >> 8);
  buf[off + 3] = (uint8_t)v;
  return off + 4;
}

size_t bb_put_f32(uint8_t *buf, size_t cap, size_t off, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits); /* IEEE-754 single, then written big-endian */
  return bb_put_u32(buf, cap, off, bits);
}

size_t bb_get_u8(const uint8_t *buf, size_t len, size_t off, uint8_t *out) {
  if (off == BB_ERR || !room(len, off, 1)) return BB_ERR;
  *out = buf[off];
  return off + 1;
}

size_t bb_get_u16(const uint8_t *buf, size_t len, size_t off, uint16_t *out) {
  if (off == BB_ERR || !room(len, off, 2)) return BB_ERR;
  *out = (uint16_t)((buf[off] << 8) | buf[off + 1]);
  return off + 2;
}

size_t bb_get_u32(const uint8_t *buf, size_t len, size_t off, uint32_t *out) {
  if (off == BB_ERR || !room(len, off, 4)) return BB_ERR;
  *out = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
         ((uint32_t)buf[off + 2] << 8) | (uint32_t)buf[off + 3];
  return off + 4;
}

size_t bb_get_f32(const uint8_t *buf, size_t len, size_t off, float *out) {
  uint32_t bits;
  size_t n = bb_get_u32(buf, len, off, &bits);
  if (n == BB_ERR) return BB_ERR;
  memcpy(out, &bits, sizeof bits);
  return n;
}

size_t bb_hex(const uint8_t *buf, size_t len, char *out, size_t cap) {
  static const char digits[] = "0123456789abcdef";
  size_t w = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; i < len && w + 3 < cap; i++) {
    if (i) out[w++] = ' ';
    out[w++] = digits[buf[i] >> 4];
    out[w++] = digits[buf[i] & 0x0F];
  }
  out[w] = '\0';
  return w;
}
