/*
 * PSE84 Voice Assistant — LVGL UI.
 *
 * Owns the on-screen layout and per-state animated widgets. Procedural
 * (no sprite sheets) so the same code runs on native_sim 320x240 RGBA8888
 * and HW GFXSS 800x480 RGB565 — all positioning uses LV_ALIGN_* and
 * percentages.
 *
 * Contract:
 *   ui_init() builds the scene, parks it on state_get(), and registers a
 *   state on-entry callback to swap the visible widget. Safe to call once
 *   after state_init() + display setup.
 *   ui_tick() must be called from the main loop; it advances LVGL's
 *   internal timers (lv_timer_handler) and is the only LVGL entry point
 *   from main.c.
 */

#ifndef PSE84_ASSISTANT_UI_H_
#define PSE84_ASSISTANT_UI_H_

#include "state.h"

/* Build the LVGL scene and register state-change callbacks. Must be called
 * after the display device is ready and state_init() has run.
 */
void ui_init(void);

/* Drive LVGL timers. Call from the main loop at ~100 Hz. */
void ui_tick(void);

/* Append a UTF-8 text chunk to the RESPONDING-state reply label. Safe to
 * call before the state actually enters RESPONDING — the accumulator
 * survives the state swap. Thread-safe via an internal mutex.
 */
void ui_append_reply_text(const char *utf8, int len);

/* Clear the reply label. Called on TEXT_END or when the IDLE state is
 * entered so the next utterance starts fresh.
 */
void ui_clear_reply_text(void);

#endif /* PSE84_ASSISTANT_UI_H_ */
