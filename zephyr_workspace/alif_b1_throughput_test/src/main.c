/*
 * Alif B1 BLE GATT Notification Throughput Test
 *
 * Streams continuous 244-byte GATT notifications using Alif BLE ROM stack.
 * Uses NUS-like UUIDs for compatibility with existing test scripts.
 * Based on Alif le_periph_hello sample pattern.
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
#include "prf.h"
#include "gatt_db.h"
#include "gatt_srv.h"
#include "ke_mem.h"
#include "address_verification.h"

#define SAMPLE_ADDR_TYPE ALIF_STATIC_RAND_ADDR
#define NOTIFY_LEN 244
#define NTF_METAINFO 0x1234

static uint8_t adv_type;
static uint8_t adv_actv_idx;
static volatile bool connected;
static volatile bool ntf_enabled;
static volatile bool ntf_ongoing;

#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME
static const char device_name[] = DEVICE_NAME;

K_SEM_DEFINE(init_sem, 0, 1);

/* NUS Service UUID (same as nRF test for compatibility) */
#define NUS_UUID_128_SVC \
	{ 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
	  0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e }
#define NUS_UUID_128_TX \
	{ 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
	  0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e }
#define NUS_UUID_128_RX \
	{ 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
	  0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e }

/* Service UUID for advertising */
static uint16_t gatt_svc_id[8] = {
	0xca9e, 0x24dc, 0xe50e, 0xe0a9,
	0xf393, 0xb5a3, 0x0001, 0x6e40
};

/* ATT macros */
#define ATT_128_PRIMARY_SERVICE  ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_CHARACTERISTIC   ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_128_CLIENT_CHAR_CFG  ATT_16_TO_128_ARRAY(GATT_DESC_CLIENT_CHAR_CFG)
#define ATT_16_TO_128_ARRAY(uuid) \
	{ (uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

enum nus_att_list {
	NUS_IDX_SERVICE = 0,
	NUS_IDX_TX_CHAR,
	NUS_IDX_TX_VAL,
	NUS_IDX_TX_NTF_CFG,
	NUS_IDX_RX_CHAR,
	NUS_IDX_RX_VAL,
	NUS_IDX_NB,
};

static const uint8_t nus_service_uuid[] = NUS_UUID_128_SVC;

static const gatt_att_desc_t nus_att_db[NUS_IDX_NB] = {
	[NUS_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},
	[NUS_IDX_TX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[NUS_IDX_TX_VAL] = {NUS_UUID_128_TX, ATT_UUID(128) | PROP(N), OPT(NO_OFFSET)},
	[NUS_IDX_TX_NTF_CFG] = {ATT_128_CLIENT_CHAR_CFG, ATT_UUID(16) | PROP(RD) | PROP(WR), 0},
	[NUS_IDX_RX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[NUS_IDX_RX_VAL] = {NUS_UUID_128_RX, ATT_UUID(128) | PROP(WR) | PROP(WC),
			     OPT(NO_OFFSET) | NOTIFY_LEN},
};

static struct {
	uint16_t start_hdl;
	uint8_t user_lid;
	uint16_t ntf_cfg;
} svc_env;

static uint8_t tx_data[NOTIFY_LEN];

/* BLE stack config */
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
	.sugg_max_tx_octets = 251,
	.sugg_max_tx_time = 2120,
	.tx_pref_phy = GAP_PHY_ANY,
	.rx_pref_phy = GAP_PHY_ANY,
};

/* GATT callbacks */
static void on_att_read_get(uint8_t conidx, uint8_t user_lid, uint16_t token,
			    uint16_t hdl, uint16_t offset, uint16_t max_length)
{
	co_buf_t *p_buf = NULL;
	uint16_t status = GAP_ERR_NO_ERROR;
	uint16_t len = 0;

	uint8_t att_idx = hdl - svc_env.start_hdl;
	if (att_idx == NUS_IDX_TX_NTF_CFG) {
		len = sizeof(svc_env.ntf_cfg);
		co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, len, GATT_BUFFER_TAIL_LEN);
		memcpy(co_buf_data(p_buf), &svc_env.ntf_cfg, len);
	} else {
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
	}

	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, len, p_buf);
	if (p_buf) {
		co_buf_release(p_buf);
	}
}

static void on_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token,
			   uint16_t hdl, uint16_t offset, co_buf_t *p_data)
{
	uint16_t status = GAP_ERR_NO_ERROR;
	uint8_t att_idx = hdl - svc_env.start_hdl;

	if (att_idx == NUS_IDX_TX_NTF_CFG) {
		uint16_t cfg;
		memcpy(&cfg, co_buf_data(p_data), sizeof(uint16_t));
		if (cfg == PRF_CLI_START_NTF || cfg == PRF_CLI_STOP_NTFIND) {
			svc_env.ntf_cfg = cfg;
			ntf_enabled = (cfg == PRF_CLI_START_NTF);
		} else {
			status = ATT_ERR_REQUEST_NOT_SUPPORTED;
		}
	} else if (att_idx == NUS_IDX_RX_VAL) {
		/* RX write - ignore data */
	} else {
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
	}

	gatt_srv_att_val_set_cfm(conidx, user_lid, token, status);
}

