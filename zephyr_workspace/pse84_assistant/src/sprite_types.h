/*
 * Types shared between src/sprites.[ch] and the generated
 * assets/<state>/frames.c files. Kept separate so the generator doesn't
 * need to know about LVGL (sprites.h drags in lvgl.h).
 */

#ifndef PSE84_ASSISTANT_SPRITE_TYPES_H_
#define PSE84_ASSISTANT_SPRITE_TYPES_H_

#include <stdint.h>

struct sprite_frame_rec {
	uint32_t lz4_off;   /* offset into the per-state lz4_blob */
	uint32_t lz4_len;   /* compressed bytes for this frame */
	uint32_t raw_len;   /* uncompressed bytes (== W*H*2) */
};

#endif /* PSE84_ASSISTANT_SPRITE_TYPES_H_ */
