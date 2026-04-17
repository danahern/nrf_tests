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

#include "opus_wrapper.h"
#include "framing.h"
#include "gatt_svc.h"
#ifdef CONFIG_APP_AUDIO_EMIT_OPUS
#define OPUS_MAX_BYTES     320  /* per-frame ceiling (libopus safe upper bound at 16 kbps) */
#endif

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

/*
 * 2 s mono s16 ring buffer. 64 KB — pinned at SOCMEM_AUDIO when that
 * node exists (so BT host has the on-chip SRAM); otherwise BSS.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(socmem_audio))
static int16_t * const audio_ring =
	(int16_t *)DT_REG_ADDR(DT_NODELABEL(socmem_audio));
#else
static int16_t audio_ring[AUDIO_RING_BYTES / sizeof(int16_t)];
#endif
static size_t audio_ring_samples;  /* samples currently filled */
static int64_t audio_capture_start_ms;
static int64_t audio_capture_stop_ms;

static const struct device *dmic_dev;
static bool audio_configured;
static bool audio_capturing;

/* Opus encoder for BLE streaming. 20 ms frames (320 samples @ 16 kHz
 * mono), 16 kbps CBR VOIP. One 100 ms DMIC block = 5 Opus frames.
 * Output bound: ~40 B/frame at 16 kbps; cap at 128 B with headroom. */
#define OPUS_FRAME_SAMPLES    320
#define OPUS_MAX_PACKET_BYTES 128
static opus_wrap_ctx_t *opus_enc;
static uint8_t opus_out_seq;

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

static K_SEM_DEFINE(dmic_running_sem, 0, 1);

