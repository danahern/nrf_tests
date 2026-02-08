/*
 * L2CAP CoC Throughput Test for nRF54L15
 *
 * Streams data over L2CAP Connection-Oriented Channel to bypass GATT/ATT
 * overhead. A small GATT service exposes the dynamically allocated PSM
 * so the central can discover which PSM to connect to.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/sys/printk.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define SDU_LEN          495
#define TX_BUF_COUNT     3
#define STATS_INTERVAL_MS 1000

/* PSM Discovery Service UUIDs */
#define BT_UUID_PSM_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF0)
#define BT_UUID_PSM_CHAR_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF1)

#define BT_UUID_PSM_SERVICE BT_UUID_DECLARE_128(BT_UUID_PSM_SERVICE_VAL)
#define BT_UUID_PSM_CHAR    BT_UUID_DECLARE_128(BT_UUID_PSM_CHAR_VAL)

/* L2CAP server and channel */
static struct bt_l2cap_server l2cap_server;
static struct bt_l2cap_le_chan l2cap_chan;
static struct bt_conn *current_conn;

/* TX flow control */
static struct k_sem tx_sem;

/* Stats */
static uint32_t bytes_sent;
static volatile bool l2cap_connected;
static volatile bool dle_ready;

static struct k_work_delayable conn_param_work;

/* TX buffer pool */
static void tx_buf_destroy(struct net_buf *buf)
{
	net_buf_destroy(buf);
}

NET_BUF_POOL_DEFINE(sdu_tx_pool, TX_BUF_COUNT, BT_L2CAP_SDU_BUF_SIZE(SDU_LEN),
		    CONFIG_BT_CONN_TX_USER_DATA_SIZE, tx_buf_destroy);

/* RX buffer pool for segmented SDU reassembly */
NET_BUF_POOL_DEFINE(sdu_rx_pool, 2, BT_L2CAP_SDU_BUF_SIZE(SDU_LEN),
		    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

/* Negotiated TX SDU size (may be less than SDU_LEN) */
static uint16_t tx_sdu_len;

/* Test data pattern */
static uint8_t tx_data[SDU_LEN];

/* ---- L2CAP Channel Callbacks ---- */

static void l2cap_chan_connected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_le_chan *le_chan =
		CONTAINER_OF(chan, struct bt_l2cap_le_chan, chan);

	printk("L2CAP channel connected: tx.mtu=%u tx.mps=%u rx.mtu=%u rx.mps=%u\n",
	       le_chan->tx.mtu, le_chan->tx.mps,
	       le_chan->rx.mtu, le_chan->rx.mps);

	/* Limit SDU size to negotiated TX MTU */
	tx_sdu_len = MIN(SDU_LEN, le_chan->tx.mtu);
	printk("Using TX SDU size: %u\n", tx_sdu_len);

	l2cap_connected = true;
	bytes_sent = 0;

	/* Allow multiple sends to keep the pipe full */
	for (int i = 0; i < TX_BUF_COUNT; i++) {
		k_sem_give(&tx_sem);
	}
}

static void l2cap_chan_disconnected(struct bt_l2cap_chan *chan)
{
	printk("L2CAP channel disconnected\n");
	l2cap_connected = false;
	k_sem_reset(&tx_sem);
}

static struct net_buf *l2cap_chan_alloc_buf(struct bt_l2cap_chan *chan)
{
	return net_buf_alloc(&sdu_rx_pool, K_NO_WAIT);
}

static int l2cap_chan_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	/* We don't expect RX data, but handle it gracefully */
	return 0;
}

static void l2cap_chan_sent(struct bt_l2cap_chan *chan)
{
	k_sem_give(&tx_sem);
}

static const struct bt_l2cap_chan_ops l2cap_chan_ops = {
	.connected = l2cap_chan_connected,
	.disconnected = l2cap_chan_disconnected,
	.alloc_buf = l2cap_chan_alloc_buf,
	.recv = l2cap_chan_recv,
	.sent = l2cap_chan_sent,
};

/* ---- L2CAP Server ---- */

static int l2cap_accept(struct bt_conn *conn, struct bt_l2cap_server *server,
			struct bt_l2cap_chan **chan)
{
	printk("L2CAP connection request\n");

	memset(&l2cap_chan, 0, sizeof(l2cap_chan));
	l2cap_chan.chan.ops = &l2cap_chan_ops;
	l2cap_chan.rx.mtu = SDU_LEN;

	*chan = &l2cap_chan.chan;
	return 0;
}

/* ---- PSM Discovery GATT Service ---- */

static ssize_t read_psm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	uint16_t psm = l2cap_server.psm;

	printk("PSM read: 0x%04X\n", psm);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &psm, sizeof(psm));
}

BT_GATT_SERVICE_DEFINE(psm_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_PSM_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_PSM_CHAR,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_psm, NULL, NULL),
);

/* ---- Advertising ---- */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_PSM_SERVICE_VAL),
};

/* ---- Connection Callbacks ---- */

