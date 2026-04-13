/*
 * Sprite playback.
 *
 * Design note: LVGL v9 lets you register a custom image decoder, but the
 * use case here is simpler than streaming decoders — we own the sheets,
 * the format is fixed, and only one sheet plays at a time. A single
 * scratch framebuffer + periodic decompress into it (triggered from an
 * lv_timer) is ~80 LOC and dodges the decoder cache interactions
 * entirely. lv_image_set_src(img, &dsc) points LVGL at the same
 * descriptor each frame; lv_obj_invalidate() + lv_image_cache_drop()
 * force a re-read so the updated pixel bytes take effect.
 *
 * Memory: one scratch buffer sized W*H*2. At tier A (800x480) that's
 * 768 KB — allocated via lv_malloc (LV_MEM_SIZE must be large enough;
 * prj.conf bumps CONFIG_LV_Z_MEM_POOL_SIZE accordingly on HW).
 *
 * Timing: k_cycle_get_32() brackets each lz4_block_decompress() so the
 * serial monitor can verify the M55 budget. k_cyc_to_us_floor32() turns
 * that into microseconds. Logged once per state change, then every 48
 * frames (~2 s at 24 fps) to keep UART quiet.
 */

#include "sprites.h"

#ifdef CONFIG_APP_ANIMATION_SPRITES

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/devicetree.h>

#include "lz4_decompress.h"

/* Not pulled in by <lvgl.h>'s public umbrella; declared in internal header. */
void lv_image_cache_drop(const void *src);

/* On the PSE84 kit, on-chip SRAM (256 KB) is too small for the 768 KB
 * scratch framebuffer. The overlay defines a 1 MiB SOCMEM region
 * (socmem_sprite) we point a raw pointer at — the M55 linker doesn't
 * own SOCMEM, so Z_GENERIC_SECTION would emit a load-time image filler
 * we don't want. For other platforms (native_sim, QEMU) the 768 KB
 * allocation comes from the LVGL heap via lv_malloc.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(socmem_sprite))
#define SPRITES_SCRATCH_ADDR ((void *)DT_REG_ADDR(DT_NODELABEL(socmem_sprite)))
#define SPRITES_SCRATCH_SIZE DT_REG_SIZE(DT_NODELABEL(socmem_sprite))
#else
#define SPRITES_SCRATCH_ADDR NULL
#define SPRITES_SCRATCH_SIZE 0
#endif

LOG_MODULE_REGISTER(sprites, LOG_LEVEL_INF);

#define SPRITE_MAX_W 800
#define SPRITE_MAX_H 480
#define SPRITE_BUF_BYTES (SPRITE_MAX_W * SPRITE_MAX_H * 2)

struct sprites_ctx {
	lv_obj_t *img;                 /* the on-screen lv_image */
	lv_image_dsc_t dsc;            /* re-used descriptor pointed at scratch */
	uint8_t *scratch;              /* W*H*2 scratch framebuffer */
	size_t scratch_cap;            /* bytes allocated */

	const struct sprite_sheet *sheet;
	uint32_t frame_idx;
	lv_timer_t *timer;

	struct sprite_stats stats;
	uint32_t log_stride; /* log once every N frames */
	uint32_t log_ctr;
};

static struct sprites_ctx ctx;

