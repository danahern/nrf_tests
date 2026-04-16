/*
 * PSE84 Voice Assistant — Phase 2 PDM audio capture.
 *
 * Thin wrapper around the Zephyr DMIC API (drivers/audio/dmic_infineon.c)
 * that exposes a button-hold capture model:
 *
 *   audio_init()            — probe DMIC device, configure 16 kHz mono s16,
 *                             spawn the dedicated capture thread.
 *   audio_capture_start()   — begin filling a 2 s ring buffer.
 *   audio_capture_stop()    — stop + emit a BEGIN/END-framed hex dump of
 *                             the captured samples over the console UART.
 *
 * Everything is compiled out when CONFIG_AUDIO_DMIC is not set, so the
 * native_sim and QEMU M55 smoke-test builds stay lean.
 */

#ifndef PSE84_ASSISTANT_AUDIO_H_
#define PSE84_ASSISTANT_AUDIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture parameters (hard-coded per Phase 2 scope lock). */
#define AUDIO_SAMPLE_RATE_HZ   16000
#define AUDIO_CHANNELS         1
#define AUDIO_BITS_PER_SAMPLE  16
#define AUDIO_MAX_DURATION_MS  2000
#define AUDIO_MIN_DURATION_MS  100

/* Derived: one second of mono s16 @ 16 kHz = 32 KB, two seconds = 64 KB. */
#define AUDIO_BYTES_PER_MS \
	((AUDIO_SAMPLE_RATE_HZ / 1000) * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8))
#define AUDIO_RING_BYTES (AUDIO_BYTES_PER_MS * AUDIO_MAX_DURATION_MS)

/* Driver block sizing — 100 ms per block matches the upstream sample and
 * gives us 20 blocks covering the 2 s cap with a single dmic_read() per
 * 100 ms of wall-clock.
 */
#define AUDIO_BLOCK_DURATION_MS 100
#define AUDIO_BLOCK_BYTES       (AUDIO_BYTES_PER_MS * AUDIO_BLOCK_DURATION_MS)

/* Initialise the DMIC device and configure it for 16 kHz mono s16.
 * Spawns the capture worker thread but DOES NOT trigger the DMIC
 * hardware — call audio_dmic_kickoff() after bt_enable has finished
 * its firmware download spike (see src/ble.c:ble_wait_ready). Kicking
 * the PDM DMA while uart4 is saturated with HCI firmware bytes
 * starves the DMA completion IRQ and the PDM HW FIFO overflows.
 * Idempotent. Returns 0 on success, negative errno on failure.
 */
int audio_init(void);

/* Start the PDM hardware + DMA. Safe to call multiple times (becomes
 * a no-op once started). Call after audio_init AND after the BT
 * firmware download has completed — see src/ble.h for ble_wait_ready.
 * Returns 0 on success, negative errno on failure.
 */
int audio_dmic_kickoff(void);

/* Begin capture. Clears the ring buffer and arms the DMIC controller.
 * Safe to call while already capturing (becomes a no-op).
 * Returns 0 on success, negative errno on failure.
 */
int audio_capture_start(void);

/* Stop capture and emit a PCM_BEGIN / hex-payload / PCM_END frame over
 * the console UART. If the capture was shorter than AUDIO_MIN_DURATION_MS
 * it is padded with silence; if longer than AUDIO_MAX_DURATION_MS only
 * the trailing AUDIO_MAX_DURATION_MS are emitted. Computes + logs the
 * peak and RMS for quick level checks.
 * Returns 0 on success, negative errno on failure.
 */
int audio_capture_stop(void);

/* True while a capture is in progress. */
bool audio_is_capturing(void);

#ifdef __cplusplus
}
#endif

#endif /* PSE84_ASSISTANT_AUDIO_H_ */
