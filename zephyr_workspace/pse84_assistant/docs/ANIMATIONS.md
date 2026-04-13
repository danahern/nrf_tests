# Per-state animations — design note

Phase 1 (Track A) renders four distinct visuals, one per `assist_state_t`:

| State      | Visual                                     | Engine                                   |
|------------|--------------------------------------------|------------------------------------------|
| IDLE       | Gradient orb breathing (size + opacity)    | `lv_anim` on `lv_obj` size + `bg_opa`    |
| LISTENING  | 5-bar VU meter (heights out of phase)      | `lv_anim` on `lv_obj` height, flex row   |
| THINKING   | Rotating spinner                           | `lv_spinner` (fallback: `lv_arc`)        |
| RESPONDING | Typing-effect label with cursor            | `lv_timer` advancing a slice of a string |

All widgets live under `lv_screen_active()`; only one group is visible at a
time (`ui_show_state()` toggles `LV_OBJ_FLAG_HIDDEN`).

## Why procedural, not sprite sheets

The master plan (`compiled-snuggling-nygaard.md` Phase 1) sketched an
RGB565 sprite-sheet path (≈9.6 MB for 4 × 30 frames × 200×200×2B, fit in
the 10 MB XIP partition). Track A went procedural instead for three
concrete reasons:

1. **Portability across the two display pipelines.** Native_sim is
   320×240 RGBA8888; the PSE84 GFXSS panel is 800×480 RGB565. Pixel-
   baked sprites need one asset set per resolution × color depth;
   `lv_anim`-driven widgets with `LV_ALIGN_*` / `LV_PCT()` re-centre
   and re-scale for free.
2. **Zero asset bytes, zero XIP dependency.** The 10 MB XIP partition
   is shared with the octal-flash boot story that's still being
   stabilised (PSRAM WIP, `enable_cm55` companion hardfault). Procedural
   keeps the animations out of that blast radius.
3. **Cheap iteration.** Tweaking a breath period or a bar count is a
   number in `ui.c`; re-rendering a sprite sheet is a Python pipeline
   + a rebuild.

## When to escalate to sprite sheets

Procedural suffices for v1. Switch to a sprite sheet if any of these
hit:

- **Visual density.** If product asks for something that can't be
  expressed as N primitive shapes + simple anim paths (e.g. a detailed
  branded mascot loop). `lv_canvas` + software draw is an intermediate
  step before committing to pre-rendered assets.
- **Frame budget.** LVGL's software renderer on GFXSS at 800×480 RGB565
  should hit ≥30 fps for these four animations (measured on the
  cartoon_test baseline). If the Thinking spinner + backdrop gradient
  drop below that, pre-rendered frames are cheaper per pixel than
  re-drawing a gradient every tick.
- **Memory pressure.** We bumped `LV_Z_MEM_POOL_SIZE` to 48 KB to host
  the flex container + spinner + per-state style scratch. If a future
  widget pushes that above ~64 KB the sprite route trades RAM for XIP.

## Cross-platform caveats already baked in

- Fonts: `lv_font_montserrat_28` is the only font enabled; both
  `prj.conf` and `prj_native_sim.conf` agree.
- Color literals use `lv_color_hex()` so they port between 16- and
  32-bit colour depths without a sprite re-bake.
- Bar / orb sizes are in pixels (not `LV_PCT`) because native_sim's
  320×240 is already tight for a 200 px widget. The numbers chosen
  (≤130 px orb, ≤110 px bar height) fit 320×240 AND look proportionate
  at 800×480. Revisit if the product wants to push to the edges of the
  panel — at that point `LV_PCT` + a small board-aware scale factor in
  `ui.c` is cheaper than sprite sheets.

## Input

Both boards use the same `gpio_keys` → `INPUT_KEY_0` path. On HW that's
the on-board user button (`sw0`, `gpio_prt8` pin 3). On native_sim that's
SDL `SCANCODE_SPACE` mapped via `zephyr,gpio-emul-sdl`. Press advances
`IDLE → LISTENING → THINKING → RESPONDING → IDLE`.
