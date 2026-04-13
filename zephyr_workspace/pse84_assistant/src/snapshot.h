/*
 * PSE84 Voice Assistant — LVGL → PPM snapshot helper (native_sim only).
 *
 * Built into a dedicated `prj_native_sim_snapshot.conf` flavour so the
 * primary native_sim / HW / QEMU M55 builds stay untouched. Writes the
 * active LVGL screen as a binary PPM (P6) file through the
 * native_simulator host trampolines, so the PPM lands on the docker
 * host's filesystem (where a post-processing step converts it to PNG).
 *
 * PPM was chosen over PNG because it is a one-header + raw-pixel format,
 * requires zero compression / CRC machinery, and is trivially readable
 * by Pillow / ffmpeg / `sips` on macOS. The `.ppm` files are treated as
 * intermediates and deleted after PNG conversion by the capture script.
 */

#ifndef PSE84_ASSISTANT_SNAPSHOT_H_
#define PSE84_ASSISTANT_SNAPSHOT_H_

#ifdef CONFIG_APP_SNAPSHOT

/*
 * Take a snapshot of lv_screen_active() and write it as a PPM (P6) file
 * at `path`.
 *
 * The file at `path` MUST already exist (the capture script pre-creates
 * empty files); this keeps us on the minimal `nsi_host_open` trampoline
 * API, which does not expose a mode argument for O_CREAT. Returns 0 on
 * success, a negative errno on failure.
 */
int app_snapshot_save_ppm(const char *path);

#endif /* CONFIG_APP_SNAPSHOT */

#endif /* PSE84_ASSISTANT_SNAPSHOT_H_ */
