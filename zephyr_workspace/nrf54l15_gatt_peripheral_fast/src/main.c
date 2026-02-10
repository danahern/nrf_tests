/*
 * GATT Notification Throughput Peripheral for nRF54L15
 *
 * Streams data via GATT notifications at max speed using bt_gatt_notify_cb()
 * with a semaphore-based flow control (same pattern as L2CAP throughput test).
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/printk.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define NOTIFY_SIZE      495  /* 498 MTU - 3 byte ATT header */
#define TX_BUF_COUNT     10
#define STATS_INTERVAL_MS 1000

/* NUS-compatible UUIDs (same as nrf54l15_ble_test) */
#define BT_UUID_THROUGHPUT_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x6E400001, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)
#define BT_UUID_THROUGHPUT_TX_VAL \
	BT_UUID_128_ENCODE(0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

#define BT_UUID_THROUGHPUT_SERVICE BT_UUID_DECLARE_128(BT_UUID_THROUGHPUT_SERVICE_VAL)
#define BT_UUID_THROUGHPUT_TX      BT_UUID_DECLARE_128(BT_UUID_THROUGHPUT_TX_VAL)

static struct bt_conn *current_conn;
static struct k_sem tx_sem;
static uint32_t bytes_sent;
static volatile bool notify_enabled;
static volatile bool dle_ready;
static struct k_work_delayable conn_param_work;

static uint8_t tx_data[NOTIFY_SIZE];

/* ---- Notification sent callback ---- */

static void notify_sent_cb(struct bt_conn *conn, void *user_data)
{
	k_sem_give(&tx_sem);
}

/* ---- GATT Service ---- */

static void tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Notifications %s\n", notify_enabled ? "enabled" : "disabled");

	if (notify_enabled) {
		/* Prime the semaphore to allow TX_BUF_COUNT in-flight */
		for (int i = 0; i < TX_BUF_COUNT; i++) {
			k_sem_give(&tx_sem);
		}
	} else {
		k_sem_reset(&tx_sem);
	}
}

BT_GATT_SERVICE_DEFINE(throughput_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_THROUGHPUT_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_THROUGHPUT_TX,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* ---- Advertising ---- */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_THROUGHPUT_SERVICE_VAL),
};

/* ---- Connection Callbacks ---- */

static void conn_param_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	struct bt_conn_le_data_len_param dl_param = {
		.tx_max_len = 251,
		.tx_max_time = 2120,
	};
	int err = bt_conn_le_data_len_update(current_conn, &dl_param);
	if (err) {
		printk("DLE update failed (err %d)\n", err);
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

	bt_le_adv_stop();
	k_work_schedule(&conn_param_work, K_MSEC(50));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	printk("Disconnected (reason %u)\n", reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	k_work_cancel_delayable(&conn_param_work);
	notify_enabled = false;
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
	printk("DLE updated: TX len=%u time=%u, RX len=%u time=%u\n",
	       info->tx_max_len, info->tx_max_time,
	       info->rx_max_len, info->rx_max_time);

	if (info->tx_max_len >= 251) {
		dle_ready = true;
	}
}

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	printk("MTU updated: TX=%u, RX=%u (max notify payload: %u)\n",
	       tx, rx, tx - 3);
}

static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = mtu_updated,
};

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
	for (int i = 0; i < NOTIFY_SIZE; i++) {
		tx_data[i] = i & 0xFF;
	}

	while (1) {
		if (!notify_enabled || !dle_ready) {
			k_sleep(K_MSEC(100));
			continue;
		}

		k_sem_take(&tx_sem, K_FOREVER);

		if (!notify_enabled) {
			continue;
		}

		struct bt_gatt_notify_params params = {
			.attr = &throughput_svc.attrs[1],
			.data = tx_data,
			.len = NOTIFY_SIZE,
			.func = notify_sent_cb,
		};

		int err = bt_gatt_notify_cb(current_conn, &params);
		if (err) {
			k_sem_give(&tx_sem);
			k_sleep(K_MSEC(10));
		} else {
			bytes_sent += NOTIFY_SIZE;
		}
	}
}

/* ---- Stats Thread ---- */

void stats_thread(void)
{
	uint32_t prev_bytes = 0;

	while (1) {
		k_sleep(K_MSEC(STATS_INTERVAL_MS));

		if (notify_enabled && dle_ready) {
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

	printk("Starting nRF54L15 GATT Notification Throughput Test\n");

	k_sem_init(&tx_sem, 0, TX_BUF_COUNT);
	k_work_init_delayable(&conn_param_work, conn_param_work_handler);

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}
	printk("Bluetooth initialized\n");

	bt_gatt_cb_register(&gatt_callbacks);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Advertising failed (err %d)\n", err);
		return 0;
	}

	printk("Advertising started as '%s'\n", DEVICE_NAME);
	printk("Waiting for connection + notification subscribe...\n");

	return 0;
}