static void on_event_sent(uint8_t conidx, uint8_t user_lid, uint16_t metainfo,
			  uint16_t status)
{
	ntf_ongoing = false;
}

static const gatt_srv_cb_t gatt_cbs = {
	.cb_att_read_get = on_att_read_get,
	.cb_att_val_set = on_att_val_set,
	.cb_event_sent = on_event_sent,
};

static uint16_t service_init(void)
{
	uint16_t status;

	status = gatt_user_srv_register(NOTIFY_LEN + 10, 0, &gatt_cbs, &svc_env.user_lid);
	if (status != GAP_ERR_NO_ERROR) {
		return status;
	}

	status = gatt_db_svc_add(svc_env.user_lid, SVC_UUID(128), nus_service_uuid,
				 NUS_IDX_NB, NULL, nus_att_db, NUS_IDX_NB,
				 &svc_env.start_hdl);
	if (status != GAP_ERR_NO_ERROR) {
		gatt_user_unregister(svc_env.user_lid);
	}
	return status;
}

/* Connection callbacks */
static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx,
				 uint8_t role, const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	gapc_le_connection_cfm(conidx, 0, NULL);
	connected = true;
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	connected = false;
	ntf_enabled = false;
	ntf_ongoing = false;
	svc_env.ntf_cfg = 0;

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

static const gapc_connection_req_cb_t gapc_con_cbs = { .le_connection_req = on_le_connection_req };
static const gapc_security_cb_t gapc_sec_cbs = { .key_received = on_key_received };
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
		const size_t name_len = sizeof(device_name) - 1;
		const uint16_t svc_uuid_len = sizeof(gatt_svc_id);
		const uint16_t adv_len = (2 + name_len) + (2 + svc_uuid_len);
		co_buf_t *p_buf;
		co_buf_alloc(&p_buf, 0, adv_len, 0);
		uint8_t *p = co_buf_data(p_buf);
		p[0] = name_len + 1;
		p[1] = GAP_AD_TYPE_COMPLETE_NAME;
		memcpy(p + 2, device_name, name_len);
		p += 2 + name_len;
		p[0] = svc_uuid_len + 1;
		p[1] = GAP_AD_TYPE_COMPLETE_LIST_128_BIT_UUID;
		memcpy(p + 2, gatt_svc_id, svc_uuid_len);
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
	gapm_le_adv_create_param_t adv_create_params = {
		.prop = GAPM_ADV_PROP_UNDIR_CONN_MASK,
		.disc_mode = GAPM_ADV_MODE_GEN_DISC,
		.tx_pwr = 0,
		.filter_pol = GAPM_ADV_ALLOW_SCAN_ANY_CON_ANY,
		.prim_cfg = {
			.adv_intv_min = 160,
			.adv_intv_max = 800,
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
	service_init();
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

	/* Fill TX data pattern */
	for (int i = 0; i < NOTIFY_LEN; i++) {
		tx_data[i] = i & 0xFF;
	}

	/* Notification streaming loop */
	while (1) {
		if (!connected || !ntf_enabled || ntf_ongoing) {
			k_sleep(K_MSEC(1));
			continue;
		}

		co_buf_t *p_buf;
		uint16_t err = co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN,
					    NOTIFY_LEN, GATT_BUFFER_TAIL_LEN);
		if (err != CO_BUF_ERR_NO_ERROR) {
			k_sleep(K_MSEC(1));
			continue;
		}

		memcpy(co_buf_data(p_buf), tx_data, NOTIFY_LEN);

		err = gatt_srv_event_send(0, svc_env.user_lid, NTF_METAINFO,
					  GATT_NOTIFY,
					  svc_env.start_hdl + NUS_IDX_TX_VAL, p_buf);
		co_buf_release(p_buf);

		if (err == GAP_ERR_NO_ERROR) {
			ntf_ongoing = true;
		} else {
			k_sleep(K_MSEC(1));
		}
	}

	return 0;
}
