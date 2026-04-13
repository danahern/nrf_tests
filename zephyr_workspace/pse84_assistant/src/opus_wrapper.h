/*
 * Opus codec wrapper for the PSE84 voice assistant.
 *
 * Thin, opinionated shim on top of upstream libopus: 16 kHz mono, 20 ms
 * frames, fixed bitrate, CBR, VOIP application. Both encoder and decoder
 * are held by a single handle so the same module can run on the M55
 * (encode PDM -> BLE) today and decode TTS frames inbound in v3.
 *
 * When CONFIG_OPUS=n the header is still includable; all entry points
 * return -ENOTSUP so callers can link unconditionally. Kconfig-gated
 * call sites are still the right thing to do to avoid pulling in PCM
 * buffers you're never going to encode.
 *
 * All functions are thread-safe *across different handles*. A single
 * handle must not be used concurrently from encode and decode paths.
 */

#ifndef PSE84_ASSISTANT_OPUS_WRAPPER_H_
#define PSE84_ASSISTANT_OPUS_WRAPPER_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. Allocated by opus_wrap_init, freed by opus_wrap_free. */
typedef struct opus_wrap_ctx opus_wrap_ctx_t;

/**
 * Allocate an encoder+decoder pair.
 *
 * @param sample_rate 8000, 12000, 16000, 24000, or 48000. The assistant
 *                    pipeline uses 16000.
 * @param channels    1 or 2. The assistant pipeline uses 1.
 * @param bitrate_bps Target bitrate. The assistant uses 16000 (16 kbps).
 * @return non-NULL handle on success, NULL on failure (bad args, OOM,
 *         CONFIG_OPUS=n).
 */
opus_wrap_ctx_t *opus_wrap_init(int sample_rate, int channels, int bitrate_bps);

/**
 * Encode one 20 ms frame of interleaved s16 PCM.
 *
 * @param ctx       Handle from opus_wrap_init.
 * @param pcm       Pointer to (20 ms * sample_rate / 1000) * channels s16
 *                  samples. For 16 kHz mono that's 320 samples.
 * @param pcm_len   Number of int16_t samples at @p pcm (not bytes).
 *                  Must equal the configured frame size.
 * @param out       Output buffer for the compressed Opus packet.
 * @param out_cap   Capacity of @p out in bytes.
 * @return bytes written to @p out on success, or a negative errno on
 *         failure. -ENOTSUP if CONFIG_OPUS=n.
 */
int opus_wrap_encode_frame(opus_wrap_ctx_t *ctx,
			   const int16_t *pcm, int pcm_len,
			   uint8_t *out, int out_cap);

/**
 * Decode one Opus packet to interleaved s16 PCM.
 *
 * @param ctx               Handle from opus_wrap_init.
 * @param in                Compressed Opus packet.
 * @param in_len            Length of @p in in bytes. 0 => packet loss
 *                          concealment.
 * @param pcm_out           Output PCM buffer.
 * @param out_cap_samples   Capacity of @p pcm_out in int16_t samples.
 *                          Must be at least one frame (20 ms) of samples.
 * @return int16_t samples written (per channel) on success, or a
 *         negative errno on failure. -ENOTSUP if CONFIG_OPUS=n.
 */
int opus_wrap_decode_frame(opus_wrap_ctx_t *ctx,
			   const uint8_t *in, int in_len,
			   int16_t *pcm_out, int out_cap_samples);

/**
 * Release all resources owned by @p ctx. NULL-safe.
 */
void opus_wrap_free(opus_wrap_ctx_t *ctx);

/**
 * Return the configured per-channel samples per 20 ms frame, or 0 if
 * @p ctx is NULL. Useful for sizing caller-side PCM ring buffers.
 */
int opus_wrap_frame_samples(const opus_wrap_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PSE84_ASSISTANT_OPUS_WRAPPER_H_ */
