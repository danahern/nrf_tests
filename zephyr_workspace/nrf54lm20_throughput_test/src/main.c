/*
 * nRF54LM20 BLE GATT Notification Throughput Test
 *
 * Ported from proven nrf54l15_ble_test. Streams continuous GATT
 * notifications via NUS TX characteristic. Uses bt_gatt_notify()
 * which handles MTU fragmentation automatically.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* Max notification payload with 498 MTU (498 - 3 byte ATT header) */
#define TEST_DATA_SIZE  495
#define STATS_INTERVAL_MS 1000

/* NUS UUIDs */
#define BT_UUID_NUS_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define BT_UUID_NUS_TX_VAL \
	BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)
#define BT_UUID_NUS_RX_VAL \
	BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e)

#define BT_UUID_NUS_SERVICE BT_UUID_DECLARE_128(BT_UUID_NUS_SERVICE_VAL)
#define BT_UUID_NUS_TX      BT_UUID_DECLARE_128(BT_UUID_NUS_TX_VAL)
#define BT_UUID_NUS_RX      BT_UUID_DECLARE_128(BT_UUID_NUS_RX_VAL)

static struct bt_conn *current_conn;
static uint32_t bytes_sent;
static bool notify_enabled;
static volatile bool dle_ready;
static uint8_t test_data[TEST_DATA_SIZE];
static struct k_work_delayable conn_param_work;

/* Advertising */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_SERVICE_VAL),
};

/* Connection param work */
static void conn_param_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	bt_conn_le_phy_update(current_conn, BT_CONN_LE_PHY_PARAM_2M);

	struct bt_conn_le_data_len_param dl_param = {
		.tx_max_len = 251, .tx_max_time = 2120,
	};
	bt_conn_le_data_len_update(current_conn, &dl_param);

	struct bt_le_conn_param param = {
		.interval_min = 6, .interval_max = 12,
		.latency = 0, .timeout = 400,
	};
	bt_conn_le_param_update(current_conn, &param);
}

/* GATT NUS Service */
static void tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("Notifications %s\n", notify_enabled ? "enabled" : "disabled");
}

static ssize_t on_rx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	return len;
}

BT_GATT_SERVICE_DEFINE(nus_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_NUS_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_NUS_TX,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_NUS_RX,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, on_rx_write, NULL),
);

/* Connection callbacks */
static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		printk("Connection failed (err %u)\n", err);
		return;
	}
	printk("Connected\n");
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
	bytes_sent = 0;
	notify_enabled = false;
	dle_ready = false;

	bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}

static void le_data_len_updated(struct bt_conn *conn,
				struct bt_conn_le_data_len_info *info)
{
	printk("DLE: TX len=%u time=%u, RX len=%u time=%u\n",
	       info->tx_max_len, info->tx_max_time,
	       info->rx_max_len, info->rx_max_time);
	if (info->tx_max_len >= 251) {
		dle_ready = true;
	}
}

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	printk("MTU updated: TX=%u RX=%u (payload=%u)\n", tx, rx, tx - 3);
}

static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = mtu_updated,
};

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_data_len_updated = le_data_len_updated,
};

/* Stream thread — sends notifications as fast as possible */
void stream_thread(void)
{
	for (int i = 0; i < TEST_DATA_SIZE; i++) {
		test_data[i] = i & 0xFF;
	}

	while (1) {
		if (current_conn && notify_enabled && dle_ready) {
			int err = bt_gatt_notify(current_conn, &nus_svc.attrs[1],
						 test_data, TEST_DATA_SIZE);
			if (err == 0) {
				bytes_sent += TEST_DATA_SIZE;
			}
			k_sleep(K_MSEC(5));
		} else {
			k_sleep(K_MSEC(100));
		}
	}
}

/* Stats thread */
void stats_thread(void)
{
	uint32_t prev_bytes = 0;

	while (1) {
		k_sleep(K_MSEC(STATS_INTERVAL_MS));
		if (current_conn && notify_enabled) {
			uint32_t delta = bytes_sent - prev_bytes;
			prev_bytes = bytes_sent;
			uint32_t kbps = (delta * 8) / STATS_INTERVAL_MS;
			printk("TX: %u kbps (%u bytes total)\n", kbps, bytes_sent);
		}
	}
}

K_THREAD_DEFINE(stats_tid, 1024, stats_thread, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(stream_tid, 2048, stream_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	int err;

	printk("nRF54LM20 GATT Throughput Test\n");

	k_work_init_delayable(&conn_param_work, conn_param_work_handler);

	err = bt_enable(NULL);
	if (err) {
		printk("BT init failed (err %d)\n", err);
		return 0;
	}
	printk("BT initialized\n");

	bt_gatt_cb_register(&gatt_callbacks);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Advertising failed (err %d)\n", err);
		return 0;
	}
	printk("Advertising as '%s'\n", DEVICE_NAME);

	return 0;
}
