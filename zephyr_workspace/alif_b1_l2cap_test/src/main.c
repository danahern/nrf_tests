/*
 * Alif B1 L2CAP CoC Throughput Test
 *
 * L2CAP Connection-Oriented Channel server using Alif BLE ROM stack.
 * Registers a dynamic SPSM, accepts CoC connections, streams SDUs.
 * A GATT service exposes the PSM for central discovery.
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
#include "l2cap.h"
#include "l2cap_coc.h"
#include "ke_mem.h"
#include "address_verification.h"

#define SAMPLE_ADDR_TYPE ALIF_STATIC_RAND_ADDR
#define SDU_LEN       492
#define LOCAL_RX_MTU  SDU_LEN
#define L2CAP_SPSM   0x0080  /* Dynamic range PSM */

static uint8_t adv_type;
static uint8_t adv_actv_idx;
static volatile bool gap_connected;
static volatile bool l2cap_connected;
static volatile bool sdu_pending;
static uint8_t l2cap_chan_lid;
static uint16_t tx_mtu;

#define DEVICE_NAME CONFIG_BLE_DEVICE_NAME
static const char device_name[] = DEVICE_NAME;

K_SEM_DEFINE(init_sem, 0, 1);

static uint8_t tx_data[SDU_LEN];

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

/* ---- L2CAP CoC Callbacks ---- */

static void on_sdu_rx(uint8_t conidx, uint8_t chan_lid, uint16_t status, co_buf_t *p_sdu)
{
	/* Release the buffer to return credits */
	co_buf_release(p_sdu);
}

static void on_sdu_sent(uint8_t conidx, uint16_t metainfo, uint8_t chan_lid,
			uint16_t status, co_buf_t *p_sdu)
{
	co_buf_release(p_sdu);
	sdu_pending = false;
}

static void on_coc_create_cmp(uint8_t conidx, uint16_t metainfo, uint16_t status,
			      uint8_t nb_chan) {}

static void on_coc_created(uint8_t conidx, uint16_t metainfo, uint8_t chan_lid,
			   uint16_t local_rx_mtu, uint16_t peer_rx_mtu)
{
	l2cap_chan_lid = chan_lid;
	tx_mtu = peer_rx_mtu < SDU_LEN ? peer_rx_mtu : SDU_LEN;
	l2cap_connected = true;
	sdu_pending = false;
}

static void on_coc_terminated(uint8_t conidx, uint16_t metainfo, uint8_t chan_lid,
			      uint16_t reason)
{
	l2cap_connected = false;
	sdu_pending = false;
}

static void on_coc_terminate_cmp(uint8_t conidx, uint16_t metainfo, uint8_t chan_lid,
				 uint16_t status) {}

static const l2cap_chan_coc_cb_t l2cap_coc_cbs = {
	.cb_sdu_rx = on_sdu_rx,
	.cb_sdu_sent = on_sdu_sent,
	.cb_coc_create_cmp = on_coc_create_cmp,
	.cb_coc_created = on_coc_created,
	.cb_coc_terminated = on_coc_terminated,
	.cb_coc_terminate_cmp = on_coc_terminate_cmp,
};

/* L2CAP SPSM connect request callback */
static void on_coc_connect_req(uint8_t conidx, uint16_t token, uint8_t nb_chan,
			       uint16_t spsm, uint16_t peer_rx_mtu)
{
	/* Accept 1 channel */
	l2cap_coc_connect_cfm(conidx, token, 1, LOCAL_RX_MTU, &l2cap_coc_cbs);
}

static const l2cap_coc_spsm_cb_t l2cap_spsm_cbs = {
	.cb_coc_connect_req = on_coc_connect_req,
};

/* ---- PSM Discovery GATT Service ---- */

