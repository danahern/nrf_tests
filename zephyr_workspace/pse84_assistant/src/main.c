/*
 * PSE84 Voice Assistant — app main.
 *
 * Wires the three pieces together and stays out of their way:
 *   - state.c: IDLE / LISTENING / THINKING / RESPONDING state machine.
 *   - ui.c: LVGL scene with a procedural animation per state.
 *   - gpio_keys INPUT_KEY_0 (HW sw0, SDL SCANCODE_SPACE on native_sim)
 *     drives state_cycle() on each press edge.
 *
 * Two build flavours share this file:
 *   - CONFIG_LVGL=y (HW, native_sim): full LVGL + display + gpio_keys
 *     input path.
 *   - CONFIG_LVGL=n (mps3/corstone300/an547 QEMU smoke test): headless
 *     build; a k_timer periodically drives state_cycle() so the state
 *     machine + Opus/framing code paths still execute on a real
 *     Cortex-M55 runtime without any display or input hardware.
 *
 * Everything board-specific lives in the overlays + prj_*.conf files.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "state.h"

#ifdef CONFIG_AUDIO_DMIC
#include "audio.h"
#endif

#include "link.h"

#ifdef CONFIG_LVGL
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <lvgl.h>

#include "ui.h"
#endif /* CONFIG_LVGL */

#ifdef CONFIG_APP_SNAPSHOT
#include <nsi_main.h>

#include "snapshot.h"
#endif

LOG_MODULE_REGISTER(pse84_assistant, LOG_LEVEL_INF);

#ifdef CONFIG_LVGL

#ifdef CONFIG_AUDIO_DMIC
/* Auto-stop timer: enforces the 2 s capture cap if the user holds the
 * button longer. Runs from ISR context; defers the actual stop to the
 * system workqueue so audio_capture_stop() (which does a bulk printk
 * hex dump) executes in a thread.
 */
static void audio_autostop_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (audio_is_capturing()) {
		LOG_INF("audio cap at %d ms — auto-stopping", AUDIO_MAX_DURATION_MS);
		(void)audio_capture_stop();
	}
}

static K_WORK_DEFINE(audio_autostop_work, audio_autostop_work_handler);

static void audio_autostop_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&audio_autostop_work);
}

static K_TIMER_DEFINE(audio_autostop_timer, audio_autostop_timer_cb, NULL);

/* Same deferred pattern for the stop-on-release path, because the input
 * callback runs in the input subsystem's thread and audio_capture_stop()
 * does a 60 KB printk dump we don't want to block that thread on.
 */
static void audio_stop_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	k_timer_stop(&audio_autostop_timer);
	(void)audio_capture_stop();
}

static K_WORK_DEFINE(audio_stop_work, audio_stop_work_handler);
#endif /* CONFIG_AUDIO_DMIC */

static void button_input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->code != INPUT_KEY_0) {
		return;
	}
	/* evt->value: 1 = pressed, 0 = released.
	 *
	 * Press edge: cycle the visual state machine AND (if audio is
	 * compiled in) arm a PDM capture. The state machine keeps its
	 * existing Phase 1 round-robin — audio capture is independent
	 * per Phase 2 scope.
	 *
	 * Release edge: stop + dump the capture.
	 */
	if (evt->value == 1) {
		/* Press -> LISTENING: show the thinking-pose animation while
		 * we capture, arm the 2 s auto-stop safety timer. The state
		 * machine no longer auto-cycles through states — transitions
		 * are driven by real events: press/release and incoming
		 * TEXT_CHUNK/TEXT_END frames from the host bridge.
		 *
		 * Entry side-effect: wipe any reply text from the previous
		 * query so the next RESPONDING starts fresh.
		 */
		ui_clear_reply_text();
		link_cancel_idle_revert();
		state_set(ASSIST_LISTENING);
		LOG_INF("button press -> LISTENING");
#ifdef CONFIG_AUDIO_DMIC
		if (audio_capture_start() == 0) {
			k_timer_start(&audio_autostop_timer,
				      K_MSEC(AUDIO_MAX_DURATION_MS), K_NO_WAIT);
		}
#endif
	} else if (evt->value == 0) {
		LOG_INF("button release (capturing=%d)",
			(int)
#ifdef CONFIG_AUDIO_DMIC
			audio_is_capturing()
#else
			0
#endif
			);
#ifdef CONFIG_AUDIO_DMIC
		if (audio_is_capturing()) {
			k_work_submit(&audio_stop_work);
		}
#endif
		/* Release -> THINKING: audio capture just ended, host is
		 * about to transcribe + query the LLM. Stay here until the
		 * first TEXT_CHUNK arrives (link.c kicks us to RESPONDING)
		 * or the user presses again (back to LISTENING).
		 */
		if (state_get() == ASSIST_LISTENING) {
			state_set(ASSIST_THINKING);
		}
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);
#else  /* !CONFIG_LVGL — headless QEMU smoke test */

