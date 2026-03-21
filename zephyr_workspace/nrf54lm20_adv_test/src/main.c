/*
 * nRF54LM20 BLE Advertising Test
 *
 * Non-connectable BLE advertising at 1-second interval.
 * No connections accepted — advertising power only.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/bluetooth/bluetooth.h>

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

int main(void)
{
	int err;

	printk("nRF54LM20 Advertising Test\n");

	err = bt_enable(NULL);
	if (err) {
		printk("bt_enable failed (err %d)\n", err);
		return 0;
	}
	printk("Bluetooth initialized\n");

	/* Non-connectable advertising, 1s interval (0x0640 = 1000ms / 0.625ms) */
	struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_USE_IDENTITY,
		0x0640,  /* 1000ms */
		0x0640,  /* 1000ms */
		NULL);

	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		printk("Advertising failed (err %d)\n", err);
		return 0;
	}
	printk("Advertising as '%s'\n", DEVICE_NAME);

	/* Sleep forever — advertising runs in controller */
	k_sleep(K_FOREVER);
	return 0;
}
