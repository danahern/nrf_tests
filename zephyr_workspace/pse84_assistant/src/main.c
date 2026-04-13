/*
 * PSE84 Voice Assistant — app main.
 *
 * Wires the three pieces together and stays out of their way:
 *   - state.c: IDLE / LISTENING / THINKING / RESPONDING state machine.
 *   - ui.c: LVGL scene with a procedural animation per state.
 *   - gpio_keys INPUT_KEY_0 (HW sw0, SDL SCANCODE_SPACE on native_sim)
 *     drives state_cycle() on each press edge.
 *
 * No #ifdef CONFIG_BOARD_* here — the native_sim vs. HW differences are
 * confined to the overlays + prj_*.conf files.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

#include "state.h"
#include "ui.h"

LOG_MODULE_REGISTER(pse84_assistant, LOG_LEVEL_INF);

static void button_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}
	/* evt->value: 1 = pressed, 0 = released. Cycle on press edge only. */
	if (evt->value == 1) {
		const assist_state_t next = state_cycle();

		LOG_INF("button press -> %s", state_name(next));
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);

int main(void)
{
	const struct device *display;
	int ret;

	LOG_INF("=== PSE84 Assistant (Phase 1) ===");

	display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		LOG_ERR("display device not ready");
		return -ENODEV;
	}

	state_init();
	ui_init();

	/* First flush before unblanking to avoid a frame of garbage. */
	ui_tick();
	ret = display_blanking_off(display);
	if (ret < 0 && ret != -ENOSYS) {
		LOG_ERR("display_blanking_off failed: %d", ret);
		return ret;
	}

	while (1) {
		ui_tick();
		k_sleep(K_MSEC(10));
	}

	return 0;
}
