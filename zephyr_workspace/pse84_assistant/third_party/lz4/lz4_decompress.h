/*
 * Minimal LZ4 block-format decoder (header).
 *
 * Compatible byte-for-byte with lz4.block.compress(store_size=False)
 * output from the host pipeline (tools/video_to_sprites.py). Only
 * decompression is implemented — encoding lives on the macOS host.
 *
 * Public API is a one-shot decoder: caller provides src/src_len and
 * dst/dst_capacity; returns produced bytes on success, -1 on malformed
 * input or insufficient dst capacity.
 *
 * Not thread-safe by itself but has no internal state; concurrent calls
 * with disjoint buffers are fine.
 */

#ifndef PSE84_ASSISTANT_LZ4_DECOMPRESS_H_
#define PSE84_ASSISTANT_LZ4_DECOMPRESS_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decompress LZ4 block-format payload. Returns >=0 byte count, -1 on error. */
int lz4_block_decompress(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_capacity);

#ifdef __cplusplus
}
#endif

#endif /* PSE84_ASSISTANT_LZ4_DECOMPRESS_H_ */
