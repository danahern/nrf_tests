/*
 * PSE84 Voice Assistant — Phase 2 PDM audio capture.
 *
 * See audio.h for the public contract.
 *
 * Implementation sketch:
 *
 *   audio_init()         — DEVICE_DT_GET(DT_NODELABEL(dmic_dev)),
 *                          dmic_configure() @ 16 kHz mono s16.
 *
 *   audio_capture_start  — clear ring state, DMIC_TRIGGER_START,
 *                          kick the worker thread via semaphore.
 *
 *   capture thread       — while capturing: dmic_read() with a short
 *                          timeout, memcpy block into ring buffer,
 *                          k_mem_slab_free(), stop appending once the
 *                          buffer is full (latest 2 s wins via a simple
 *                          "cap at full" policy — button-hold is bounded
 *                          by AUDIO_MAX_DURATION_MS anyway).
 *
 *   audio_capture_stop   — DMIC_TRIGGER_STOP, pad with silence if under
 *                          min duration, hex-dump the captured bytes
 *                          between PCM_BEGIN / PCM_END markers on stdout
 *                          via printk() (so the log subsystem can't drop
 *                          the payload), log peak + RMS.
 */

#include "audio.h"

#ifdef CONFIG_AUDIO_DMIC

#include <zephyr/kernel.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

/* DMA block count — match CONFIG_DMIC_INFINEON_QUEUE_SIZE (default 8).
 * Four is enough to absorb one ~40 ms scheduling hiccup at 100 ms/block
 * without the HW FIFO overflowing. Using 8 matches driver default.
 */
#define AUDIO_BLOCK_COUNT 8

/* Memory slab the driver fills with 100 ms PCM blocks. Alignment 4 is
 * what the upstream sample uses and what the DMIC driver expects for
 * int16 PCM DMA destinations.
 */
K_MEM_SLAB_DEFINE_STATIC(audio_mem_slab, AUDIO_BLOCK_BYTES, AUDIO_BLOCK_COUNT, 4);

/* 2 s mono s16 ring buffer. 64 KB — fits comfortably in M55 SRAM
 * (kit_pse84_eval_m55 has ~1 MB zephyr,sram per the board dtsi chain).
 * We don't put this in PSRAM because (a) PSRAM init path is still
 * flaky across the M33 companion hardfault path (see
 * project_pse84_m33_hardfault.md), and (b) 64 KB is trivially small.
 * BSS is zeroed at boot so an "unused" ring starts silent.
 */
static int16_t audio_ring[AUDIO_RING_BYTES / sizeof(int16_t)];
static size_t audio_ring_samples;  /* samples currently filled */
static int64_t audio_capture_start_ms;
static int64_t audio_capture_stop_ms;

static const struct device *dmic_dev;
static bool audio_configured;
static bool audio_capturing;

/* Dedicated capture thread — blocks on dmic_read() while audio_capturing
 * is true, yielding as the DMA fills 100 ms chunks. A semaphore gates
 * the thread on start/stop so it isn't spinning when idle.
 */
#define AUDIO_THREAD_STACK_SIZE 2048
#define AUDIO_THREAD_PRIORITY   5

static K_SEM_DEFINE(audio_capture_sem, 0, 1);
static K_THREAD_STACK_DEFINE(audio_thread_stack, AUDIO_THREAD_STACK_SIZE);
static struct k_thread audio_thread;
static k_tid_t audio_thread_id;

static int audio_configure(void)
{
	static struct pcm_stream_cfg stream = {
		.pcm_width = AUDIO_BITS_PER_SAMPLE,
		.mem_slab  = &audio_mem_slab,
	};
	static struct dmic_cfg cfg;

	/* Re-initialise every time in case of ABI additions; the struct is
	 * static so tests (if any) can re-inspect it via a friend symbol.
	 */
	memset(&cfg, 0, sizeof(cfg));
	cfg.io.min_pdm_clk_freq = 1000000;
	cfg.io.max_pdm_clk_freq = 3500000;
	cfg.io.min_pdm_clk_dc   = 40;
	cfg.io.max_pdm_clk_dc   = 60;
	cfg.streams = &stream;
	cfg.channel.req_num_streams = 1;
	cfg.channel.req_num_chan = AUDIO_CHANNELS;
	/* Controller index 1 = PDM channels 2/3 on pdm3 pins (matches our
	 * overlay which enables dmic0_ch2 with use-alt-io). Sample uses
	 * the same pair, taking channel 2 as LEFT.
	 */
	/* PDM_CHAN_RIGHT: kit_pse84_eval onboard mic drives data on the
	 * rising clock edge (SEL=VDD), so the LEFT half reads all zeros.
	 * Together with fir0-enable on dmic0_ch2 in the overlay this turns
	 * "silence with DC transient" / "peak=0 silence" into real audio.
	 */
	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 1, PDM_CHAN_RIGHT);
	stream.pcm_rate = AUDIO_SAMPLE_RATE_HZ;
	stream.block_size = AUDIO_BLOCK_BYTES;

	int ret = dmic_configure(dmic_dev, &cfg);

	if (ret < 0) {
		LOG_ERR("dmic_configure failed: %d", ret);
		return ret;
	}
	return 0;
}

