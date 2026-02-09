/*
 * L2CAP CoC Throughput Central for nRF54L15
 *
 * Scans for the peripheral "nRF54L15_Test", connects, discovers PSM via GATT,
 * opens an L2CAP CoC channel, receives data, and prints throughput stats.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/sys/printk.h>

#define TARGET_NAME     "nRF54L15_Test"
#define TARGET_NAME_LEN (sizeof(TARGET_NAME) - 1)

#define SDU_LEN          2000
#define RX_MPS           247
#define INITIAL_CREDITS  80
#define STATS_INTERVAL_MS 1000

/* PSM Discovery Service UUIDs - must match peripheral */
#define BT_UUID_PSM_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF0)
#define BT_UUID_PSM_CHAR_VAL \
	BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789ABCDEF1)

#define BT_UUID_PSM_SERVICE BT_UUID_DECLARE_128(BT_UUID_PSM_SERVICE_VAL)
#define BT_UUID_PSM_CHAR    BT_UUID_DECLARE_128(BT_UUID_PSM_CHAR_VAL)

/* L2CAP channel */
static struct bt_l2cap_le_chan l2cap_chan;
static struct bt_conn *current_conn;

/* Stats */
static uint32_t rx_bytes;
static int64_t rx_start_time;
static volatile bool l2cap_connected;


/* GATT discovery state */
static struct bt_gatt_discover_params disc_params;
static struct bt_gatt_read_params read_params;
static uint16_t psm_char_handle;

/* Delayed connection setup */
static struct k_work_delayable conn_setup_work;
static struct k_work_delayable ci_update_work;

/* ---- L2CAP Channel Callbacks ---- */

static void l2cap_chan_connected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_le_chan *le_chan =
		CONTAINER_OF(chan, struct bt_l2cap_le_chan, chan);

	printk("L2CAP channel connected: tx.mtu=%u tx.mps=%u rx.mtu=%u rx.mps=%u\n",
	       le_chan->tx.mtu, le_chan->tx.mps,
	       le_chan->rx.mtu, le_chan->rx.mps);

	rx_bytes = 0;
	rx_start_time = k_uptime_get();
	l2cap_connected = true;

	/* Give additional credits now that channel is connected */
	bt_l2cap_chan_give_credits(chan, INITIAL_CREDITS);
}

static void l2cap_chan_disconnected(struct bt_l2cap_chan *chan)
{
	printk("L2CAP channel disconnected\n");
	l2cap_connected = false;
}

static uint32_t seg_count;

static void l2cap_chan_seg_recv(struct bt_l2cap_chan *chan, size_t sdu_len,
				off_t seg_offset, struct net_buf_simple *seg)
{
	rx_bytes += seg->len;
	seg_count++;

	/* Replenish credits in batches to reduce credit PDU overhead */
	if (l2cap_connected && (seg_count % 10 == 0)) {
		bt_l2cap_chan_give_credits(chan, 10);
	}
}

static const struct bt_l2cap_chan_ops l2cap_chan_ops = {
	.connected = l2cap_chan_connected,
	.disconnected = l2cap_chan_disconnected,
	.seg_recv = l2cap_chan_seg_recv,
};

/* ---- L2CAP Connect ---- */

static void l2cap_connect(uint16_t psm)
{
	int err;

	memset(&l2cap_chan, 0, sizeof(l2cap_chan));
	l2cap_chan.chan.ops = &l2cap_chan_ops;
	l2cap_chan.rx.mtu = SDU_LEN;
	l2cap_chan.rx.mps = RX_MPS;

	/* Give initial credits before connect - sent in connection request PDU */
	err = bt_l2cap_chan_give_credits(&l2cap_chan.chan, INITIAL_CREDITS);
	if (err) {
		printk("Initial credits failed (err %d)\n", err);
	}

	err = bt_l2cap_chan_connect(current_conn, &l2cap_chan.chan, psm);
	if (err) {
		printk("L2CAP connect failed (err %d)\n", err);
	} else {
		printk("L2CAP connect initiated (PSM=0x%04X, %u initial credits)\n",
		       psm, INITIAL_CREDITS);
	}
}

/* ---- GATT Discovery ---- */

