/*
 * PSE84 Voice Assistant — LVGL UI (implementation).
 *
 * Four procedural animations, one per assist_state_t. All widgets live
 * under lv_screen_active(); only one widget group is visible at a time
 * (ui_show_state() toggles LV_OBJ_FLAG_HIDDEN). Animations run
 * unconditionally — LVGL's anim engine is cheap and pausing risks state
 * desync on re-show. Positioning is LV_ALIGN_CENTER / LV_PCT so the same
 * scene adapts to native_sim 320x240 and HW 800x480.
 *
 * Animation choices (see docs/ANIMATIONS.md for the tradeoff rationale):
 *   IDLE        — gradient orb, size + opacity breathe over 2 s.
 *   LISTENING   — 5 vertical bars, heights animated out of phase (VU meter).
 *   THINKING    — lv_spinner (built-in arc animation).
 *   RESPONDING  — typing-effect label, one char every 80 ms.
 */

#include "ui.h"

#include <lvgl.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_APP_ANIMATION_SPRITES
#include "sprites.h"
#endif

LOG_MODULE_REGISTER(assist_ui, LOG_LEVEL_INF);

#define RESPOND_BARS          5
#define RESPOND_BAR_PERIOD_MS 900
#define IDLE_BREATHE_MS       2000
#define TYPING_TICK_MS        80
#define TYPING_TEXT           "The answer is forty-two."

struct ui_scene {
	lv_obj_t *status_label;

	/* Per-state containers. Each holds exactly the widgets for its state
	 * so swapping visibility is one hidden-flag flip.
	 */
	lv_obj_t *idle_group;
	lv_obj_t *listening_group;
	lv_obj_t *thinking_group;
	lv_obj_t *responding_group;

	/* Animated children we need to poke. */
	lv_obj_t *idle_orb;
	lv_obj_t *listening_bars[RESPOND_BARS];
	lv_obj_t *responding_label;

	/* Typing-effect scratch. */
	uint32_t typing_idx;
	lv_timer_t *typing_timer;
};

static struct ui_scene ui;

#ifdef CONFIG_APP_ANIMATION_PROCEDURAL
/* ---- animation callbacks (LVGL calls these with int32_t values) ---- */

static void anim_orb_size_cb(void *var, int32_t v)
{
	lv_obj_set_size((lv_obj_t *)var, v, v);
	/* Re-centre after resize since LV_ALIGN_CENTER anchors on the top-left. */
	lv_obj_align((lv_obj_t *)var, LV_ALIGN_CENTER, 0, 0);
}

