/*
 * PSE84 Voice Assistant — GATT data service.
 *
 * TX characteristic (notify): device → host (audio frames, state).
 * RX characteristic (write-without-response): host → device (text chunks).
 *
 * Both directions use the same |type|seq|len|payload| framing as the
 * UART link layer and the Python bridge's StreamingFrameParser.
 */

#include "gatt_svc.h"

#ifdef CONFIG_BT

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/conn.h>

#include "framing.h"
#include "state.h"
#include "ui.h"

LOG_MODULE_REGISTER(gatt_svc, LOG_LEVEL_INF);

#define ASST_SERVICE_UUID  BT_UUID_DECLARE_128(ASST_SERVICE_UUID_VAL)
#define ASST_TX_CHAR_UUID  BT_UUID_DECLARE_128(ASST_TX_CHAR_UUID_VAL)
#define ASST_RX_CHAR_UUID  BT_UUID_DECLARE_128(ASST_RX_CHAR_UUID_VAL)

static struct bt_conn *current_conn;
static bool tx_notifications_enabled;

#define PARSER_BUF_SZ 512
static frame_parser_t rx_parser;
static uint8_t rx_parser_buf[PARSER_BUF_SZ];

static void rx_frame_cb(uint8_t type, uint8_t seq,
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
		LOG_INF("TEXT_END via GATT");
		state_set(ASSIST_IDLE);
		break;
	case FRAME_TYPE_CTRL_STATE:
		if (len >= 1) {
			LOG_INF("CTRL_STATE %u from host", payload[0]);
		}
		break;
	default:
		LOG_DBG("gatt rx type=0x%02x len=%u", type, (unsigned)len);
		break;
	}
}

static ssize_t rx_write_cb(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len,
			   uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	frame_parser_feed(&rx_parser, buf, len, rx_frame_cb, NULL);
	return len;
}

static void tx_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	tx_notifications_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("TX notifications %s",
		tx_notifications_enabled ? "enabled" : "disabled");
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		return;
	}
	current_conn = bt_conn_ref(conn);
	frame_parser_reset(&rx_parser);
	LOG_INF("GATT client connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	tx_notifications_enabled = false;
	frame_parser_reset(&rx_parser);
	LOG_INF("GATT client disconnected");
}

BT_CONN_CB_DEFINE(gatt_svc_conn_cbs) = {
	.connected = connected,
	.disconnected = disconnected,
};

BT_GATT_SERVICE_DEFINE(asst_svc,
	BT_GATT_PRIMARY_SERVICE(ASST_SERVICE_UUID),

	/* TX characteristic: device → host via notify */
	BT_GATT_CHARACTERISTIC(ASST_TX_CHAR_UUID,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_changed,
		     BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* RX characteristic: host → device via write */
	BT_GATT_CHARACTERISTIC(ASST_RX_CHAR_UUID,
			       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, rx_write_cb, NULL),
);

int gatt_svc_init(void)
{
	frame_parser_init(&rx_parser, rx_parser_buf, sizeof(rx_parser_buf));
	printk("[gatt_svc] registered (TX notify + RX write)\n");
	return 0;
}

static atomic_t tx_sent;
static atomic_t tx_failed;

int gatt_svc_send(const uint8_t *data, uint16_t len)
{
	if (!current_conn || !tx_notifications_enabled) {
		atomic_inc(&tx_failed);
		return -ENOTCONN;
	}

	/* attrs[2] is the TX characteristic value; BT_GATT_SERVICE_DEFINE
	 * lays out: primary[0], char-decl[1], char-value[2], CCC[3]. */
	const struct bt_gatt_attr *tx_attr = &asst_svc.attrs[2];

	struct bt_gatt_notify_params params = {
		.attr = tx_attr,
		.data = data,
		.len = len,
	};

	int ret = bt_gatt_notify_cb(current_conn, &params);
	if (ret) {
		atomic_inc(&tx_failed);
	} else {
		atomic_inc(&tx_sent);
	}
	return ret;
}

void gatt_svc_log_stats(void)
{
	LOG_INF("gatt_svc tx: sent=%u failed=%u",
		(unsigned)atomic_get(&tx_sent),
		(unsigned)atomic_get(&tx_failed));
}

bool gatt_svc_is_connected(void)
{
	return current_conn != NULL && tx_notifications_enabled;
}

#else

int gatt_svc_init(void) { return 0; }
int gatt_svc_send(const uint8_t *data, uint16_t len) { return -ENOSYS; }
bool gatt_svc_is_connected(void) { return false; }

#endif
