/* bytebuf.h - tiny bounds-checked big-endian pack/unpack helpers (plain C).
 * Used by the C++ BACnet codec through extern "C". Every function returns the
 * new offset on success or BB_ERR if the buffer is too small, so callers can
 * chain calls and check once at the end. */
#ifndef BYTEBUF_H
#define BYTEBUF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BB_ERR ((size_t)-1)

size_t bb_put_u8(uint8_t *buf, size_t cap, size_t off, uint8_t v);
size_t bb_put_u16(uint8_t *buf, size_t cap, size_t off, uint16_t v);
size_t bb_put_u32(uint8_t *buf, size_t cap, size_t off, uint32_t v);
size_t bb_put_f32(uint8_t *buf, size_t cap, size_t off, float v);

size_t bb_get_u8(const uint8_t *buf, size_t len, size_t off, uint8_t *out);
size_t bb_get_u16(const uint8_t *buf, size_t len, size_t off, uint16_t *out);
size_t bb_get_u32(const uint8_t *buf, size_t len, size_t off, uint32_t *out);
size_t bb_get_f32(const uint8_t *buf, size_t len, size_t off, float *out);

/* Hex dump ("81 0b 00 0c ...") into out; always NUL-terminated; returns chars written. */
size_t bb_hex(const uint8_t *buf, size_t len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