static void audio_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		/* Wait until a capture is armed. */
		k_sem_take(&audio_capture_sem, K_FOREVER);

		while (audio_capturing) {
			void *buffer;
			uint32_t size;
			/* Short timeout so we notice audio_capturing flipping
			 * to false within one block's worth of wall-clock.
			 */
			int ret = dmic_read(dmic_dev, 0, &buffer, &size,
					    AUDIO_BLOCK_DURATION_MS + 50);

			if (ret < 0) {
				if (ret == -EAGAIN || ret == -ETIMEDOUT) {
					continue;
				}
				LOG_ERR("dmic_read failed: %d", ret);
				break;
			}

			/* Append up to the cap. Beyond the cap we drop the
			 * tail (button is supposed to have been released
			 * already via the 2 s timeout in main.c).
			 */
			const size_t remaining_bytes =
				(size_t)AUDIO_RING_BYTES -
				(audio_ring_samples * sizeof(int16_t));
			const size_t copy_bytes =
				MIN((size_t)size, remaining_bytes);

			if (copy_bytes > 0) {
				memcpy((uint8_t *)audio_ring +
					       audio_ring_samples * sizeof(int16_t),
				       buffer, copy_bytes);
				audio_ring_samples += copy_bytes / sizeof(int16_t);
			}

			k_mem_slab_free(&audio_mem_slab, buffer);
		}
	}
}

int audio_init(void)
{
	if (audio_configured) {
		return 0;
	}

	dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("DMIC device %s not ready", dmic_dev->name);
		return -ENODEV;
	}

	int ret = audio_configure();

	if (ret < 0) {
		return ret;
	}

	audio_thread_id = k_thread_create(&audio_thread, audio_thread_stack,
					  K_THREAD_STACK_SIZEOF(audio_thread_stack),
					  audio_thread_fn, NULL, NULL, NULL,
					  AUDIO_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(audio_thread_id, "audio");

	audio_configured = true;
	LOG_INF("audio_init ok (dmic=%s, %d Hz mono s16, ring=%d bytes)",
		dmic_dev->name, AUDIO_SAMPLE_RATE_HZ, (int)AUDIO_RING_BYTES);
	return 0;
}

int audio_capture_start(void)
{
	if (!audio_configured) {
		return -ENODEV;
	}
	if (audio_capturing) {
		return 0;
	}

	audio_ring_samples = 0;
	audio_capture_start_ms = k_uptime_get();

	int ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);

	if (ret < 0) {
		LOG_ERR("DMIC_TRIGGER_START failed: %d", ret);
		return ret;
	}

	audio_capturing = true;
	k_sem_give(&audio_capture_sem);
	LOG_INF("audio capture START");
	return 0;
}

static void audio_compute_levels(const int16_t *samples, size_t n,
				 int32_t *peak_out, int32_t *rms_out)
{
	int32_t peak = 0;
	uint64_t sumsq = 0;

	for (size_t i = 0; i < n; i++) {
		int32_t s = samples[i];
		int32_t a = s < 0 ? -s : s;

		if (a > peak) {
			peak = a;
		}
		sumsq += (uint64_t)(s * s);
	}

	int32_t rms = 0;

	if (n > 0) {
		uint64_t mean = sumsq / n;

		/* Integer sqrt — n ≤ 32000 and s² ≤ 2^30 so mean fits u32. */
		uint32_t x = (uint32_t)mean;
		uint32_t r = 0;
		uint32_t b = 1u << 30;

		while (b > x) {
			b >>= 2;
		}
		while (b != 0) {
			if (x >= r + b) {
				x -= r + b;
				r = (r >> 1) + b;
			} else {
				r >>= 1;
			}
			b >>= 2;
		}
		rms = (int32_t)r;
	}

	*peak_out = peak;
	*rms_out = rms;
}

