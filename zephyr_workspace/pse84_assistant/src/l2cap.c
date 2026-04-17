/*
 * PSE84 Voice Assistant — L2CAP CoC data channel.
 *
 * Registers a server on LE_PSM 0x0080. When macOS connects and opens
 * a CoC channel, incoming SDUs are fed through the framing parser
 * (same link_frame_cb dispatch that the UART link layer uses).
 *
 * TX path: l2cap_send_frame() copies caller data into a net_buf and
 * calls bt_l2cap_chan_send(). Flow-controlled via a semaphore so the
 * caller blocks until the previous send completes (the BT stack calls
 * the .sent callback which posts the semaphore).
 *
 * Pattern adapted from nrf54l15_l2cap_test + Zephyr l2cap_coc_acceptor
 * sample. See docs/pse84_context_reload.md §13 for the full pipeline.
 */

#include "l2cap.h"

#ifdef CONFIG_BT_L2CAP_DYNAMIC_CHANNEL

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/net_buf.h>

#include "framing.h"
#include "link.h"
#include "state.h"
#include "ui.h"

LOG_MODULE_REGISTER(l2cap, LOG_LEVEL_INF);

#define L2CAP_RX_MTU  247
#define L2CAP_TX_MTU  247
#define TX_BUF_COUNT  4
#define PARSER_BUF_SZ 512

NET_BUF_POOL_DEFINE(l2cap_tx_pool, TX_BUF_COUNT,
		    BT_L2CAP_SDU_BUF_SIZE(L2CAP_TX_MTU),
		    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static struct bt_l2cap_le_chan l2cap_chan;
static bool channel_connected;
static K_SEM_DEFINE(tx_sem, TX_BUF_COUNT, TX_BUF_COUNT);

static frame_parser_t l2cap_parser;
static uint8_t l2cap_parser_buf[PARSER_BUF_SZ];

static void l2cap_frame_cb(uint8_t type, uint8_t seq,
			   const uint8_t *payload, uint16_t len,
			   void *user)
{
	ARG_UNUSED(seq);
	ARG_UNUSED(user);

	switch (type) {
	case FRAME_TYPE_TEXT_CHUNK:
		if (state_get() != ASSIST_RESPONDING) {
			ui_clear_reply_text();
			state_set(ASSIST_RESPONDING);
		}
		ui_append_reply_text((const char *)payload, (int)len);
		break;
	case FRAME_TYPE_TEXT_END:
		LOG_INF("TEXT_END via L2CAP — holding RESPONDING for 5 s");
		link_schedule_idle_revert();
		break;
	case FRAME_TYPE_CTRL_STATE: {
		if (len >= 1) {
			LOG_INF("CTRL_STATE %u from host", payload[0]);
		}
		break;
	}
	default:
		LOG_DBG("l2cap rx type=0x%02x len=%u", type, (unsigned)len);
		break;
	}
}

static int l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	LOG_DBG("l2cap rx %u bytes", buf->len);
	frame_parser_feed(&l2cap_parser, buf->data, buf->len,
			  l2cap_frame_cb, NULL);
	return 0;
}

static void l2cap_connected_cb(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_le_chan *le =
		CONTAINER_OF(chan, struct bt_l2cap_le_chan, chan);
	LOG_INF("L2CAP connected: tx.mtu=%u tx.mps=%u rx.mtu=%u rx.mps=%u",
		le->tx.mtu, le->tx.mps, le->rx.mtu, le->rx.mps);
	channel_connected = true;
	frame_parser_reset(&l2cap_parser);
}

static void l2cap_disconnected_cb(struct bt_l2cap_chan *chan)
{
	LOG_INF("L2CAP disconnected");
	channel_connected = false;
	frame_parser_reset(&l2cap_parser);
	k_sem_reset(&tx_sem);
	k_sem_init(&tx_sem, TX_BUF_COUNT, TX_BUF_COUNT);
}

static void l2cap_sent_cb(struct bt_l2cap_chan *chan)
{
	k_sem_give(&tx_sem);
}

static const struct bt_l2cap_chan_ops l2cap_ops = {
	.recv = l2cap_recv,
	.connected = l2cap_connected_cb,
	.disconnected = l2cap_disconnected_cb,
	.sent = l2cap_sent_cb,
};

static int l2cap_accept(struct bt_conn *conn,
			struct bt_l2cap_server *server,
			struct bt_l2cap_chan **chan)
{
	if (channel_connected) {
		LOG_WRN("l2cap_accept: already connected, rejecting");
		return -ENOMEM;
	}
	memset(&l2cap_chan, 0, sizeof(l2cap_chan));
	l2cap_chan.chan.ops = &l2cap_ops;
	l2cap_chan.rx.mtu = L2CAP_RX_MTU;
	*chan = &l2cap_chan.chan;
	LOG_INF("l2cap_accept: channel assigned");
	return 0;
}

static struct bt_l2cap_server l2cap_server = {
	.psm = L2CAP_PSM,
	.sec_level = BT_SECURITY_L1,
	.accept = l2cap_accept,
};

int l2cap_init(void)
{
	frame_parser_init(&l2cap_parser, l2cap_parser_buf,
			  sizeof(l2cap_parser_buf));

	int err = bt_l2cap_server_register(&l2cap_server);
	if (err) {
		printk("[l2cap] server register FAILED: %d\n", err);
		return err;
	}
	printk("[l2cap] server registered on PSM 0x%04x\n", L2CAP_PSM);
	return 0;
}

int l2cap_send_frame(const uint8_t *data, uint16_t len)
{
	if (!channel_connected) {
		return -ENOTCONN;
	}

	if (k_sem_take(&tx_sem, K_MSEC(500)) != 0) {
		LOG_WRN("l2cap_send_frame: tx sem timeout");
		return -EAGAIN;
	}

	struct net_buf *buf = net_buf_alloc(&l2cap_tx_pool, K_MSEC(100));
	if (!buf) {
		k_sem_give(&tx_sem);
		return -ENOMEM;
	}

	net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, data, MIN(len, L2CAP_TX_MTU));

	int ret = bt_l2cap_chan_send(&l2cap_chan.chan, buf);
	if (ret < 0) {
		LOG_ERR("bt_l2cap_chan_send failed: %d", ret);
		net_buf_unref(buf);
		k_sem_give(&tx_sem);
		return ret;
	}
	return 0;
}

bool l2cap_is_connected(void)
{
	return channel_connected;
}

#else /* !CONFIG_BT_L2CAP_DYNAMIC_CHANNEL */

#include <errno.h>

int l2cap_init(void) { return 0; }
int l2cap_send_frame(const uint8_t *data, uint16_t len) { return -ENOSYS; }
bool l2cap_is_connected(void) { return false; }

#endif
