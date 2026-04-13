/*
 * Opus encode -> decode round trip against a synthetic 1 kHz sine @
 * 16 kHz mono. We allow up to 600 ms of codec warm-up (first frames
 * are muted while SILK boots its LTP state) before measuring signal
 * energy, then assert RMS error stays below a loose threshold. The
 * goal is "codec is wired up and does something sensible", not PSNR
 * perfection — the quality gate lives in the host bridge roundtrip.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "opus_wrapper.h"

#define SAMPLE_RATE   16000
#define CHANNELS      1
#define BITRATE_BPS   16000
#define FRAME_SAMPLES 320 /* 20 ms @ 16 kHz */
#define NUM_FRAMES    60  /* 1.2 s of audio — enough to pass warm-up */
#define WARMUP_FRAMES 30  /* first 600 ms: codec convergence */
#define OPUS_OUT_MAX  1275 /* libopus hard ceiling for one packet */

static int16_t pcm_in[FRAME_SAMPLES];
static int16_t pcm_out[FRAME_SAMPLES];
static uint8_t packet[OPUS_OUT_MAX];

/* Fill pcm_in with one frame of a 1 kHz sine starting at @p frame_idx. */
static void fill_sine(int frame_idx)
{
	const double two_pi_f_over_sr = 2.0 * 3.14159265358979323846 *
					1000.0 / (double)SAMPLE_RATE;
	const double amp = 16000.0; /* headroom below int16 clip */

	for (int i = 0; i < FRAME_SAMPLES; i++) {
		int n = frame_idx * FRAME_SAMPLES + i;
		double v = amp * sin(two_pi_f_over_sr * (double)n);
		pcm_in[i] = (int16_t)v;
	}
}

ZTEST(opus_roundtrip, test_init_rejects_bad_args)
{
	zassert_is_null(opus_wrap_init(7777, 1, 16000), "bad sample rate");
	zassert_is_null(opus_wrap_init(16000, 0, 16000), "bad channels (0)");
	zassert_is_null(opus_wrap_init(16000, 3, 16000), "bad channels (3)");
	zassert_is_null(opus_wrap_init(16000, 1, -1), "bad bitrate");
}

ZTEST(opus_roundtrip, test_frame_samples_reports_frame_size)
{
	opus_wrap_ctx_t *ctx = opus_wrap_init(SAMPLE_RATE, CHANNELS, BITRATE_BPS);
	zassert_not_null(ctx, "init");
	zassert_equal(opus_wrap_frame_samples(ctx), FRAME_SAMPLES, "20 ms");
	zassert_equal(opus_wrap_frame_samples(NULL), 0, "null-safe");
	opus_wrap_free(ctx);
}

ZTEST(opus_roundtrip, test_encode_rejects_wrong_frame_size)
{
	opus_wrap_ctx_t *ctx = opus_wrap_init(SAMPLE_RATE, CHANNELS, BITRATE_BPS);
	zassert_not_null(ctx, "init");
	int n = opus_wrap_encode_frame(ctx, pcm_in, FRAME_SAMPLES + 1,
				       packet, sizeof(packet));
	zassert_true(n < 0, "expected error on wrong frame size, got %d", n);
	opus_wrap_free(ctx);
}

