/*
 * PSE84 Voice Assistant — Phase 4 BLE bring-up.
 *
 * Stands up the Zephyr Bluetooth stack on M55 (canonical would be M33
 * but that's blocked on extended-boot, see project_pse84_m33_flash_slot_mystery.md)
 * and starts legacy LE advertising so macOS can discover
 * "PSE84-Assistant". No GATT yet — Phase 4.2 wires L2CAP CoC with the
 * |type|seq|len|payload| framing from the master plan.
 */
#include "ble.h"

#ifdef CONFIG_BT

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("connect failed: %u", err);
		return;
	}
	LOG_INF("connected");
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("disconnected: 0x%02x", reason);
	/* Restart advertising so the device stays discoverable. */
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, adv_data,
				  ARRAY_SIZE(adv_data), NULL, 0);
	if (err && err != -EALREADY) {
		LOG_ERR("adv restart failed: %d", err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

/* bt_enable can block for ~1–2 s while the CYW55513 firmware loads.
 * Run it on a dedicated thread so main() keeps ticking audio / UI and
 * we can watch progress via LOG_INF at every step. */
static void ble_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	LOG_INF("ble thread: bt_enable()…");
	int err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return;
	}
	LOG_INF("bt_enable ok; starting advertising (name='%s')",
		CONFIG_BT_DEVICE_NAME);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, adv_data,
			      ARRAY_SIZE(adv_data), NULL, 0);
	if (err) {
		LOG_ERR("adv start failed: %d", err);
		return;
	}
	LOG_INF("advertising as '%s'", CONFIG_BT_DEVICE_NAME);
}

K_THREAD_STACK_DEFINE(ble_thread_stack, 4096);
static struct k_thread ble_thread;

int ble_init(void)
{
	LOG_INF("ble: spawning bt_enable thread");
	k_thread_create(&ble_thread, ble_thread_stack,
			K_THREAD_STACK_SIZEOF(ble_thread_stack),
			ble_thread_fn, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&ble_thread, "ble");
	return 0;
}

#else

int ble_init(void)
{
	return 0;
}

#endif /* CONFIG_BT */