static void anim_orb_opacity_cb(void *var, int32_t v)
{
	lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_bar_height_cb(void *var, int32_t v)
{
	lv_obj_set_height((lv_obj_t *)var, v);
	/* Keep the bar bottom-anchored so height grows upward (VU-meter style). */
	lv_obj_align((lv_obj_t *)var, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* ---- group builders ---- */

static void build_idle_group(lv_obj_t *parent)
{
	ui.idle_group = lv_obj_create(parent);
	lv_obj_remove_style_all(ui.idle_group);
	lv_obj_set_size(ui.idle_group, LV_PCT(100), LV_PCT(80));
	lv_obj_align(ui.idle_group, LV_ALIGN_CENTER, 0, 10);

	ui.idle_orb = lv_obj_create(ui.idle_group);
	lv_obj_remove_style_all(ui.idle_orb);
	lv_obj_set_size(ui.idle_orb, 80, 80);
	lv_obj_align(ui.idle_orb, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_radius(ui.idle_orb, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(ui.idle_orb, lv_color_hex(0x3B82F6), 0); /* blue-500 */
	lv_obj_set_style_bg_grad_color(ui.idle_orb, lv_color_hex(0x1E3A8A), 0);
	lv_obj_set_style_bg_grad_dir(ui.idle_orb, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(ui.idle_orb, LV_OPA_COVER, 0);
	lv_obj_clear_flag(ui.idle_orb, LV_OBJ_FLAG_SCROLLABLE);

	/* Size breathe: 80 -> 130 -> 80 over 2 s, forever. */
	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, ui.idle_orb);
	lv_anim_set_exec_cb(&a, anim_orb_size_cb);
	lv_anim_set_values(&a, 80, 130);
	lv_anim_set_duration(&a, IDLE_BREATHE_MS / 2);
	lv_anim_set_reverse_duration(&a, IDLE_BREATHE_MS / 2);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_start(&a);

	lv_anim_init(&a);
	lv_anim_set_var(&a, ui.idle_orb);
	lv_anim_set_exec_cb(&a, anim_orb_opacity_cb);
	lv_anim_set_values(&a, LV_OPA_60, LV_OPA_COVER);
	lv_anim_set_duration(&a, IDLE_BREATHE_MS / 2);
	lv_anim_set_reverse_duration(&a, IDLE_BREATHE_MS / 2);
	lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_start(&a);
}

static void build_listening_group(lv_obj_t *parent)
{
	ui.listening_group = lv_obj_create(parent);
	lv_obj_remove_style_all(ui.listening_group);
	lv_obj_set_size(ui.listening_group, 220, 120);
	lv_obj_align(ui.listening_group, LV_ALIGN_CENTER, 0, 10);
	/* flex_row with even spacing keeps it layout-engine driven, not
	 * per-pixel. Bars sit on the bottom; heights animate above.
	 */
	lv_obj_set_flex_flow(ui.listening_group, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(ui.listening_group, LV_FLEX_ALIGN_SPACE_EVENLY,
			      LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_clear_flag(ui.listening_group, LV_OBJ_FLAG_SCROLLABLE);

	for (int i = 0; i < RESPOND_BARS; i++) {
		lv_obj_t *bar = lv_obj_create(ui.listening_group);

		lv_obj_remove_style_all(bar);
		lv_obj_set_size(bar, 20, 30);
		lv_obj_set_style_radius(bar, 6, 0);
		lv_obj_set_style_bg_color(bar, lv_color_hex(0x10B981), 0); /* emerald-500 */
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
		lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
		ui.listening_bars[i] = bar;

		/* Phase-offset per bar so they don't move in lockstep. */
		lv_anim_t a;

		lv_anim_init(&a);
		lv_anim_set_var(&a, bar);
		lv_anim_set_exec_cb(&a, anim_bar_height_cb);
		lv_anim_set_values(&a, 15, 110);
		lv_anim_set_duration(&a, RESPOND_BAR_PERIOD_MS / 2);
		lv_anim_set_reverse_duration(&a, RESPOND_BAR_PERIOD_MS / 2);
		lv_anim_set_delay(&a, i * (RESPOND_BAR_PERIOD_MS / (RESPOND_BARS * 2)));
		lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
		lv_anim_start(&a);
	}
}

static void build_thinking_group(lv_obj_t *parent)
{
	ui.thinking_group = lv_obj_create(parent);
	lv_obj_remove_style_all(ui.thinking_group);
	lv_obj_set_size(ui.thinking_group, LV_PCT(100), LV_PCT(80));
	lv_obj_align(ui.thinking_group, LV_ALIGN_CENTER, 0, 10);

#if defined(LV_USE_SPINNER) && LV_USE_SPINNER
	lv_obj_t *spin = lv_spinner_create(ui.thinking_group);

	lv_spinner_set_anim_params(spin, 1000, 200);
	lv_obj_set_size(spin, 110, 110);
	lv_obj_align(spin, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_arc_color(spin, lv_color_hex(0xF59E0B), LV_PART_INDICATOR); /* amber */
#else
	/* Spinner disabled in LV config — fall back to a rotating arc built
	 * from lv_arc for the same visual intent.
	 */
	lv_obj_t *arc = lv_arc_create(ui.thinking_group);

	lv_obj_set_size(arc, 110, 110);
	lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
	lv_arc_set_bg_angles(arc, 0, 360);
	lv_arc_set_value(arc, 75);
	lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
#endif
}

static void typing_timer_cb(lv_timer_t *t)
{
	ARG_UNUSED(t);
	const size_t total = strlen(TYPING_TEXT);

	ui.typing_idx++;
	if (ui.typing_idx > total) {
		ui.typing_idx = 0;
	}
	char buf[sizeof(TYPING_TEXT) + 2];

	memcpy(buf, TYPING_TEXT, ui.typing_idx);
	buf[ui.typing_idx] = '_'; /* fake cursor */
	buf[ui.typing_idx + 1] = '\0';
	if (ui.responding_label != NULL) {
		lv_label_set_text(ui.responding_label, buf);
	}
}

static void build_responding_group(lv_obj_t *parent)
{
	ui.responding_group = lv_obj_create(parent);
	lv_obj_remove_style_all(ui.responding_group);
	lv_obj_set_size(ui.responding_group, LV_PCT(90), LV_PCT(60));
	lv_obj_align(ui.responding_group, LV_ALIGN_CENTER, 0, 10);
	lv_obj_clear_flag(ui.responding_group, LV_OBJ_FLAG_SCROLLABLE);

	ui.responding_label = lv_label_create(ui.responding_group);
	lv_label_set_long_mode(ui.responding_label, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_width(ui.responding_label, LV_PCT(100));
	lv_obj_set_style_text_align(ui.responding_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(ui.responding_label, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(ui.responding_label, lv_color_hex(0xE879F9), 0); /* fuchsia */
	lv_label_set_text(ui.responding_label, "_");
	lv_obj_align(ui.responding_label, LV_ALIGN_CENTER, 0, 0);

	/* Timer runs permanently; pauses visually whenever the group is hidden. */
	ui.typing_timer = lv_timer_create(typing_timer_cb, TYPING_TICK_MS, NULL);
}
#endif /* CONFIG_APP_ANIMATION_PROCEDURAL */

/* ---- state switching ---- */

static void ui_show_state(assist_state_t s)
{
#ifdef CONFIG_APP_ANIMATION_SPRITES
	/* Sprite path: all four groups stay hidden (built conditionally, see
	 * build_*_group calls in ui_init); one sprite_sheet plays on the
	 * dedicated lv_image widget.
	 */
	const struct sprite_sheet *sheet = NULL;

	switch (s) {
	case ASSIST_IDLE:       sheet = &sprite_sheet_idle;       break;
	case ASSIST_LISTENING:  sheet = &sprite_sheet_listening;  break;
	case ASSIST_THINKING:   sheet = &sprite_sheet_thinking;   break;
	case ASSIST_RESPONDING: sheet = &sprite_sheet_responding; break;
	default: break;
	}
	sprites_play(sheet);

	if (ui.status_label != NULL) {
		lv_label_set_text(ui.status_label, state_name(s));
	}
#else
	static const struct {
		assist_state_t state;
		lv_obj_t **group;
	} groups[] = {
		{ASSIST_IDLE, &ui.idle_group},
		{ASSIST_LISTENING, &ui.listening_group},
		{ASSIST_THINKING, &ui.thinking_group},
		{ASSIST_RESPONDING, &ui.responding_group},
	};

	for (size_t i = 0; i < ARRAY_SIZE(groups); i++) {
		lv_obj_t *g = *groups[i].group;

		if (g == NULL) {
			continue;
		}
		if (groups[i].state == s) {
			lv_obj_clear_flag(g, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (ui.status_label != NULL) {
		lv_label_set_text(ui.status_label, state_name(s));
	}

	if (s == ASSIST_RESPONDING) {
		ui.typing_idx = 0;
	}
#endif
}

static void on_state_entry(assist_state_t prev, assist_state_t next)
{
	ARG_UNUSED(prev);
	ui_show_state(next);
}

void ui_init(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1220), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	/* Status label top-center — persistent across states. */
	ui.status_label = lv_label_create(scr);
	lv_label_set_text(ui.status_label, state_name(state_get()));
	lv_obj_set_style_text_font(ui.status_label, &lv_font_montserrat_28, 0);
	lv_obj_set_style_text_color(ui.status_label, lv_color_hex(0xFFFFFF), 0);
	lv_obj_align(ui.status_label, LV_ALIGN_TOP_MID, 0, 8);

#ifdef CONFIG_APP_ANIMATION_SPRITES
	if (sprites_init(scr) != 0) {
		LOG_ERR("sprites_init failed; screen will remain blank");
	}
#endif
#ifdef CONFIG_APP_ANIMATION_PROCEDURAL
	build_idle_group(scr);
	build_listening_group(scr);
	build_thinking_group(scr);
	build_responding_group(scr);
#endif

	/* Show the current state; hide others. */
	ui_show_state(state_get());

	state_set_on_entry(on_state_entry);
}

void ui_tick(void)
{
	lv_timer_handler();
}
