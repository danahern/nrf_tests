/*
 * Opus wrapper — see opus_wrapper.h for rationale.
 *
 * The file compiles in two modes:
 *   CONFIG_OPUS=y : real implementation against upstream libopus.
 *   CONFIG_OPUS=n : every entry point returns -ENOTSUP so link succeeds
 *                   even when the codec is stubbed out.
 */

#include "opus_wrapper.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(opus_wrap, LOG_LEVEL_INF);

#define OPUS_FRAME_MS 20 /* Hard-coded — matches the L2CAP AUDIO frame cadence. */

#ifdef CONFIG_OPUS

#include <opus.h>

struct opus_wrap_ctx {
	OpusEncoder *enc;
	OpusDecoder *dec;
	int sample_rate;
	int channels;
	int frame_samples; /* per channel */
};

static bool sample_rate_supported(int sr)
{
	switch (sr) {
	case 8000:
	case 12000:
	case 16000:
	case 24000:
	case 48000:
		return true;
	default:
		return false;
	}
}

opus_wrap_ctx_t *opus_wrap_init(int sample_rate, int channels, int bitrate_bps)
{
	if (!sample_rate_supported(sample_rate) || channels < 1 || channels > 2 ||
	    bitrate_bps <= 0) {
		LOG_ERR("opus_wrap_init: bad args sr=%d ch=%d br=%d",
			sample_rate, channels, bitrate_bps);
		return NULL;
	}

	opus_wrap_ctx_t *ctx = k_calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		LOG_ERR("opus_wrap_init: OOM (ctx)");
		return NULL;
	}

	int err = 0;
	ctx->enc = opus_encoder_create(sample_rate, channels,
				       OPUS_APPLICATION_VOIP, &err);
	if (err != OPUS_OK || ctx->enc == NULL) {
		LOG_ERR("opus_encoder_create failed: %d", err);
		k_free(ctx);
		return NULL;
	}

	/* Decoder is optional — M55 only encodes; host→device audio
	 * (Phase 8 TTS) is the only caller that needs it. If malloc
	 * can't fit both enc+dec, keep the encoder and log. Callers
	 * of opus_wrap_decode_frame must handle ctx->dec == NULL. */
	ctx->dec = opus_decoder_create(sample_rate, channels, &err);
	if (err != OPUS_OK || ctx->dec == NULL) {
		LOG_WRN("opus_decoder_create failed: %d (encoder still "
			"available for TX-only use)", err);
		ctx->dec = NULL;
	}

	/* CBR, no DTX, no FEC — predictable wire budget, matches the
	 * master-plan 16 kbps / 20 ms = 40 B per frame target.
	 */
	(void)opus_encoder_ctl(ctx->enc, OPUS_SET_BITRATE(bitrate_bps));
	(void)opus_encoder_ctl(ctx->enc, OPUS_SET_VBR(0));
	(void)opus_encoder_ctl(ctx->enc, OPUS_SET_DTX(0));
	(void)opus_encoder_ctl(ctx->enc, OPUS_SET_APPLICATION(OPUS_APPLICATION_VOIP));
	(void)opus_encoder_ctl(ctx->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

	ctx->sample_rate = sample_rate;
	ctx->channels = channels;
	ctx->frame_samples = (sample_rate * OPUS_FRAME_MS) / 1000;
	return ctx;
}

int opus_wrap_encode_frame(opus_wrap_ctx_t *ctx,
			   const int16_t *pcm, int pcm_len,
			   uint8_t *out, int out_cap)
{
	if (ctx == NULL || pcm == NULL || out == NULL || out_cap <= 0) {
		return -EINVAL;
	}
	if (pcm_len != ctx->frame_samples * ctx->channels) {
		LOG_ERR("opus_wrap_encode_frame: got %d samples, need %d",
			pcm_len, ctx->frame_samples * ctx->channels);
		return -EINVAL;
	}

	int n = opus_encode(ctx->enc, pcm, ctx->frame_samples, out, out_cap);
	if (n < 0) {
		LOG_ERR("opus_encode failed: %d", n);
		return -EIO;
	}
	return n;
}

int opus_wrap_decode_frame(opus_wrap_ctx_t *ctx,
			   const uint8_t *in, int in_len,
			   int16_t *pcm_out, int out_cap_samples)
{
	if (ctx == NULL || pcm_out == NULL) {
		return -EINVAL;
	}
	if (ctx->dec == NULL) {
		return -ENOTSUP;
	}
	if (out_cap_samples < ctx->frame_samples * ctx->channels) {
		return -ENOSPC;
	}

	/* in=NULL/in_len=0 is libopus's PLC signal. */
	int n = opus_decode(ctx->dec, (in_len > 0) ? in : NULL, in_len,
			    pcm_out, ctx->frame_samples, 0);
	if (n < 0) {
		LOG_ERR("opus_decode failed: %d", n);
		return -EIO;
	}
	return n; /* per-channel samples */
}

void opus_wrap_free(opus_wrap_ctx_t *ctx)
{
	if (ctx == NULL) {
		return;
	}
	if (ctx->enc != NULL) {
		opus_encoder_destroy(ctx->enc);
	}
	if (ctx->dec != NULL) {
		opus_decoder_destroy(ctx->dec);
	}
	k_free(ctx);
}

int opus_wrap_frame_samples(const opus_wrap_ctx_t *ctx)
{
	return (ctx == NULL) ? 0 : ctx->frame_samples;
}

#else /* !CONFIG_OPUS */

struct opus_wrap_ctx {
	int _unused;
};

opus_wrap_ctx_t *opus_wrap_init(int sample_rate, int channels, int bitrate_bps)
{
	ARG_UNUSED(sample_rate);
	ARG_UNUSED(channels);
	ARG_UNUSED(bitrate_bps);
	LOG_WRN("opus_wrap_init called with CONFIG_OPUS=n");
	return NULL;
}

int opus_wrap_encode_frame(opus_wrap_ctx_t *ctx,
			   const int16_t *pcm, int pcm_len,
			   uint8_t *out, int out_cap)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(pcm);
	ARG_UNUSED(pcm_len);
	ARG_UNUSED(out);
	ARG_UNUSED(out_cap);
	return -ENOTSUP;
}

int opus_wrap_decode_frame(opus_wrap_ctx_t *ctx,
			   const uint8_t *in, int in_len,
			   int16_t *pcm_out, int out_cap_samples)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(in);
	ARG_UNUSED(in_len);
	ARG_UNUSED(pcm_out);
	ARG_UNUSED(out_cap_samples);
	return -ENOTSUP;
}

void opus_wrap_free(opus_wrap_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
}

int opus_wrap_frame_samples(const opus_wrap_ctx_t *ctx)
{
	ARG_UNUSED(ctx);
	return 0;
}

#endif /* CONFIG_OPUS */