static void audio_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Block until audio_dmic_kickoff posts the semaphore. Before
	 * that point dmic_read would return -EIO (state != ACTIVE) and
	 * we'd log hundreds of spurious errors during the BT fw
	 * download window. */
	k_sem_take(&dmic_running_sem, K_FOREVER);

	/* DMIC runs continuously after audio_init() — see the comment on
	 * audio_capture_start(). This thread pulls every block out of the
	 * driver forever, and only COPIES to audio_ring when a capture is
	 * active. That keeps the PDM DC-blocker warm between captures,
	 * eliminating the ~800 ms settling transient that otherwise dominated
	 * each 2 s capture window (peak=27842 first-sample artifact).
	 */
	uint32_t total_blocks = 0;
	uint32_t copied_blocks = 0;
	uint32_t err_timeout = 0;
	uint32_t err_other = 0;
	int64_t last_log_ms = 0;

	while (1) {
		void *buffer;
		uint32_t size;
		int ret = dmic_read(dmic_dev, 0, &buffer, &size,
				    AUDIO_BLOCK_DURATION_MS + 50);

		if (ret < 0) {
			if (ret == -EAGAIN || ret == -ETIMEDOUT) {
				err_timeout++;
				continue;
			}
			LOG_ERR("dmic_read failed: %d", ret);
			err_other++;
			k_sleep(K_MSEC(50));
			continue;
		}

		total_blocks++;

		if (audio_capturing) {
			/* Skip the first 2 samples of each block — DMA/driver
			 * writes them at init (s[0]=-26184, s[1]=9747 on this
			 * kit) and never overwrites them. They pollute peak
			 * calculation and corrupt the first 4 bytes of every
			 * 100 ms window's worth of real audio.
			 */
			const size_t skip_bytes = 2 * sizeof(int16_t);
			const uint8_t *src =
				(const uint8_t *)buffer + skip_bytes;
			const size_t src_bytes =
				(size > skip_bytes) ? (size - skip_bytes) : 0;
			const size_t remaining_bytes =
				(size_t)AUDIO_RING_BYTES -
				(audio_ring_samples * sizeof(int16_t));
			const size_t copy_bytes =
				MIN(src_bytes, remaining_bytes);

			if (copy_bytes > 0) {
				memcpy((uint8_t *)audio_ring +
					       audio_ring_samples * sizeof(int16_t),
				       src, copy_bytes);
				audio_ring_samples += copy_bytes / sizeof(int16_t);
				copied_blocks++;
			}

			/* Opus-encode and stream over BLE. 100 ms block =
			 * 5 × 20 ms Opus frames. Each frame goes as one
			 * FRAME_TYPE_AUDIO SDU. If no BLE peer subscribed,
			 * gatt_svc_send returns -ENOTCONN and we drop. */
			if (opus_enc != NULL && gatt_svc_is_connected()) {
				const int16_t *pcm =
					(const int16_t *)((const uint8_t *)buffer +
							  skip_bytes);
				size_t pcm_samples =
					(src_bytes) / sizeof(int16_t);
				size_t nframes =
					pcm_samples / OPUS_FRAME_SAMPLES;

				for (size_t f = 0; f < nframes; f++) {
					uint8_t opus_pkt[OPUS_MAX_PACKET_BYTES];
					int n = opus_wrap_encode_frame(
						opus_enc,
						pcm + f * OPUS_FRAME_SAMPLES,
						OPUS_FRAME_SAMPLES,
						opus_pkt, sizeof(opus_pkt));
					if (n <= 0) {
						continue;
					}
					uint8_t framed[FRAME_HEADER_LEN +
						       OPUS_MAX_PACKET_BYTES];
					int fn = frame_encode(
						FRAME_TYPE_AUDIO,
						opus_out_seq++,
						opus_pkt, (uint16_t)n,
						framed, sizeof(framed));
					if (fn > 0) {
						(void)gatt_svc_send(
							framed, (uint16_t)fn);
					}
				}
			}
		}

		k_mem_slab_free(&audio_mem_slab, buffer);

		/* Heartbeat once a second so we can see the thread is alive
		 * and measure read rate vs copy rate. Also peek at the first
		 * sample of the just-received buffer to confirm fresh data.
		 */
		int64_t now = k_uptime_get();

		if (now - last_log_ms >= 1000) {
			const int16_t *s = (const int16_t *)buffer;
			const size_t n = size / sizeof(int16_t);
			int32_t peak = 0;

			for (size_t k = 0; k < n; k++) {
				int32_t a = s[k] < 0 ? -s[k] : s[k];

				if (a > peak) peak = a;
			}
			LOG_INF("audio hb: blks=%u cop=%u to=%u o=%u | "
				"buf=%p sz=%u s[0]=%d s[1]=%d s[800]=%d s[-1]=%d pk=%ld cap=%d",
				(unsigned)total_blocks, (unsigned)copied_blocks,
				(unsigned)err_timeout, (unsigned)err_other,
				buffer, (unsigned)size, (int)s[0], (int)s[1],
				(int)s[800], (int)s[n - 1], (long)peak,
				(int)audio_capturing);
			last_log_ms = now;
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

	/* Persistent Opus encoder for BLE streaming. 16 kHz / mono /
	 * 16 kbps CBR — matches host bridge decode config. */
	opus_enc = opus_wrap_init(AUDIO_SAMPLE_RATE_HZ, AUDIO_CHANNELS, 16000);
	if (opus_enc == NULL) {
		LOG_WRN("opus_wrap_init failed — BLE audio disabled");
	}

	audio_thread_id = k_thread_create(&audio_thread, audio_thread_stack,
					  K_THREAD_STACK_SIZEOF(audio_thread_stack),
					  audio_thread_fn, NULL, NULL, NULL,
					  AUDIO_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(audio_thread_id, "audio");

	audio_configured = true;
	LOG_INF("audio_init ok (dmic=%s, %d Hz mono s16, ring=%d bytes) — "
		"waiting for audio_dmic_kickoff() to start PDM hardware",
		dmic_dev->name, AUDIO_SAMPLE_RATE_HZ, (int)AUDIO_RING_BYTES);
	return 0;
}

static bool dmic_running;

#include "ble.h"

#define AUDIO_KICKOFF_THREAD_STACK_SIZE 1024
#define AUDIO_KICKOFF_THREAD_PRIORITY   10
static K_THREAD_STACK_DEFINE(audio_kickoff_stack, AUDIO_KICKOFF_THREAD_STACK_SIZE);
static struct k_thread audio_kickoff_thread_data;

static void audio_kickoff_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	(void)ble_wait_ready(K_FOREVER);
	(void)audio_dmic_kickoff();
}

void audio_kickoff_thread_start(void)
{
	k_thread_create(&audio_kickoff_thread_data, audio_kickoff_stack,
			K_THREAD_STACK_SIZEOF(audio_kickoff_stack),
			audio_kickoff_fn, NULL, NULL, NULL,
			AUDIO_KICKOFF_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&audio_kickoff_thread_data, "audio_kickoff");
}

int audio_dmic_kickoff(void)
{
	if (dmic_running) {
		return 0;
	}
	if (!audio_configured) {
		LOG_ERR("audio_dmic_kickoff called before audio_init");
		return -EINVAL;
	}

	/* Start DMIC once and leave it running. The capture thread drains
	 * every block forever; audio_capture_start/stop just gate which
	 * blocks are copied into the ring. This keeps the PDM DC-blocker
	 * warm so captures start from a settled baseline instead of the
	 * big initial filter-state transient. See
	 * zephyr_workspace/pse84_dmic_test for the diagnostic that proved
	 * the settling takes ~800 ms of silence per START.
	 */
	int ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("DMIC_TRIGGER_START (warm-up) failed: %d", ret);
		return ret;
	}
	dmic_running = true;
	k_sem_give(&dmic_running_sem);
	LOG_INF("audio_dmic_kickoff ok — DMA streaming started");
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

	/* DMIC is already running; just open the ring for writes. Zero the
	 * ring so short presses that don't fill min_samples worth of audio
	 * don't leak stale data from a previous capture through the padding.
	 */
	memset(audio_ring, 0, sizeof(audio_ring));
	audio_ring_samples = 0;
	audio_capture_start_ms = k_uptime_get();
	opus_out_seq = 0;
	audio_capturing = true;

	/* Tell the host to start buffering audio. The host uses this to
	 * mark the beginning of a capture window for Whisper. */
	if (gatt_svc_is_connected()) {
		uint8_t framed[FRAME_HEADER_LEN];
		int fn = frame_encode(FRAME_TYPE_CTRL_START_LISTEN, 0,
				      NULL, 0, framed, sizeof(framed));
		if (fn > 0) {
			(void)gatt_svc_send(framed, (uint16_t)fn);
		}
	}

	LOG_INF("audio capture START (ring armed, opus seq reset)");
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
		/* Workqueue is cooperative (prio -1), audio_thread is
		 * preemptible (prio 5). k_yield would only rotate among
		 * cooperative threads and still starve the preemptible
		 * audio_thread, so we need an actual time-slice via
		 * k_msleep. Every 8 lines so the added latency is bounded
		 * (~100 ms per 2 s dump) while still giving the DMA queue
		 * enough chances to drain (queue depth is 8 blocks × 100 ms
		 * each, so one slice every 8 printk lines keeps us well
		 * under the depth).
		 */
		if ((i & 0x7) == 0) {
			k_msleep(1);
		}
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

	/* Flip the flag — the thread will stop copying new blocks into the
	 * ring on its next read. DMIC stays running to keep the DC blocker
	 * warm for the next capture.
	 */
	audio_capturing = false;
	audio_capture_stop_ms = k_uptime_get();

	/* Signal end-of-utterance so the host can hand the buffered audio
	 * to Whisper and start the LLM call. */
	if (gatt_svc_is_connected()) {
		uint8_t framed[FRAME_HEADER_LEN];
		int fn = frame_encode(FRAME_TYPE_CTRL_STOP_LISTEN, 0,
				      NULL, 0, framed, sizeof(framed));
		if (fn > 0) {
			(void)gatt_svc_send(framed, (uint16_t)fn);
		}
	}

	/* Give the thread one block's worth of time to drain its pending
	 * dmic_read() so we don't race on audio_ring_samples.
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

#ifdef CONFIG_APP_AUDIO_EMIT_OPUS
	/* Phase 3: Opus-encode the same buffer into 20 ms frames and emit
	 * alongside the PCM dump. Host-side receive_pcm.py picks up the
	 * OPUS block opportunistically, decodes, and writes a second WAV.
	 * Keeps the PCM_BEGIN/END payload intact so existing parsers keep
	 * working. Encoder state is stack-allocated (large-ish; ~20 KB) —
	 * only instantiated on capture-stop so the steady-state M55 RAM
	 * doesn't carry it.
	 */
	opus_wrap_ctx_t *enc =
		opus_wrap_init(AUDIO_SAMPLE_RATE_HZ, AUDIO_CHANNELS, 16000);
	if (enc == NULL) {
		LOG_WRN("OPUS encoder init failed, skipping OPUS dump");
	} else {
		const size_t full_frames = audio_ring_samples / OPUS_FRAME_SAMPLES;
		size_t total_opus_bytes = 0;
		printk("=== OPUS_BEGIN frames=%u frame_samples=%d sample_rate=%d "
		       "bitrate=16000 ===\n",
		       (unsigned)full_frames, OPUS_FRAME_SAMPLES,
		       AUDIO_SAMPLE_RATE_HZ);
		uint8_t enc_buf[OPUS_MAX_BYTES];
		for (size_t f = 0; f < full_frames; f++) {
			const int16_t *pcm = &audio_ring[f * OPUS_FRAME_SAMPLES];
			int n = opus_wrap_encode_frame(enc, pcm, OPUS_FRAME_SAMPLES,
						       enc_buf, sizeof(enc_buf));
			if (n <= 0) {
				LOG_WRN("OPUS encode frame %u failed: %d",
					(unsigned)f, n);
				continue;
			}
			/* u16 LE length prefix per frame, then the opus bytes,
			 * line-wrapped identically to PCM hex (64 chars/line).
			 */
			uint8_t len_prefix[2] = { (uint8_t)(n & 0xff),
						  (uint8_t)((n >> 8) & 0xff) };
			audio_hex_dump(len_prefix, 2);
			audio_hex_dump(enc_buf, (size_t)n);
			total_opus_bytes += (size_t)n;
		}
		printk("=== OPUS_END ===\n");
		const size_t raw_bytes = full_frames * OPUS_FRAME_SAMPLES *
					 sizeof(int16_t);
		LOG_INF("OPUS emit: frames=%u total_bytes=%u ratio=%u.%02ux",
			(unsigned)full_frames, (unsigned)total_opus_bytes,
			(unsigned)(raw_bytes / (total_opus_bytes + 1)),
			(unsigned)((raw_bytes * 100 /
				    (total_opus_bytes + 1)) % 100));
		opus_wrap_free(enc);
	}
#endif /* CONFIG_APP_AUDIO_EMIT_OPUS */

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
