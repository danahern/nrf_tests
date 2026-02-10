/*
 * GATT Notification Throughput Central for nRF54L15
 *
 * Scans for "nRF54L15_Test", connects, exchanges MTU, discovers the
 * NUS TX characteristic, subscribes to notifications, and measures throughput.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/printk.h>

#define TARGET_NAME     "nRF54L15_Test"
#define TARGET_NAME_LEN (sizeof(TARGET_NAME) - 1)

#define STATS_INTERVAL_MS 1000

/* NUS TX characteristic UUID (notifications come from here) */
#define BT_UUID_NUS_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x6E400001, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
#define BT_UUID_NUS_TX_VAL \
	BT_UUID_128_ENCODE(0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

#define BT_UUID_NUS_SERVICE BT_UUID_DECLARE_128(BT_UUID_NUS_SERVICE_VAL)
#define BT_UUID_NUS_TX      BT_UUID_DECLARE_128(BT_UUID_NUS_TX_VAL)

static struct bt_conn *current_conn;
static uint32_t rx_bytes;
static int64_t rx_start_time;
static volatile bool subscribed;

/* GATT discovery state */
static struct bt_gatt_discover_params disc_params;
static struct bt_gatt_subscribe_params sub_params;

/* Delayed work */
static struct k_work_delayable conn_setup_work;

/* ---- Notification Callback ---- */

static uint8_t notify_cb(struct bt_conn *conn,
			  struct bt_gatt_subscribe_params *params,
			  const void *data, uint16_t length)
{
	if (!data) {
		printk("Notifications unsubscribed\n");
		subscribed = false;
		return BT_GATT_ITER_STOP;
	}

	rx_bytes += length;
	return BT_GATT_ITER_CONTINUE;
}

/* ---- GATT Discovery ---- */

static uint8_t gatt_discover_cb(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				struct bt_gatt_discover_params *params)
{
	if (!attr) {
		if (params->type == BT_GATT_DISCOVER_PRIMARY) {
			printk("NUS service not found\n");
		} else {
			printk("NUS TX characteristic not found\n");
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_PRIMARY) {
		struct bt_gatt_service_val *svc =
			(struct bt_gatt_service_val *)attr->user_data;

		printk("Found NUS service (handle %u-%u)\n",
		       attr->handle, svc->end_handle);

		/* Now discover characteristics within the service */
		disc_params.uuid = NULL;
		disc_params.start_handle = attr->handle + 1;
		disc_params.end_handle = svc->end_handle;
		disc_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

		int err = bt_gatt_discover(conn, &disc_params);
		if (err) {
			printk("Char discovery failed (err %d)\n", err);
		}
		return BT_GATT_ITER_STOP;
	}

	if (params->type == BT_GATT_DISCOVER_CHARACTERISTIC) {
		struct bt_gatt_chrc *chrc =
			(struct bt_gatt_chrc *)attr->user_data;

		if (bt_uuid_cmp(chrc->uuid, BT_UUID_NUS_TX) != 0) {
			return BT_GATT_ITER_CONTINUE;
		}

		printk("Found NUS TX characteristic (value handle %u)\n",
		       chrc->value_handle);

		/* Subscribe to notifications */
		sub_params.notify = notify_cb;
		sub_params.value_handle = chrc->value_handle;
		sub_params.ccc_handle = 0; /* auto-discover CCC */
		sub_params.end_handle = disc_params.end_handle;
		sub_params.disc_params = &disc_params;
		sub_params.value = BT_GATT_CCC_NOTIFY;

		int err = bt_gatt_subscribe(conn, &sub_params);
		if (err) {
			printk("Subscribe failed (err %d)\n", err);
		} else {
			printk("Subscribed to notifications\n");
			subscribed = true;
			rx_bytes = 0;
			rx_start_time = k_uptime_get();
		}
		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_STOP;
}

static void start_gatt_discovery(void)
{
	printk("Starting GATT discovery for NUS service...\n");

	disc_params.uuid = BT_UUID_NUS_SERVICE;
	disc_params.func = gatt_discover_cb;
	disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	disc_params.type = BT_GATT_DISCOVER_PRIMARY;

	int err = bt_gatt_discover(current_conn, &disc_params);
	if (err) {
		printk("GATT discovery failed (err %d)\n", err);
	}
}

/* ---- Connection Setup (delayed) ---- */

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
			    struct bt_gatt_exchange_params *params)
{
	if (err) {
		printk("MTU exchange failed (err %u)\n", err);
	} else {
		printk("MTU exchange done\n");
	}
}

static void conn_setup_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	int err;

	/* Request DLE */
	struct bt_conn_le_data_len_param dl_param = {
		.tx_max_len = 251,
		.tx_max_time = 2120,
	};
	err = bt_conn_le_data_len_update(current_conn, &dl_param);
	if (err) {
		printk("DLE update failed (err %d)\n", err);
	}

	/* Request 2M PHY */
	const struct bt_conn_le_phy_param phy_param = {
		.options = BT_CONN_LE_PHY_OPT_NONE,
		.pref_tx_phy = BT_GAP_LE_PHY_2M,
		.pref_rx_phy = BT_GAP_LE_PHY_2M,
	};
	err = bt_conn_le_phy_update(current_conn, &phy_param);
	if (err) {
		printk("PHY update failed (err %d)\n", err);
	}

	/* Exchange MTU */
	static struct bt_gatt_exchange_params mtu_params;
	mtu_params.func = mtu_exchange_cb;
	err = bt_gatt_exchange_mtu(current_conn, &mtu_params);
	if (err) {
		printk("MTU exchange failed (err %d)\n", err);
	}

	/* Start GATT discovery after a small delay for params to settle */
	k_sleep(K_MSEC(200));
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
	printk("Disconnected (reason %u)\n", reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	k_work_cancel_delayable(&conn_setup_work);
	subscribed = false;
	rx_bytes = 0;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	printk("Conn params: interval=%u (%u.%u ms), latency=%u, timeout=%u\n",
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
	printk("DLE updated: TX len=%u time=%u, RX len=%u time=%u\n",
	       info->tx_max_len, info->tx_max_time,
	       info->rx_max_len, info->rx_max_time);
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
	int err;

	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND) {
		return;
	}

	bt_data_parse(ad, name_matches, &found);
	if (!found) {
		return;
	}

	char addr_str[BT_ADDR_LE_STR_LEN];
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

		if (subscribed) {
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

	printk("Starting nRF54L15 GATT Notification Central\n");

	k_work_init_delayable(&conn_setup_work, conn_setup_work_handler);

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
