/*
 * Minimal LZ4 block-format decoder — safe single-shot variant.
 *
 * Why vendor instead of using an upstream lz4.c? Two reasons:
 *   1. Zephyr's tree does not ship lz4 (verified 2026-04-12); the upstream
 *      lz4.c (~2700 lines) brings streaming + HC encoder we don't need.
 *   2. We only need decoder; host encoder is python lz4.block. Vendoring
 *      just the decoder at ~120 LOC keeps the audit surface small.
 *
 * Spec: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 *
 * Each LZ4 sequence is:
 *   - 1 token byte: high nibble = literal length run, low nibble = match len run (after +4)
 *   - optional extra literal-length bytes (0xff chains, terminated by <0xff)
 *   - `literal_len` raw literal bytes copied to dst
 *   - 2-byte little-endian match offset (may be absent for the last, literal-only sequence)
 *   - optional extra match-length bytes (0xff chains)
 *   - match copy of (match_len + 4) bytes from dst - offset
 *
 * Implementation intentionally uses byte-by-byte copies (no memcpy with
 * overlap tricks) to keep the code simple and auditable; throughput on
 * Cortex-M55 @ 400 MHz is still well under the 42 ms frame budget
 * (~0.7 ms for 768 KB per Phase 1b planning notes).
 */

#include "lz4_decompress.h"

#include <string.h>

int lz4_block_decompress(const uint8_t *src, size_t src_len,
                         uint8_t *dst, size_t dst_capacity)
{
	if (src == NULL || dst == NULL) {
		return -1;
	}
	const uint8_t *const src_end = src + src_len;
	uint8_t *const dst_start = dst;
	uint8_t *const dst_end = dst + dst_capacity;

	while (src < src_end) {
		/* Token. */
		const uint8_t token = *src++;
		size_t lit_len = token >> 4;
		size_t match_len = token & 0x0f;

		/* Extend literal length if 15. */
		if (lit_len == 15) {
			while (1) {
				if (src >= src_end) {
					return -1;
				}
				const uint8_t b = *src++;

				lit_len += b;
				if (b != 0xff) {
					break;
				}
			}
		}

		/* Copy literals. */
		if ((size_t)(src_end - src) < lit_len ||
		    (size_t)(dst_end - dst) < lit_len) {
			return -1;
		}
		if (lit_len != 0) {
			memcpy(dst, src, lit_len);
			src += lit_len;
			dst += lit_len;
		}

		/* Last sequence may end here if src consumed exactly. */
		if (src == src_end) {
			break;
		}

		/* Match offset (2-byte LE). */
		if ((size_t)(src_end - src) < 2) {
			return -1;
		}
		const uint16_t offset = (uint16_t)src[0] |
					((uint16_t)src[1] << 8);

		src += 2;
		if (offset == 0) {
			return -1;
		}

		/* Extend match length if 15. */
		if (match_len == 15) {
			while (1) {
				if (src >= src_end) {
					return -1;
				}
				const uint8_t b = *src++;

				match_len += b;
				if (b != 0xff) {
					break;
				}
			}
		}
		match_len += 4;  /* minmatch */

		/* Validate the match source is in-range. */
		if ((size_t)(dst - dst_start) < offset) {
			return -1;
		}
		if ((size_t)(dst_end - dst) < match_len) {
			return -1;
		}

		/* Copy match — byte-wise to handle overlap (RLE expand). */
		const uint8_t *match_src = dst - offset;

		for (size_t i = 0; i < match_len; i++) {
			dst[i] = match_src[i];
		}
		dst += match_len;
	}

	return (int)(dst - dst_start);
}
