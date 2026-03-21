/*
 * Alif B1 BLE Advertising Test
 *
 * Non-connectable BLE advertising at ~1s interval using Alif BLE ROM stack.
 * Based on Alif le_periph_hello sample, stripped to advertising only.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include "alif_ble.h"
#include "gapm.h"
#include "gap_le.h"
#include "gapc_le.h"
#include "gapc_sec.h"
#include "gapm_le.h"
#include "gapm_le_adv.h"
#include "co_buf.h"
#include "address_verification.h"

#define SAMPLE_ADDR_TYPE ALIF_STATIC_RAND_ADDR

static uint8_t adv_type;
static uint8_t adv_actv_idx;

#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME
static const char device_name[] = DEVICE_NAME;

K_SEM_DEFINE(init_sem, 0, 1);

/* BLE stack config - peripheral, no pairing */
static gapm_config_t gapm_cfg = {
	.role = GAP_ROLE_LE_PERIPHERAL,
	.pairing_mode = GAPM_PAIRING_DISABLE,
	.privacy_cfg = 0,
	.renew_dur = 1500,
	.private_identity.addr = {0xCF, 0xFE, 0xFB, 0xDE, 0x11, 0x07},
	.irk.key = {0},
	.gap_start_hdl = 0,
	.gatt_start_hdl = 0,
	.att_cfg = 0,
	.sugg_max_tx_octets = GAP_LE_MIN_OCTETS,
	.sugg_max_tx_time = GAP_LE_MIN_TIME,
	.tx_pref_phy = GAP_PHY_ANY,
	.rx_pref_phy = GAP_PHY_ANY,
};

/* Connection callbacks (reject connections) */
static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx,
				 uint8_t role, const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	/* Reject - advertising only test */
	gapc_le_connection_cfm(conidx, 0, NULL);
	gapc_disconnect(conidx, 0, CO_ERROR_REMOTE_USER_TERM_CON, NULL);
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	gapm_le_adv_param_t adv_params = { .duration = 0 };
	gapm_le_start_adv(adv_actv_idx, &adv_params);
}

static void on_name_get(uint8_t conidx, uint32_t metainfo, uint16_t token,
			uint16_t offset, uint16_t max_len)
{
	const size_t len = sizeof(device_name) - 1;
	gapc_le_get_name_cfm(conidx, token, GAP_ERR_NO_ERROR, len,
			     len > max_len ? max_len : len, (const uint8_t *)device_name);
}

static void on_appearance_get(uint8_t conidx, uint32_t metainfo, uint16_t token)
{
	gapc_le_get_appearance_cfm(conidx, token, GAP_ERR_NO_ERROR, 0);
}

static void on_key_received(uint8_t conidx, uint32_t metainfo,
			    const gapc_pairing_keys_t *p_keys) {}

static const gapc_connection_req_cb_t gapc_con_cbs = {
	.le_connection_req = on_le_connection_req,
};
static const gapc_security_cb_t gapc_sec_cbs = {
	.key_received = on_key_received,
};
static const gapc_connection_info_cb_t gapc_con_inf_cbs = {
	.disconnected = on_disconnection,
	.name_get = on_name_get,
	.appearance_get = on_appearance_get,
};
static const gapc_le_config_cb_t gapc_le_cfg_cbs;
static void on_gapm_err(uint32_t metainfo, uint8_t code) {}
static const gapm_cb_t gapm_err_cbs = { .cb_hw_error = on_gapm_err };

static const gapm_callbacks_t gapm_cbs = {
	.p_con_req_cbs = &gapc_con_cbs,
	.p_sec_cbs = &gapc_sec_cbs,
	.p_info_cbs = &gapc_con_inf_cbs,
	.p_le_config_cbs = &gapc_le_cfg_cbs,
	.p_bt_config_cbs = NULL,
	.p_gapm_cbs = &gapm_err_cbs,
};

/* Advertising callbacks */
static void on_adv_actv_stopped(uint32_t metainfo, uint8_t actv_idx, uint16_t reason) {}

static void on_adv_actv_proc_cmp(uint32_t metainfo, uint8_t proc_id, uint8_t actv_idx,
				 uint16_t status)
{
	if (status) {
		return;
	}
	switch (proc_id) {
	case GAPM_ACTV_CREATE_LE_ADV: {
		adv_actv_idx = actv_idx;
		/* Set advertising data: device name */
		const size_t name_len = sizeof(device_name) - 1;
		const uint16_t adv_len = 2 + name_len;
		co_buf_t *p_buf;
		co_buf_alloc(&p_buf, 0, adv_len, 0);
		uint8_t *p_data = co_buf_data(p_buf);
		p_data[0] = name_len + 1;
		p_data[1] = GAP_AD_TYPE_COMPLETE_NAME;
		memcpy(p_data + 2, device_name, name_len);
		gapm_le_set_adv_data(actv_idx, p_buf);
		co_buf_release(p_buf);
		break;
	}
	case GAPM_ACTV_SET_ADV_DATA: {
		co_buf_t *p_buf;
		co_buf_alloc(&p_buf, 0, 0, 0);
		gapm_le_set_scan_response_data(actv_idx, p_buf);
		co_buf_release(p_buf);
		break;
	}
	case GAPM_ACTV_SET_SCAN_RSP_DATA: {
		gapm_le_adv_param_t adv_params = { .duration = 0 };
		gapm_le_start_adv(actv_idx, &adv_params);
		break;
	}
	case GAPM_ACTV_START:
		k_sem_give(&init_sem);
		break;
	}
}

static void on_adv_created(uint32_t metainfo, uint8_t actv_idx, int8_t tx_pwr) {}

static const gapm_le_adv_cb_actv_t le_adv_cbs = {
	.hdr.actv.stopped = on_adv_actv_stopped,
	.hdr.actv.proc_cmp = on_adv_actv_proc_cmp,
	.created = on_adv_created,
};

static void create_advertising(void)
{
	/* Non-connectable advertising, ~1s interval (1600 * 0.625ms = 1000ms) */
	gapm_le_adv_create_param_t adv_create_params = {
		.prop = GAPM_ADV_PROP_NON_CONN_NON_SCAN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
		.tx_pwr = 0,
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg = {
			.adv_intv_min = 1600,
			.adv_intv_max = 1600,
			.ch_map = ADV_ALL_CHNLS_EN,
			.phy = GAPM_PHY_TYPE_LE_1M,
		},
	};
	gapm_le_create_adv_legacy(0, adv_type, &adv_create_params, &le_adv_cbs);
}

void on_gapm_process_complete(uint32_t metainfo, uint16_t status)
{
	if (status) {
		return;
	}
	create_advertising();
}

int main(void)
{
	alif_ble_enable(NULL);

	if (address_verification(SAMPLE_ADDR_TYPE, &adv_type, &gapm_cfg)) {
		return -1;
	}

	gapm_configure(0, &gapm_cfg, &gapm_cbs, on_gapm_process_complete);

	k_sem_take(&init_sem, K_FOREVER);

	/* Advertising runs in controller — sleep forever */
	k_sleep(K_FOREVER);
	return 0;
}