/* 1 s is slow enough to be readable on the QEMU UART and fast enough
 * that `west build -t run` sees all four states inside a short run.
 */
#define HEADLESS_CYCLE_PERIOD_MS 1000

static void headless_cycle_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	const assist_state_t next = state_cycle();

	LOG_INF("headless tick -> %s", state_name(next));
}

static K_WORK_DEFINE(headless_cycle_work, headless_cycle_work_handler);

static void headless_cycle_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	/* k_timer callbacks run in ISR context; state transitions touch
	 * LOG_INF which wants a thread context. Defer to the system
	 * workqueue.
	 */
	k_work_submit(&headless_cycle_work);
}

static K_TIMER_DEFINE(headless_cycle_timer, headless_cycle_timer_cb, NULL);
#endif /* CONFIG_LVGL */

int main(void)
{
	LOG_INF("=== PSE84 Assistant (Phase 1) ===");

	state_init();

#ifdef CONFIG_LVGL
	const struct device *display;
	int ret;

	display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		LOG_ERR("display device not ready");
		return -ENODEV;
	}

	ui_init();

#ifdef CONFIG_AUDIO_DMIC
	if (audio_init() != 0) {
		LOG_ERR("audio_init failed; PDM capture disabled");
	}
#endif

	if (link_init() != 0) {
		LOG_ERR("link_init failed; host text replies won't render");
	}

	/* First flush before unblanking to avoid a frame of garbage. */
	ui_tick();
	ret = display_blanking_off(display);
	if (ret < 0 && ret != -ENOSYS) {
		LOG_ERR("display_blanking_off failed: %d", ret);
		return ret;
	}

#ifdef CONFIG_APP_SNAPSHOT
	/* Snapshot capture path: force each of the four states in turn,
	 * pump LVGL for long enough that the per-state animations have
	 * moved off their "frame 0" poses (LISTENING bars and IDLE orb
	 * ease in/out over ~1 s), write a PPM per state, then exit.
	 *
	 * Timing: 1500 ms of ui_tick() at 10 ms/tick = 150 anim steps. That
	 * places the IDLE orb and LISTENING bars well inside their first
	 * cycle and gives the RESPONDING typing label ~18 characters in
	 * (enough to clearly show the typing effect + cursor).
	 */
	static const struct {
		assist_state_t state;
		const char *path;
	} shots[] = {
		{ASSIST_IDLE,       "/tmp/snapshots/pse84_assistant_01_idle.ppm"},
		{ASSIST_LISTENING,  "/tmp/snapshots/pse84_assistant_02_listening.ppm"},
		{ASSIST_THINKING,   "/tmp/snapshots/pse84_assistant_03_thinking.ppm"},
		{ASSIST_RESPONDING, "/tmp/snapshots/pse84_assistant_04_responding.ppm"},
	};

	for (size_t i = 0; i < ARRAY_SIZE(shots); i++) {
		(void)state_set(shots[i].state);

		for (int t = 0; t < 150; t++) {
			ui_tick();
			k_sleep(K_MSEC(10));
		}

		const int rc = app_snapshot_save_ppm(shots[i].path);

		if (rc != 0) {
			LOG_ERR("snapshot save failed for %s: %d",
				state_name(shots[i].state), rc);
			nsi_exit(1);
		}
	}
	LOG_INF("snapshot capture complete, exiting");
	nsi_exit(0);
#endif /* CONFIG_APP_SNAPSHOT */

	while (1) {
		ui_tick();
		k_sleep(K_MSEC(10));
	}
#else  /* !CONFIG_LVGL */
	LOG_INF("headless mode: cycling state every %d ms", HEADLESS_CYCLE_PERIOD_MS);
	k_timer_start(&headless_cycle_timer, K_MSEC(HEADLESS_CYCLE_PERIOD_MS),
		      K_MSEC(HEADLESS_CYCLE_PERIOD_MS));

	while (1) {
		k_sleep(K_SECONDS(1));
	}
#endif /* CONFIG_LVGL */

	return 0;
}