static uint8_t gatt_read_psm_cb(struct bt_conn *conn, uint8_t err,
				 struct bt_gatt_read_params *params,
				 const void *data, uint16_t length)
{
	if (err) {
		printk("PSM read failed (err %u)\n", err);
		return BT_GATT_ITER_STOP;
	}

	if (!data || length < 2) {
		printk("PSM read: no data\n");
		return BT_GATT_ITER_STOP;
	}

	uint16_t psm = ((const uint8_t *)data)[0] |
		       (((const uint8_t *)data)[1] << 8);
	printk("Discovered PSM: 0x%04X\n", psm);

	l2cap_connect(psm);
	return BT_GATT_ITER_STOP;
}

static uint8_t gatt_discover_cb(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				struct bt_gatt_discover_params *params)
{
	if (!attr) {
		if (params->type == BT_GATT_DISCOVER_PRIMARY) {
			printk("PSM service not found\n");
		} else {
			printk("PSM characteristic not found\n");
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		struct bt_gatt_service_val *svc =
			(struct bt_gatt_service_val *)attr->user_data;

		printk("Found PSM service (handle %u-%u)\n",
		       attr->handle, svc->end_handle);

		disc_params.uuid = NULL;
		disc_params.start_handle = attr->handle + 1;
		disc_params.end_handle = svc->end_handle;
		disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		int err = bt_gatt_discover(conn, &disc_params);
		if (err) {
			printk("Characteristic discovery failed (err %d)\n", err);
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
		struct bt_gatt_chrc *chrc =
			(struct bt_gatt_chrc *)attr->user_data;

		if (bt_uuid_cmp(chrc->uuid, BT_UUID_PSM_CHAR) != 0) {
			return BT_GATT_ITER_CONTINUE;
		}

		psm_char_handle = chrc->value_handle;
		printk("Found PSM characteristic (value handle %u)\n",
		       psm_char_handle);

		read_params.func = gatt_read_psm_cb;
		read_params.handle_count = 1;
		read_params.single.handle = psm_char_handle;
		read_params.single.offset = 0;

		int err = bt_gatt_read(conn, &read_params);
		if (err) {
			printk("PSM read request failed (err %d)\n", err);
		}
		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_STOP;
}

static void start_gatt_discovery(void)
{
	int err;

	printk("Starting GATT discovery for PSM service...\n");

	disc_params.uuid = BT_UUID_PSM_SERVICE;
	disc_params.func = gatt_discover_cb;
	disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(current_conn, &disc_params);
	if (err) {
		printk("GATT discovery failed (err %d)\n", err);
	}
}

/* ---- Connection Setup (delayed) ---- */

static void ci_update_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	const struct bt_le_conn_param ci_param = {
		.interval_min = 40,  /* 50ms */
		.interval_max = 40,  /* 50ms */
		.latency = 0,
		.timeout = 400,
	};
	int err = bt_conn_le_param_update(current_conn, &ci_param);
	if (err) {
		printk("CI update to 15ms failed (err %d)\n", err);
	} else {
		printk("Requested CI update to 15ms\n");
	}
}

static void conn_setup_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	int err;

	struct bt_conn_le_data_len_param dl_param = {
		.tx_max_len = 251,
		.tx_max_time = 2120,
	};
	err = bt_conn_le_data_len_update(current_conn, &dl_param);
	if (err) {
		printk("Data length update request failed (err %d)\n", err);
	}

	const struct bt_conn_le_phy_param phy_param = {
		.options = BT_CONN_LE_PHY_OPT_NONE,
		.pref_tx_phy = BT_GAP_LE_PHY_2M,
		.pref_rx_phy = BT_GAP_LE_PHY_2M,
	};
	err = bt_conn_le_phy_update(current_conn, &phy_param);
	if (err) {
		printk("PHY update request failed (err %d)\n", err);
	}

	start_gatt_discovery();
}

/* ---- Connection Callbacks ---- */

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		printk("Connection failed (err %u)\n", err);
		current_conn = NULL;
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Connected: %s\n", addr);
	current_conn = bt_conn_ref(conn);

	struct bt_conn_info info;
	if (bt_conn_get_info(conn, &info) == 0) {
		printk("Initial params: interval=%u (%u.%u ms), latency=%u, timeout=%u\n",
		       info.le.interval,
		       info.le.interval * 125 / 100,
		       (info.le.interval * 125 % 100),
		       info.le.latency, info.le.timeout);
	}

	k_work_schedule(&conn_setup_work, K_MSEC(100));
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

	k_work_cancel_delayable(&conn_setup_work);
	k_work_cancel_delayable(&ci_update_work);
	l2cap_connected = false;
	rx_bytes = 0;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	printk("Conn params updated: interval=%u (%u.%u ms), latency=%u, timeout=%u\n",
	       interval, interval * 125 / 100, (interval * 125 % 100),
	       latency, timeout);
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

	(void)info; /* CI update done via delayed work */
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
	.le_phy_updated = le_phy_updated,
	.le_data_len_updated = le_data_len_updated,
};

