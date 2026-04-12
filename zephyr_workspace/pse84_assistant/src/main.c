/*
 * PSE84 Voice Assistant — Phase 0a scaffold.
 *
 * Minimum viable skeleton for the voice-assistant app:
 *   - LVGL on the in-tree GFXSS display_driver_api (no shim).
 *   - One centered label ("PSE84 Assistant" on boot).
 *   - sw0 / user_bt (gpio_prt8 pin 3) toggles the label text between
 *     "IDLE" and "PRESSED" via a Zephyr input_callback.
 *
 * No audio, BLE, IPC, or state machine yet — those are Phase 1+.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(pse84_assistant, LOG_LEVEL_INF);

/* UI state. Written only from the input callback (ISR context from gpio_keys'
 * deferred workqueue on most platforms, not a true ISR). Read from the main
 * loop under k_poll_signal_raise; atomic reads are sufficient for a single
 * bool here.
 */
static atomic_t pressed;
static lv_obj_t *status_label;

static void update_label_text(void)
{
	const bool is_pressed = (bool)atomic_get(&pressed);

	if (status_label == NULL) {
		return;
	}
	lv_label_set_text(status_label, is_pressed ? "PRESSED" : "IDLE");
}

static void button_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}
	/* evt->value: 1 = pressed, 0 = released. Toggle label on press only. */
	if (evt->value == 1) {
		(void)atomic_set(&pressed, !atomic_get(&pressed));
		LOG_INF("button press -> %s", atomic_get(&pressed) ? "PRESSED" : "IDLE");
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);

int main(void)
{
	const struct device *display;
	lv_obj_t *title_label;
	int ret;

	LOG_INF("=== PSE84 Assistant (Phase 0a) ===");

	display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		LOG_ERR("display device not ready");
		return -ENODEV;
	}

	/* Title on top, status in the middle. LVGL styles default to white-on-
	 * black once the framebuffer is initialised — good enough for a PoC.
	 */
	title_label = lv_label_create(lv_screen_active());
	lv_label_set_text(title_label, "PSE84 Assistant");
	lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 40);

	status_label = lv_label_create(lv_screen_active());
	lv_label_set_text(status_label, "IDLE");
	lv_obj_set_style_text_font(status_label, &lv_font_montserrat_28, 0);
	lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 0);

	/* First flush before unblanking to avoid a frame of garbage. */
	lv_timer_handler();
	ret = display_blanking_off(display);
	if (ret < 0 && ret != -ENOSYS) {
		LOG_ERR("display_blanking_off failed: %d", ret);
		return ret;
	}

	while (1) {
		update_label_text();
		lv_timer_handler();
		k_sleep(K_MSEC(10));
	}

	return 0;
}
