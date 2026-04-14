/*
 * PSE84 Voice Assistant — host link layer.
 *
 * See link.h for the public contract.
 *
 * Flow:
 *   - link_init() grabs the console UART, installs an IRQ RX callback,
 *     primes frame_parser_t, creates a worker thread for dispatch.
 *   - IRQ callback drains bytes into a small lockless ring buffer (SPSC).
 *   - Worker thread calls frame_parser_feed() which dispatches via
 *     link_frame_cb to the appropriate handler:
 *       TEXT_CHUNK -> ui_append_reply_text + state_set(RESPONDING)
 *       TEXT_END   -> state_set(IDLE) + ui_clear_reply_text on next LISTEN
 *       anything else -> logged and dropped
 */

#include "link.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

#include "framing.h"
#include "state.h"
#include "ui.h"

LOG_MODULE_REGISTER(link, LOG_LEVEL_INF);

#define LINK_RING_SIZE        512   /* bytes */
#define LINK_PARSE_STORAGE    2048  /* max inflight frame payload */
#define LINK_THREAD_STACK_SZ  2048
#define LINK_THREAD_PRIORITY  6

static const struct device *link_uart;

/* SPSC ring: IRQ is producer, worker thread is consumer. */
static uint8_t link_ring[LINK_RING_SIZE];
static atomic_t link_ring_head;   /* next write */
static atomic_t link_ring_tail;   /* next read */

static frame_parser_t link_parser;
static uint8_t link_parser_storage[LINK_PARSE_STORAGE];

static K_THREAD_STACK_DEFINE(link_thread_stack, LINK_THREAD_STACK_SZ);
static struct k_thread link_thread;
static k_tid_t link_thread_id;
static K_SEM_DEFINE(link_sem, 0, 1);

/* After TEXT_END we stay in RESPONDING for RESPONDING_HOLD_MS so the
 * user sees the full reply, then auto-transition to IDLE. A button
 * press in the meantime pre-empts this via main.c's press handler
 * (which sets LISTENING directly + cancels our delayed work).
 */
#define RESPONDING_HOLD_MS 5000
static void link_idle_revert_handler(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(link_idle_revert, link_idle_revert_handler);

static void link_idle_revert_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	if (state_get() == ASSIST_RESPONDING) {
		state_set(ASSIST_IDLE);
	}
}

void link_cancel_idle_revert(void)
{
	k_work_cancel_delayable(&link_idle_revert);
}

static size_t ring_count(void)
{
	int h = atomic_get(&link_ring_head);
	int t = atomic_get(&link_ring_tail);

	return (h - t + LINK_RING_SIZE) % LINK_RING_SIZE;
}

static void ring_push_byte(uint8_t b)
{
	int h = atomic_get(&link_ring_head);
	int next_h = (h + 1) % LINK_RING_SIZE;
	int t = atomic_get(&link_ring_tail);

	if (next_h == t) {
		/* Ring full — drop oldest to preserve real-time streaming.
		 * For TEXT_CHUNK traffic this should never happen at 460800
		 * baud (46 KB/s) vs the thread consuming in ms.
		 */
		atomic_set(&link_ring_tail, (t + 1) % LINK_RING_SIZE);
	}
	link_ring[h] = b;
	atomic_set(&link_ring_head, next_h);
}

static atomic_t link_irq_count;
static atomic_t link_bytes_count;
static atomic_t link_frames_count;

static void link_uart_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	atomic_inc(&link_irq_count);
	uart_irq_update(dev);
	if (!uart_irq_rx_ready(dev)) {
		return;
	}
	uint8_t buf[64];
	int n = uart_fifo_read(dev, buf, sizeof(buf));

	for (int i = 0; i < n; i++) {
		ring_push_byte(buf[i]);
	}
	if (n > 0) {
		atomic_add(&link_bytes_count, n);
		k_sem_give(&link_sem);
	}
}