static int decode_frame(uint32_t idx)
{
	const struct sprite_sheet *s = ctx.sheet;

	if (s == NULL || idx >= s->n_frames) {
		return -EINVAL;
	}
	const struct sprite_frame_rec *r = &s->frames[idx];

	if (r->raw_len > ctx.scratch_cap) {
		LOG_ERR("frame %u raw_len=%u exceeds scratch %zu",
			idx, r->raw_len, ctx.scratch_cap);
		return -ENOMEM;
	}
	const uint32_t t0 = k_cycle_get_32();
	const int produced = lz4_block_decompress(s->lz4_blob + r->lz4_off,
						  r->lz4_len,
						  ctx.scratch,
						  ctx.scratch_cap);
	const uint32_t dt = k_cycle_get_32() - t0;

	if (produced != (int)r->raw_len) {
		LOG_ERR("frame %u decompress failed: got %d expected %u",
			idx, produced, r->raw_len);
		return -EIO;
	}

	ctx.stats.frames_decoded++;
	ctx.stats.last_decomp_cycles = dt;
	ctx.stats.total_decomp_cycles += dt;

	if (ctx.log_stride != 0) {
		ctx.log_ctr++;
		if (ctx.log_ctr == 1U || ctx.log_ctr % ctx.log_stride == 0U) {
			const uint32_t us = k_cyc_to_us_floor32(dt);
			const uint32_t avg_us = k_cyc_to_us_floor32(
				ctx.stats.total_decomp_cycles /
				MAX(1U, ctx.stats.frames_decoded));

			LOG_INF("sprite %s frame %u/%u decomp=%u us avg=%u us",
				s->name, idx, s->n_frames, us, avg_us);
		}
	}
	return 0;
}

static void publish_current_frame(void)
{
	/* Drop any cached decode of this dsc so the freshly written bytes
	 * get picked up on next draw.
	 */
	lv_image_cache_drop(&ctx.dsc);
	lv_image_set_src(ctx.img, &ctx.dsc);
	lv_obj_invalidate(ctx.img);
}

static void frame_timer_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);
	if (ctx.sheet == NULL) {
		return;
	}
	ctx.frame_idx = (ctx.frame_idx + 1U) % ctx.sheet->n_frames;
	if (decode_frame(ctx.frame_idx) == 0) {
		publish_current_frame();
	}
}

int sprites_init(lv_obj_t *parent)
{
	if (ctx.img != NULL) {
		return 0;
	}
	if (parent == NULL) {
		return -EINVAL;
	}

	if (SPRITES_SCRATCH_ADDR != NULL && SPRITES_SCRATCH_SIZE >= SPRITE_BUF_BYTES) {
		ctx.scratch = SPRITES_SCRATCH_ADDR;
		ctx.scratch_cap = SPRITE_BUF_BYTES;
	} else {
		ctx.scratch = lv_malloc(SPRITE_BUF_BYTES);
		if (ctx.scratch == NULL) {
			LOG_ERR("sprites: lv_malloc(%d) for scratch failed",
				SPRITE_BUF_BYTES);
			return -ENOMEM;
		}
		ctx.scratch_cap = SPRITE_BUF_BYTES;
	}
	memset(ctx.scratch, 0, ctx.scratch_cap);

	ctx.img = lv_image_create(parent);
	lv_obj_align(ctx.img, LV_ALIGN_CENTER, 0, 0);

	/* Pre-fill a sensible header; w/h get overwritten per sheet. */
	memset(&ctx.dsc, 0, sizeof(ctx.dsc));
	ctx.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	ctx.dsc.header.w = SPRITE_MAX_W;
	ctx.dsc.header.h = SPRITE_MAX_H;
	ctx.dsc.header.stride = SPRITE_MAX_W * 2;
	ctx.dsc.data = ctx.scratch;
	ctx.dsc.data_size = ctx.scratch_cap;

	ctx.log_stride = 48;
	LOG_INF("sprites_init: scratch=%zu KiB img=%p", ctx.scratch_cap / 1024,
		(void *)ctx.img);
	return 0;
}

void sprites_stop(void)
{
	if (ctx.timer != NULL) {
		lv_timer_del(ctx.timer);
		ctx.timer = NULL;
	}
	ctx.sheet = NULL;
}