static void conn_param_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	printk("Requesting PHY update to 2M...\n");

	int err = bt_conn_le_phy_update(current_conn, BT_CONN_LE_PHY_PARAM_2M);
	if (err) {
		printk("PHY update request failed (err %d)\n", err);
	}

	struct bt_conn_le_data_len_param dl_param = {
		.tx_max_len = 251,
		.tx_max_time = 2120,
	};
	err = bt_conn_le_data_len_update(current_conn, &dl_param);
	if (err) {
		printk("Data length update failed (err %d)\n", err);
	}

	struct bt_le_conn_param param = {
		.interval_min = 6,
		.interval_max = 12,
		.latency = 0,
		.timeout = 400,
	};
	err = bt_conn_le_param_update(current_conn, &param);
	if (err) {
		printk("Conn param update failed (err %d)\n", err);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		printk("Connection failed (err %u)\n", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Connected: %s\n", addr);
	current_conn = bt_conn_ref(conn);

	/* Stop advertising to free radio time for data transfer */
	bt_le_adv_stop();

	k_work_schedule(&conn_param_work, K_MSEC(50));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Disconnected: %s (reason %u)\n", addr, reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	k_work_cancel_delayable(&conn_param_work);
	l2cap_connected = false;
	dle_ready = false;
	bytes_sent = 0;
	k_sem_reset(&tx_sem);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	printk("Conn params updated: interval=%u (%.2f ms), latency=%u, timeout=%u\n",
	       interval, interval * 1.25f, latency, timeout);
}

static void le_phy_updated(struct bt_conn *conn,
			   struct bt_conn_le_phy_info *param)
{
	printk("PHY updated: TX=%u, RX=%u\n", param->tx_phy, param->rx_phy);
}

static void le_data_len_updated(struct bt_conn *conn,
				struct bt_conn_le_data_len_info *info)
{
	printk("Data Length updated: TX len=%u time=%u, RX len=%u time=%u\n",
	       info->tx_max_len, info->tx_max_time,
	       info->rx_max_len, info->rx_max_time);

	if (info->tx_max_len >= 251) {
		dle_ready = true;
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
	.le_phy_updated = le_phy_updated,
	.le_data_len_updated = le_data_len_updated,
};

/* ---- Stream Thread ---- */

void stream_thread(void)
{
	/* Init test data */
	for (int i = 0; i < SDU_LEN; i++) {
		tx_data[i] = i & 0xFF;
	}

	while (1) {
		if (!l2cap_connected || !dle_ready) {
			k_sleep(K_MSEC(100));
			continue;
		}

		/* Wait for a TX slot */
		k_sem_take(&tx_sem, K_FOREVER);

		if (!l2cap_connected) {
			continue;
		}

		struct net_buf *buf = net_buf_alloc(&sdu_tx_pool, K_MSEC(100));
		if (!buf) {
			k_sem_give(&tx_sem);
			continue;
		}

		net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
		net_buf_add_mem(buf, tx_data, tx_sdu_len);

		int ret = bt_l2cap_chan_send(&l2cap_chan.chan, buf);
		if (ret < 0) {
			net_buf_unref(buf);
			k_sem_give(&tx_sem);
			k_sleep(K_MSEC(10));
		} else {
			bytes_sent += tx_sdu_len;
		}
	}
}

/* ---- Stats Thread ---- */

void stats_thread(void)
{
	uint32_t prev_bytes = 0;

	while (1) {
		k_sleep(K_MSEC(STATS_INTERVAL_MS));

		if (l2cap_connected && dle_ready) {
			uint32_t delta = bytes_sent - prev_bytes;
			prev_bytes = bytes_sent;

			uint32_t kbps = (delta * 8) / STATS_INTERVAL_MS;

			printk("TX: %u bytes total, %u kbps\n", bytes_sent, kbps);
		}
	}
}

K_THREAD_DEFINE(stats_tid, 1024, stats_thread, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(stream_tid, 2048, stream_thread, NULL, NULL, NULL, 5, 0, 0);

/* ---- Main ---- */

int main(void)
{
	int err;

	printk("Starting nRF54L15 L2CAP CoC Throughput Test\n");

	k_sem_init(&tx_sem, 0, TX_BUF_COUNT);
	k_work_init_delayable(&conn_param_work, conn_param_work_handler);

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}
	printk("Bluetooth initialized\n");

	/* Register L2CAP server with dynamic PSM */
	l2cap_server.psm = 0;
	l2cap_server.sec_level = BT_SECURITY_L1;
	l2cap_server.accept = l2cap_accept;

	err = bt_l2cap_server_register(&l2cap_server);
	if (err) {
		printk("L2CAP server registration failed (err %d)\n", err);
		return 0;
	}
	printk("L2CAP server registered, PSM=0x%04X\n", l2cap_server.psm);

	/* Start advertising */
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Advertising failed (err %d)\n", err);
		return 0;
	}

	printk("Advertising started as '%s'\n", DEVICE_NAME);
	printk("Waiting for connection...\n");

	return 0;
}