static void link_frame_cb(uint8_t type, uint8_t seq, const uint8_t *payload,
			  uint16_t len, void *user)
{
	ARG_UNUSED(seq);
	ARG_UNUSED(user);
	atomic_inc(&link_frames_count);
	switch (type) {
	case FRAME_TYPE_TEXT_CHUNK:
		/* First chunk of a new reply: transition into RESPONDING
		 * (also clears any stale text, keeping idempotency if host
		 * already transitioned us via a CTRL_STATE frame).
		 */
		if (state_get() != ASSIST_RESPONDING) {
			ui_clear_reply_text();
			state_set(ASSIST_RESPONDING);
		}
		ui_append_reply_text((const char *)payload, (int)len);
		break;
	case FRAME_TYPE_TEXT_END:
		LOG_INF("TEXT_END (reply complete) — RESPONDING hold %d ms",
			RESPONDING_HOLD_MS);
		/* Stay in RESPONDING for RESPONDING_HOLD_MS so the user sees
		 * the full reply, then auto-transition to IDLE. A new press
		 * in the meantime cancels this work.
		 */
		k_work_schedule(&link_idle_revert, K_MSEC(RESPONDING_HOLD_MS));
		break;
	case FRAME_TYPE_CTRL_STATE:
	case FRAME_TYPE_CTRL_START_LISTEN:
	case FRAME_TYPE_CTRL_STOP_LISTEN:
	case FRAME_TYPE_AUDIO:
		/* Host shouldn't send these in the PoC; just log. */
		LOG_DBG("ignored frame type=0x%02x len=%u", type, (unsigned)len);
		break;
	default:
		LOG_WRN("unknown frame type=0x%02x", type);
		break;
	}
}

static void link_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint8_t tmp[128];
	int64_t last_hb_ms = k_uptime_get();

	while (1) {
		/* Heartbeat every 2 s so we can see from serial whether the
		 * UART IRQ is firing and frames are being parsed. Wake every
		 * 500 ms to check even if the ring is empty.
		 */
		int ret = k_sem_take(&link_sem, K_MSEC(500));

		int64_t now = k_uptime_get();

		if (now - last_hb_ms >= 2000) {
			LOG_INF("link hb: irq=%u bytes=%u frames=%u ring=%u",
				(unsigned)atomic_get(&link_irq_count),
				(unsigned)atomic_get(&link_bytes_count),
				(unsigned)atomic_get(&link_frames_count),
				(unsigned)ring_count());
			last_hb_ms = now;
		}
		if (ret != 0) {
			continue;
		}

		while (ring_count() > 0) {
			/* Drain into tmp[] — up to 128 bytes at a time. */
			int t = atomic_get(&link_ring_tail);
			int n = 0;

			while (n < (int)sizeof(tmp) && ring_count() > 0) {
				tmp[n++] = link_ring[t];
				t = (t + 1) % LINK_RING_SIZE;
				atomic_set(&link_ring_tail, t);
			}
			if (n > 0) {
				frame_parser_feed(&link_parser, tmp, n,
						  link_frame_cb, NULL);
			}
		}
	}
}

int link_init(void)
{
	link_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(link_uart)) {
		LOG_ERR("console UART not ready");
		return -ENODEV;
	}

	(void)frame_parser_init(&link_parser, link_parser_storage,
				sizeof(link_parser_storage));

	uart_irq_rx_disable(link_uart);
	uart_irq_tx_disable(link_uart);
	int ret = uart_irq_callback_set(link_uart, link_uart_cb);

	if (ret < 0) {
		LOG_ERR("uart_irq_callback_set failed: %d", ret);
		return ret;
	}
	uart_irq_rx_enable(link_uart);

	link_thread_id = k_thread_create(&link_thread, link_thread_stack,
					 K_THREAD_STACK_SIZEOF(link_thread_stack),
					 link_thread_fn, NULL, NULL, NULL,
					 LINK_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(link_thread_id, "link");
	LOG_INF("link_init ok (uart=%s, parser_cap=%d)", link_uart->name,
		LINK_PARSE_STORAGE);
	return 0;
}