void sprites_play(const struct sprite_sheet *sheet)
{
	sprites_stop();

	if (ctx.img == NULL) {
		LOG_ERR("sprites_play before sprites_init");
		return;
	}
	if (sheet == NULL) {
		lv_image_set_src(ctx.img, NULL);
		lv_obj_invalidate(ctx.img);
		return;
	}
	if ((size_t)sheet->width * sheet->height * 2U > ctx.scratch_cap) {
		LOG_ERR("sheet %s %ux%u exceeds scratch %zu",
			sheet->name, sheet->width, sheet->height, ctx.scratch_cap);
		return;
	}

	ctx.sheet = sheet;
	ctx.frame_idx = 0;
	ctx.log_ctr = 0;

	ctx.dsc.header.w = sheet->width;
	ctx.dsc.header.h = sheet->height;
	ctx.dsc.header.stride = sheet->width * 2;
	ctx.dsc.data_size = (uint32_t)sheet->width * sheet->height * 2U;

	if (decode_frame(0) != 0) {
		LOG_ERR("sprites_play: initial decode failed");
		ctx.sheet = NULL;
		return;
	}
	publish_current_frame();

	/* Apply per-sheet render zoom (tier B uses 512 = 2x). */
	if (sheet->zoom != 0 && sheet->zoom != LV_SCALE_NONE) {
		lv_image_set_scale(ctx.img, sheet->zoom);
	} else {
		lv_image_set_scale(ctx.img, LV_SCALE_NONE);
	}

	const uint32_t period_ms = sheet->fps ? (1000U / sheet->fps) : 42U;

	ctx.timer = lv_timer_create(frame_timer_cb, period_ms, NULL);
	LOG_INF("sprites_play: %s %ux%u@%u fps (%u frames, period=%u ms)",
		sheet->name, sheet->width, sheet->height, sheet->fps,
		sheet->n_frames, period_ms);
}

void sprites_get_stats(struct sprite_stats *out)
{
	if (out != NULL) {
		*out = ctx.stats;
	}
}

/* ---- sprite_sheet descriptors referencing per-state asset blobs.
 *
 * These name-mangle the extern symbols emitted by the python pipeline
 * into assets/<state>/frames.c. See tools/video_to_sprites.py for the
 * (source-mp4 -> state) mapping.
 */

#include "../assets/idle/frames.h"
#include "../assets/listening/frames.h"
#include "../assets/thinking/frames.h"
#include "../assets/responding/frames.h"

#if CONFIG_APP_ANIMATION_SPRITES_TIER_B
#define SHEET_ZOOM 512
#else
#define SHEET_ZOOM LV_SCALE_NONE
#endif

const struct sprite_sheet sprite_sheet_idle = {
	.name = "idle",
	.lz4_blob = idle_lz4_blob,
	.frames = idle_frames,
	.n_frames = IDLE_FRAMES,
	.width = IDLE_WIDTH,
	.height = IDLE_HEIGHT,
	.fps = IDLE_FPS,
	.zoom = SHEET_ZOOM,
};
const struct sprite_sheet sprite_sheet_listening = {
	.name = "listening",
	.lz4_blob = listening_lz4_blob,
	.frames = listening_frames,
	.n_frames = LISTENING_FRAMES,
	.width = LISTENING_WIDTH,
	.height = LISTENING_HEIGHT,
	.fps = LISTENING_FPS,
	.zoom = SHEET_ZOOM,
};
const struct sprite_sheet sprite_sheet_thinking = {
	.name = "thinking",
	.lz4_blob = thinking_lz4_blob,
	.frames = thinking_frames,
	.n_frames = THINKING_FRAMES,
	.width = THINKING_WIDTH,
	.height = THINKING_HEIGHT,
	.fps = THINKING_FPS,
	.zoom = SHEET_ZOOM,
};
const struct sprite_sheet sprite_sheet_responding = {
	.name = "responding",
	.lz4_blob = responding_lz4_blob,
	.frames = responding_frames,
	.n_frames = RESPONDING_FRAMES,
	.width = RESPONDING_WIDTH,
	.height = RESPONDING_HEIGHT,
	.fps = RESPONDING_FPS,
	.zoom = SHEET_ZOOM,
};

#endif /* CONFIG_APP_ANIMATION_SPRITES */