/* Emit one hex nibble per 4 bits. LUT + raw printk avoids the log
 * subsystem's ring buffer entirely — critical for the 64 KB burst.
 */
static void audio_hex_dump(const uint8_t *buf, size_t len)
{
	static const char lut[] = "0123456789abcdef";
	char line[64 * 2 + 2]; /* 64 bytes → 128 hex chars + \n + NUL */
	size_t i = 0;

	while (i < len) {
		size_t chunk = MIN((size_t)64, len - i);
		size_t p = 0;

		for (size_t j = 0; j < chunk; j++) {
			uint8_t b = buf[i + j];

			line[p++] = lut[b >> 4];
			line[p++] = lut[b & 0xF];
		}
		line[p++] = '\n';
		line[p] = '\0';
		/* printk is unbuffered (CONFIG_LOG_PRINTK off by default
		 * for direct printk mode) and blocks on UART tx-empty —
		 * that's what we want for lossless bulk transfer.
		 */
		printk("%s", line);
		i += chunk;
	}
}

int audio_capture_stop(void)
{
	if (!audio_configured) {
		return -ENODEV;
	}
	if (!audio_capturing) {
		return 0;
	}

	/* Flip the flag first so the capture thread exits its inner loop
	 * as soon as its next dmic_read() returns.
	 */
	audio_capturing = false;
	audio_capture_stop_ms = k_uptime_get();

	int ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	if (ret < 0) {
		LOG_ERR("DMIC_TRIGGER_STOP failed: %d", ret);
		/* Keep going — we still want whatever we captured. */
	}

	/* Give the thread one block's worth of time to drain its pending
	 * dmic_read() + append.
	 */
	k_sleep(K_MSEC(AUDIO_BLOCK_DURATION_MS + 20));

	/* Pad short presses up to the min duration with trailing silence. */
	const int64_t duration_ms = audio_capture_stop_ms - audio_capture_start_ms;
	const size_t min_samples =
		(AUDIO_MIN_DURATION_MS * AUDIO_SAMPLE_RATE_HZ) / 1000;

	if (audio_ring_samples < min_samples) {
		/* audio_ring is BSS → any unfilled tail is already 0x0000
		 * (digital silence). Just bump the sample count.
		 */
		audio_ring_samples = min_samples;
	}

	/* Cap at 2 s. The ring buffer is exactly 2 s so this usually just
	 * agrees with reality, but belt-and-braces.
	 */
	const size_t max_samples = AUDIO_RING_BYTES / sizeof(int16_t);

	if (audio_ring_samples > max_samples) {
		audio_ring_samples = max_samples;
	}

	int32_t peak, rms;

	audio_compute_levels(audio_ring, audio_ring_samples, &peak, &rms);
	LOG_INF("audio capture STOP: duration=%lld ms, samples=%u, peak=%d, rms=%d",
		duration_ms, (unsigned)audio_ring_samples, peak, rms);

	/* Emit the framed hex dump. The trailing \n on the markers makes
	 * line-oriented host parsing trivial.
	 */
	printk("=== PCM_BEGIN samples=%u sample_rate=%d channels=%d bits=%d ===\n",
	       (unsigned)audio_ring_samples, AUDIO_SAMPLE_RATE_HZ,
	       AUDIO_CHANNELS, AUDIO_BITS_PER_SAMPLE);
	audio_hex_dump((const uint8_t *)audio_ring,
		       audio_ring_samples * sizeof(int16_t));
	printk("=== PCM_END ===\n");

	return 0;
}

bool audio_is_capturing(void)
{
	return audio_capturing;
}

#else /* !CONFIG_AUDIO_DMIC */

/* native_sim / QEMU M55 smoke-test stubs. Keep the public ABI identical
 * so callers in main.c don't need yet another #ifdef.
 */
int audio_init(void)
{
	return 0;
}

int audio_capture_start(void)
{
	return 0;
}

int audio_capture_stop(void)
{
	return 0;
}

bool audio_is_capturing(void)
{
	return false;
}

#endif /* CONFIG_AUDIO_DMIC */
