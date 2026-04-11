/*
 * Thin app driving the PSE84 GFXSS Zephyr display driver.
 * All the GFXSS / panel bring-up lives in the driver now — this file
 * just grabs the display device and writes 4 vertical color bars one
 * row at a time (the full FB is ~780 KB which won't fit in M55 SRAM,
 * but a single row is tiny).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define FB_WIDTH  832
#define FB_HEIGHT 480

static uint16_t row_buf[FB_WIDTH];

int main(void)
{
	const struct device *display;
	struct display_capabilities caps;
	struct display_buffer_descriptor desc;
	int ret;

	printk("=== PSE84 display test (Zephyr driver) ===\n");

	display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		printk("display device not ready\n");
		return -ENODEV;
	}

	display_get_capabilities(display, &caps);
	printk("display: %ux%u, pixel formats 0x%x, current 0x%x\n",
	       caps.x_resolution, caps.y_resolution, caps.supported_pixel_formats,
	       caps.current_pixel_format);

	/* Build 1 row of 4 vertical color bars in RGB565 — same for every line. */
	static const uint16_t palette[4] = {
		0xF800, /* RED   */
		0x07E0, /* GREEN */
		0x001F, /* BLUE  */
		0xFFFF, /* WHITE */
	};
	for (int col = 0; col < FB_WIDTH; col++) {
		row_buf[col] = palette[(col * 4) / FB_WIDTH];
	}

	desc.buf_size = sizeof(row_buf);
	desc.width = FB_WIDTH;
	desc.height = 1;
	desc.pitch = FB_WIDTH;

	for (int row = 0; row < FB_HEIGHT; row++) {
		ret = display_write(display, 0, row, &desc, row_buf);
		if (ret < 0) {
			printk("display_write row=%d failed: %d\n", row, ret);
			return ret;
		}
	}
	printk("display_write OK — should show 4 color bars\n");

	while (1) {
		k_msleep(1000);
	}
	return 0;
}
