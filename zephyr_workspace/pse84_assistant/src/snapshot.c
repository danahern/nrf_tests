/*
 * PSE84 Voice Assistant — LVGL → PPM snapshot helper (native_sim).
 *
 * See snapshot.h for the why. This file is only compiled when
 * CONFIG_APP_SNAPSHOT=y, which is only selected by
 * `prj_native_sim_snapshot.conf`. It relies on:
 *   - LVGL's lv_snapshot_take() (CONFIG_LV_USE_SNAPSHOT=y) to rasterise
 *     the active screen into an RGBA8888 draw buffer.
 *   - nsi_host_{open,write,close}() trampolines to reach the host
 *     filesystem from the Zephyr/POSIX-arch embedded side.
 *
 * RGBA8888 from LVGL is emitted as B, G, R, A per pixel on
 * little-endian hosts (see lv_color_format.h: pixels are packed as
 * 0xAARRGGBB in a 32-bit word, which on LE serialises to B, G, R, A).
 * PPM P6 wants R, G, B per pixel, so we reorder while we stream.
 */

#ifdef CONFIG_APP_SNAPSHOT

#include "snapshot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>
#include <nsi_host_trampolines.h>

LOG_MODULE_REGISTER(assist_snapshot, LOG_LEVEL_INF);

/* Write exactly `len` bytes or return -EIO. nsi_host_write() is a thin
 * wrapper over write(2), so short writes are possible in principle.
 */
static int write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t remaining = len;

	while (remaining > 0) {
		const long n = nsi_host_write(fd, p, remaining);

		if (n <= 0) {
			return -EIO;
		}
		p += (size_t)n;
		remaining -= (size_t)n;
	}
	return 0;
}

int app_snapshot_save_ppm(const char *path)
{
	if (path == NULL) {
		return -EINVAL;
	}

	lv_obj_t *scr = lv_screen_active();

	if (scr == NULL) {
		LOG_ERR("no active screen");
		return -ENODEV;
	}

	/* Ensure layout + any pending draws are resolved before snapshot. */
	lv_obj_update_layout(scr);
	(void)lv_timer_handler();

	lv_draw_buf_t *snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_ARGB8888);

	if (snap == NULL) {
		LOG_ERR("lv_snapshot_take failed");
		return -ENOMEM;
	}

	const uint32_t w = snap->header.w;
	const uint32_t h = snap->header.h;
	const uint32_t stride = snap->header.stride;
	const uint8_t *pixels = snap->data;

	/* Header (P6 magic / width / height / maxval). */
	char hdr[64];
	const int hdr_len = snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", w, h);

	if (hdr_len < 0 || hdr_len >= (int)sizeof(hdr)) {
		lv_draw_buf_destroy(snap);
		return -EINVAL;
	}

	const int fd = nsi_host_open(path, O_WRONLY | O_TRUNC);

	if (fd < 0) {
		LOG_ERR("open(%s) failed", path);
		lv_draw_buf_destroy(snap);
		return -EIO;
	}

	int ret = write_all(fd, hdr, (size_t)hdr_len);

	if (ret != 0) {
		goto out;
	}

	/* Stream one row at a time, repacking BGRA → RGB into a scratch buf.
	 * 1024 px × 3 B = 3 KB; native_sim has plenty of stack.
	 */
	uint8_t row[1024 * 3];

	if (w > 1024) {
		LOG_ERR("snapshot width %u exceeds scratch row buffer", w);
		ret = -EINVAL;
		goto out;
	}

	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *src = pixels + (size_t)y * stride;

		for (uint32_t x = 0; x < w; x++) {
			/* LVGL ARGB8888 on little-endian: B,G,R,A in memory. */
			row[x * 3 + 0] = src[x * 4 + 2]; /* R */
			row[x * 3 + 1] = src[x * 4 + 1]; /* G */
			row[x * 3 + 2] = src[x * 4 + 0]; /* B */
		}
		ret = write_all(fd, row, (size_t)w * 3U);
		if (ret != 0) {
			goto out;
		}
	}

out:
	(void)nsi_host_close(fd);
	lv_draw_buf_destroy(snap);

	if (ret == 0) {
		LOG_INF("snapshot saved: %s (%ux%u)", path, w, h);
	}
	return ret;
}

#endif /* CONFIG_APP_SNAPSHOT */