/* ---- Scanning ---- */

static bool name_matches(struct bt_data *data, void *user_data)
{
	bool *found = user_data;

	if (data->type == BT_DATA_NAME_COMPLETE &&
	    data->data_len == TARGET_NAME_LEN &&
	    memcmp(data->data, TARGET_NAME, TARGET_NAME_LEN) == 0) {
		*found = true;
		return false;
	}
	return true;
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi,
		    uint8_t type, struct net_buf_simple *ad)
{
	bool found = false;
	char addr_str[BT_ADDR_LE_STR_LEN];
	int err;

	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	bt_data_parse(ad, name_matches, &found);
	if (!found) {
		return;
	}

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
	printk("Found peripheral: %s (RSSI %d)\n", addr_str, rssi);

	err = bt_le_scan_stop();
	if (err) {
		printk("Scan stop failed (err %d)\n", err);
		return;
	}

	struct bt_conn_le_create_param create_param = {
		.options = BT_CONN_LE_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
		.interval_coded = 0,
		.window_coded = 0,
		.timeout = 0,
	};
	struct bt_le_conn_param conn_param = {
		.interval_min = 40,  /* 50ms */
		.interval_max = 40,  /* 50ms */
		.latency = 0,
		.timeout = 400,
	};

	struct bt_conn *conn;
	err = bt_conn_le_create(addr, &create_param, &conn_param, &conn);
	if (err) {
		printk("Connection create failed (err %d)\n", err);
		return;
	}
	bt_conn_unref(conn);
	printk("Connecting...\n");
}

/* ---- Stats Thread ---- */

void stats_thread(void)
{
	uint32_t prev_bytes = 0;

	while (1) {
		k_sleep(K_MSEC(STATS_INTERVAL_MS));

		if (l2cap_connected) {
			uint32_t cur_bytes = rx_bytes;
			uint32_t delta = cur_bytes - prev_bytes;
			prev_bytes = cur_bytes;

			uint32_t kbps = (delta * 8) / STATS_INTERVAL_MS;

			int64_t elapsed_ms = k_uptime_get() - rx_start_time;
			uint32_t avg_kbps = 0;
			if (elapsed_ms > 0) {
				avg_kbps = (uint32_t)((uint64_t)cur_bytes * 8000 /
						      elapsed_ms / 1000);
			}

			uint32_t elapsed_s = (uint32_t)(elapsed_ms / 1000);
			uint32_t elapsed_frac = (uint32_t)((elapsed_ms % 1000) / 100);
			printk("RX: %u kbps (avg: %u kbps) | %u bytes in %u.%us\n",
			       kbps, avg_kbps, cur_bytes, elapsed_s, elapsed_frac);
		}
	}
}

K_THREAD_DEFINE(stats_tid, 2048, stats_thread, NULL, NULL, NULL, 7, 0, 0);

/* ---- Main ---- */

int main(void)
{
	int err;

	printk("Starting nRF54L15 L2CAP CoC Central\n");

	k_work_init_delayable(&conn_setup_work, conn_setup_work_handler);
	k_work_init_delayable(&ci_update_work, ci_update_work_handler);

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}
	printk("Bluetooth initialized\n");

	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	err = bt_le_scan_start(&scan_param, scan_cb);
	if (err) {
		printk("Scan start failed (err %d)\n", err);
		return 0;
	}

	printk("Scanning for '%s'...\n", TARGET_NAME);

	return 0;
}