ZTEST(opus_roundtrip, test_sine_roundtrip_rms_energy_preserved)
{
	/* Sample-wise error is not useful for an Opus roundtrip check:
	 * the codec introduces a multi-millisecond lookahead / delay, so
	 * `dec[i] - ref[i]` is dominated by phase offset even when the
	 * decoded signal is a faithful 1 kHz sine. Instead we compare
	 * block-level RMS energy — if the codec is wired up correctly,
	 * RMS(decoded) should track RMS(input) to within a modest
	 * tolerance, without caring about alignment. This catches the
	 * real failure modes (muted output, garbage samples, wrong
	 * sample-rate config) while tolerating the codec's intrinsic
	 * group delay.
	 */
	opus_wrap_ctx_t *ctx = opus_wrap_init(SAMPLE_RATE, CHANNELS, BITRATE_BPS);
	zassert_not_null(ctx, "init");

	double sum_sq_dec = 0.0;
	double sum_sq_ref = 0.0;
	long long measured_samples = 0;

	for (int f = 0; f < NUM_FRAMES; f++) {
		fill_sine(f);
		int enc = opus_wrap_encode_frame(ctx, pcm_in, FRAME_SAMPLES,
						 packet, sizeof(packet));
		zassert_true(enc > 0, "encode failed at frame %d: %d", f, enc);
		/* 16 kbps CBR / 50 frames per sec = 40 byte frames. Allow a
		 * small cushion for CBR rounding and header padding.
		 */
		zassert_true(enc <= 80,
			     "frame %d encoded to %d bytes (expected ~40)", f, enc);

		int dec = opus_wrap_decode_frame(ctx, packet, enc,
						 pcm_out, FRAME_SAMPLES);
		zassert_equal(dec, FRAME_SAMPLES,
			      "decode returned %d samples at frame %d", dec, f);

		if (f < WARMUP_FRAMES) {
			continue; /* codec is still converging */
		}
		for (int i = 0; i < FRAME_SAMPLES; i++) {
			double ref = (double)pcm_in[i];
			double d = (double)pcm_out[i];
			sum_sq_ref += ref * ref;
			sum_sq_dec += d * d;
		}
		measured_samples += FRAME_SAMPLES;
	}

	zassert_true(measured_samples > 0, "no samples measured");
	double rms_ref = sqrt(sum_sq_ref / (double)measured_samples);
	double rms_dec = sqrt(sum_sq_dec / (double)measured_samples);
	double ratio = rms_dec / rms_ref;

	TC_PRINT("rms_ref=%f rms_dec=%f ratio=%f\n", rms_ref, rms_dec, ratio);
	/* Input sine is amplitude 16000 -> RMS ≈ 11313. Decoded energy
	 * should land in roughly the same neighborhood (0.5x – 1.5x).
	 * SILK at 16 kbps will modulate the envelope slightly; CELT
	 * layering can add a touch of transient energy. A 2x / 0.5x
	 * window comfortably rejects "codec broken" while tolerating
	 * real codec behavior.
	 */
	zassert_true(ratio > 0.5 && ratio < 2.0,
		     "decoded RMS out of band: %f", ratio);

	/* And guarantee the decoder actually produced audio — not zeros,
	 * not rails. At amplitude 16000 in, expect RMS well above 500.
	 */
	zassert_true(rms_dec > 500.0, "decoded signal too quiet: %f", rms_dec);

	opus_wrap_free(ctx);
}

ZTEST(opus_roundtrip, test_plc_accepts_zero_length_input)
{
	opus_wrap_ctx_t *ctx = opus_wrap_init(SAMPLE_RATE, CHANNELS, BITRATE_BPS);
	zassert_not_null(ctx, "init");

	/* Drive one real frame first so the decoder has state. */
	fill_sine(0);
	int enc = opus_wrap_encode_frame(ctx, pcm_in, FRAME_SAMPLES,
					 packet, sizeof(packet));
	zassert_true(enc > 0, "encode");
	int dec = opus_wrap_decode_frame(ctx, packet, enc,
					 pcm_out, FRAME_SAMPLES);
	zassert_equal(dec, FRAME_SAMPLES, "decode");

	/* Now request PLC. */
	int plc = opus_wrap_decode_frame(ctx, NULL, 0, pcm_out, FRAME_SAMPLES);
	zassert_equal(plc, FRAME_SAMPLES, "PLC should produce a full frame");

	opus_wrap_free(ctx);
}

ZTEST(opus_roundtrip, test_free_null_is_safe)
{
	opus_wrap_free(NULL); /* must not crash */
}

ZTEST_SUITE(opus_roundtrip, NULL, NULL, NULL, NULL, NULL);
