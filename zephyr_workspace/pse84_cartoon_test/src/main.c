/*
 * PSE84 GFXSS video playback demo.
 *
 * Plays a 125-frame RGB565 animation (240x144 source, 3x pixel-doubled
 * to 720x432 on the 800x480 panel, centered with a 40/24 px black
 * border) at the native 24 fps. The raw frame blob is embedded as a
 * const in flash via generate_inc_file_for_target() — see CMakeLists.txt.
 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define SRC_W       240U
#define SRC_H       144U
#define NUM_FRAMES  125U
#define FRAME_BYTES (SRC_W * SRC_H * 2U)

#define UPSCALE     3U
#define DST_W       (SRC_W * UPSCALE) /* 720 */
#define DST_H       (SRC_H * UPSCALE) /* 432 */

#define PANEL_W     800U
#define PANEL_H     480U
#define DST_X       ((PANEL_W - DST_W) / 2U) /* 40 */
#define DST_Y       ((PANEL_H - DST_H) / 2U) /* 24 */

#define FRAME_PERIOD_MS 42U /* ~24 fps (1000 / 24 = 41.67) */

static const uint8_t frames_blob[] = {
#include "frames.bin.inc"
};

BUILD_ASSERT(sizeof(frames_blob) == NUM_FRAMES * FRAME_BYTES,
	     "frames.bin blob size does not match NUM_FRAMES * FRAME_BYTES");

/* Upscaled destination row: DST_W pixels, UPSCALE copies stacked. */
static uint16_t dst_row_buf[DST_W * UPSCALE];

/* Black border fill buffer: one row of PANEL_W zeros. */
static uint16_t border_row[PANEL_W];

static void draw_border(const struct device *display)
{
	struct display_buffer_descriptor desc = {
		.width = PANEL_W,
		.height = 1,
		.pitch = PANEL_W,
		.buf_size = sizeof(border_row),
	};
	uint16_t y;

	memset(border_row, 0, sizeof(border_row));

	/* Top bar: rows [0, DST_Y). */
	for (y = 0; y < DST_Y; y++) {
		(void)display_write(display, 0, y, &desc, border_row);
	}
	/* Bottom bar: rows [DST_Y + DST_H, PANEL_H). */
	for (y = DST_Y + DST_H; y < PANEL_H; y++) {
		(void)display_write(display, 0, y, &desc, border_row);
	}
	/* Left bar: one-row writes at each y in [DST_Y, DST_Y + DST_H) for
	 * columns [0, DST_X).
	 */
	desc.width = DST_X;
	desc.pitch = DST_X;
	desc.buf_size = DST_X * 2U;
	for (y = DST_Y; y < DST_Y + DST_H; y++) {
		(void)display_write(display, 0, y, &desc, border_row);
		(void)display_write(display, DST_X + DST_W, y, &desc, border_row);
	}
}

static void draw_frame(const struct device *display, const uint8_t *frame)
{
	const uint16_t *src = (const uint16_t *)frame;
	struct display_buffer_descriptor desc = {
		.width = DST_W,
		.height = UPSCALE,
		.pitch = DST_W,
		.buf_size = sizeof(dst_row_buf),
	};
	uint32_t sy, sx;

	for (sy = 0; sy < SRC_H; sy++) {
		const uint16_t *src_row = &src[sy * SRC_W];
		uint16_t *out = dst_row_buf;

		/* Horizontal 3x expand: each source pixel -> 3 dest pixels. */
		for (sx = 0; sx < SRC_W; sx++) {
			uint16_t p = src_row[sx];

			out[0] = p;
			out[1] = p;
			out[2] = p;
			out += UPSCALE;
		}
		/* Vertical 3x expand: replicate the row twice more. */
		memcpy(&dst_row_buf[DST_W], &dst_row_buf[0], DST_W * 2U);
		memcpy(&dst_row_buf[DST_W * 2U], &dst_row_buf[0], DST_W * 2U);

		(void)display_write(display, DST_X, DST_Y + sy * UPSCALE, &desc,
				    dst_row_buf);
	}
}

int main(void)
{
	const struct device *display;
	struct display_capabilities caps;
	uint32_t frame_idx = 0;
	int64_t next_tick;

	printk("=== PSE84 video playback (240x144 -> 720x432 @ 24 fps) ===\n");
	printk("blob: %u frames x %u bytes = %u bytes\n",
	       NUM_FRAMES, (unsigned int)FRAME_BYTES,
	       (unsigned int)sizeof(frames_blob));

	display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		printk("display device not ready\n");
		return -ENODEV;
	}

	display_get_capabilities(display, &caps);
	printk("display: %ux%u fmt 0x%x\n",
	       caps.x_resolution, caps.y_resolution, caps.current_pixel_format);

	draw_border(display);

	next_tick = k_uptime_get();
	while (1) {
		const uint8_t *frame = &frames_blob[frame_idx * FRAME_BYTES];

		draw_frame(display, frame);

		frame_idx++;
		if (frame_idx >= NUM_FRAMES) {
			frame_idx = 0;
		}

		next_tick += FRAME_PERIOD_MS;
		int64_t now = k_uptime_get();
		int64_t sleep_ms = next_tick - now;

		if (sleep_ms > 0) {
			k_msleep((uint32_t)sleep_ms);
		} else {
			/* We're behind schedule; resync to now. */
			next_tick = now;
		}
	}

	return 0;
}