#define ATT_128_PRIMARY_SERVICE  ATT_16_TO_128_ARRAY(GATT_DECL_PRIMARY_SERVICE)
#define ATT_128_CHARACTERISTIC   ATT_16_TO_128_ARRAY(GATT_DECL_CHARACTERISTIC)
#define ATT_16_TO_128_ARRAY(uuid) \
	{ (uuid) & 0xFF, (uuid >> 8) & 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

/* Same PSM service UUID as nRF L2CAP test */
#define PSM_SVC_UUID \
	{ 0xF0, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, \
	  0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12 }
#define PSM_CHAR_UUID \
	{ 0xF1, 0xDE, 0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12, \
	  0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12 }

static uint16_t gatt_svc_id[8] = {
	0xDEF0, 0x9ABC, 0x5678, 0x1234,
	0x5678, 0x1234, 0x5678, 0x1234
};

enum psm_att_list {
	PSM_IDX_SERVICE = 0,
	PSM_IDX_CHAR,
	PSM_IDX_VAL,
	PSM_IDX_NB,
};

static const uint8_t psm_service_uuid[] = PSM_SVC_UUID;

static const gatt_att_desc_t psm_att_db[PSM_IDX_NB] = {
	[PSM_IDX_SERVICE] = {ATT_128_PRIMARY_SERVICE, ATT_UUID(16) | PROP(RD), 0},
	[PSM_IDX_CHAR] = {ATT_128_CHARACTERISTIC, ATT_UUID(16) | PROP(RD), 0},
	[PSM_IDX_VAL] = {PSM_CHAR_UUID, ATT_UUID(128) | PROP(RD), OPT(NO_OFFSET) | sizeof(uint16_t)},
};

static struct {
	uint16_t start_hdl;
	uint8_t user_lid;
} psm_svc_env;

static void on_psm_att_read(uint8_t conidx, uint8_t user_lid, uint16_t token,
			    uint16_t hdl, uint16_t offset, uint16_t max_length)
{
	co_buf_t *p_buf = NULL;
	uint16_t psm = L2CAP_SPSM;
	uint16_t status = GAP_ERR_NO_ERROR;

	uint8_t att_idx = hdl - psm_svc_env.start_hdl;
	if (att_idx == PSM_IDX_VAL) {
		co_buf_alloc(&p_buf, GATT_BUFFER_HEADER_LEN, sizeof(psm), GATT_BUFFER_TAIL_LEN);
		memcpy(co_buf_data(p_buf), &psm, sizeof(psm));
	} else {
		status = ATT_ERR_REQUEST_NOT_SUPPORTED;
	}

	gatt_srv_att_read_get_cfm(conidx, user_lid, token, status, sizeof(psm), p_buf);
	if (p_buf) {
		co_buf_release(p_buf);
	}
}

static void on_psm_att_val_set(uint8_t conidx, uint8_t user_lid, uint16_t token,
			       uint16_t hdl, uint16_t offset, co_buf_t *p_data)
{
	gatt_srv_att_val_set_cfm(conidx, user_lid, token, ATT_ERR_REQUEST_NOT_SUPPORTED);
}

static const gatt_srv_cb_t psm_gatt_cbs = {
	.cb_att_read_get = on_psm_att_read,
	.cb_att_val_set = on_psm_att_val_set,
};

static uint16_t psm_service_init(void)
{
	uint16_t status;

	status = gatt_user_srv_register(64, 0, &psm_gatt_cbs, &psm_svc_env.user_lid);
	if (status != GAP_ERR_NO_ERROR) {
		return status;
	}

	status = gatt_db_svc_add(psm_svc_env.user_lid, SVC_UUID(128), psm_service_uuid,
				 PSM_IDX_NB, NULL, psm_att_db, PSM_IDX_NB,
				 &psm_svc_env.start_hdl);
	return status;
}

/* ---- GAP Connection Callbacks ---- */

static void on_le_connection_req(uint8_t conidx, uint32_t metainfo, uint8_t actv_idx,
				 uint8_t role, const gap_bdaddr_t *p_peer_addr,
				 const gapc_le_con_param_t *p_con_params, uint8_t clk_accuracy)
{
	gapc_le_connection_cfm(conidx, 0, NULL);
	gap_connected = true;
}

static void on_disconnection(uint8_t conidx, uint32_t metainfo, uint16_t reason)
{
	gap_connected = false;
	l2cap_connected = false;
	sdu_pending = false;

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

/* ---- Advertising ---- */

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

	/* Register L2CAP SPSM */
	l2cap_coc_spsm_add(L2CAP_SPSM, 0, &l2cap_spsm_cbs);

	/* Register PSM discovery GATT service */
	psm_service_init();

	/* Start advertising */
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
	for (int i = 0; i < SDU_LEN; i++) {
		tx_data[i] = i & 0xFF;
	}

	/* L2CAP SDU streaming loop */
	while (1) {
		if (!l2cap_connected || sdu_pending) {
			k_sleep(K_MSEC(1));
			continue;
		}

		co_buf_t *p_sdu;
		uint16_t err = co_buf_alloc(&p_sdu, 0, tx_mtu, 0);
		if (err != CO_BUF_ERR_NO_ERROR) {
			k_sleep(K_MSEC(1));
			continue;
		}

		memcpy(co_buf_data(p_sdu), tx_data, tx_mtu);

		err = l2cap_chan_sdu_send(0, 0, l2cap_chan_lid, p_sdu);
		if (err == GAP_ERR_NO_ERROR) {
			sdu_pending = true;
		} else {
			co_buf_release(p_sdu);
			k_sleep(K_MSEC(1));
		}
	}

	return 0;
}
