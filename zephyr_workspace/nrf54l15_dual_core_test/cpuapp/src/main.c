/*
 * BLE Throughput Test for nRF54L15 (ARM Cortex-M33)
 * Measures MIPS during BLE data streaming
 * Communicates with RISC-V core for workload testing
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/timing/timing.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ipc/ipc_service.h>
#include <string.h>

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define TEST_DATA_SIZE 495  /* Max notification payload with 498 MTU (498 - 3 byte ATT header) */
#define STATS_INTERVAL_MS 1000

/* Custom Throughput Service UUID: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E */
#define BT_UUID_THROUGHPUT_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x6E400001, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

/* TX Characteristic UUID: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E */
#define BT_UUID_THROUGHPUT_TX_VAL \
	BT_UUID_128_ENCODE(0x6E400003, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

/* RX Characteristic UUID: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E */
#define BT_UUID_THROUGHPUT_RX_VAL \
	BT_UUID_128_ENCODE(0x6E400002, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

/* Control Characteristic UUID: 6E400004-B5A3-F393-E0A9-E50E24DCCA9E */
#define BT_UUID_THROUGHPUT_CTRL_VAL \
	BT_UUID_128_ENCODE(0x6E400004, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

/* RISC-V Workload Control UUID: 6E400005-B5A3-F393-E0A9-E50E24DCCA9E */
#define BT_UUID_RISCV_WORKLOAD_VAL \
	BT_UUID_128_ENCODE(0x6E400005, 0xB5A3, 0xF393, 0xE0A9, 0xE50E24DCCA9E)

#define BT_UUID_THROUGHPUT_SERVICE  BT_UUID_DECLARE_128(BT_UUID_THROUGHPUT_SERVICE_VAL)
#define BT_UUID_THROUGHPUT_TX       BT_UUID_DECLARE_128(BT_UUID_THROUGHPUT_TX_VAL)
#define BT_UUID_THROUGHPUT_RX       BT_UUID_DECLARE_128(BT_UUID_THROUGHPUT_RX_VAL)
#define BT_UUID_THROUGHPUT_CTRL     BT_UUID_DECLARE_128(BT_UUID_THROUGHPUT_CTRL_VAL)
#define BT_UUID_RISCV_WORKLOAD      BT_UUID_DECLARE_128(BT_UUID_RISCV_WORKLOAD_VAL)

/* IPC message types */
enum ipc_msg_type {
	IPC_MSG_STATS = 1,
	IPC_MSG_SET_WORKLOAD = 2,
	IPC_MSG_HEARTBEAT = 3,
	IPC_MSG_AUDIO_DATA = 4,
};

struct ipc_message {
	uint8_t type;
	uint8_t workload;
	uint16_t reserved;
	uint32_t data[5];  /* Generic data payload - increased to fit stats_data (20 bytes) */
} __packed;

struct stats_data {
	uint64_t total_cycles;
	uint32_t iterations;
	uint32_t mips;
	uint32_t workload_type;
	uint32_t cpu_pct;  /* CPU utilization percentage */
} __packed;

static struct bt_conn *current_conn;
static uint32_t bytes_sent = 0;
static uint32_t bytes_received = 0;
static uint64_t total_cycles = 0;
static uint32_t iterations = 0;

static uint8_t test_data[TEST_DATA_SIZE];

static bool notify_enabled = false;
static struct k_work_delayable conn_param_work;

/* TX rate control: 0 = disabled, >0 = target kbps */
static uint32_t target_tx_kbps = 0;  /* Default: max speed (0 = no delay) */

/* IPC for RISC-V communication */
static struct ipc_ept ep;
static uint32_t riscv_mips = 0;
static uint32_t riscv_workload = 0;
static uint32_t riscv_cpu_pct = 0;
static uint32_t audio_frames_received = 0;
static uint32_t audio_voice_detected = 0;
static bool ipc_ready = false;

/* BLE Advertising data */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_THROUGHPUT_SERVICE_VAL),
};

static void conn_param_work_handler(struct k_work *work)
{
	if (!current_conn) {
		return;
	}

	printk("Requesting PHY update to 2M and connection params...\n");

	/* Request PHY update to 2M */
	int err = bt_conn_le_phy_update(current_conn, BT_CONN_LE_PHY_PARAM_2M);
	if (err) {
		printk("PHY update request failed (err %d)\n", err);
	}

	/* Request connection parameter update for better throughput */
	/* Using 15ms (interval=12) instead of 7.5ms - macOS more likely to accept */
	struct bt_le_conn_param param = {
		.interval_min = 12,
		.interval_max = 12,
		.latency = 0,
		.timeout = 400,
	};
	err = bt_conn_le_param_update(current_conn, &param);
	if (err) {
		printk("Conn param update request failed (err %d)\n", err);
	}
}

static void tx_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	printk("TX notifications %s\n", notify_enabled ? "enabled" : "disabled");
}

static ssize_t on_receive(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  const void *buf,
			  uint16_t len,
			  uint16_t offset,
			  uint8_t flags)
{
	if (len > 0) {
		bytes_received += len;
	}
	return len;
}

static ssize_t on_control_write(struct bt_conn *conn,
				 const struct bt_gatt_attr *attr,
				 const void *buf,
				 uint16_t len,
				 uint16_t offset,
				 uint8_t flags)
{
	if (len == 4) {
		/* Expect 4-byte uint32_t for target TX rate in kbps */
		uint32_t new_rate;
		memcpy(&new_rate, buf, 4);
		target_tx_kbps = new_rate;
		printk("Control: TX rate set to %u kbps\n", target_tx_kbps);
	}
	return len;
}

static ssize_t on_riscv_workload_write(struct bt_conn *conn,
					const struct bt_gatt_attr *attr,
					const void *buf,
					uint16_t len,
					uint16_t offset,
					uint8_t flags)
{
	if (len == 1) {
		/* Expect 1-byte workload type */
		uint8_t workload = *(uint8_t *)buf;

		#if DT_NODE_EXISTS(DT_NODELABEL(ipc0))
		if (!ipc_ready) {
			printk("ARM: IPC not ready yet, waiting...\n");
			/* Wait up to 2 seconds for IPC to be ready */
			for (int i = 0; i < 20 && !ipc_ready; i++) {
				k_sleep(K_MSEC(100));
			}
			if (!ipc_ready) {
				printk("ARM: IPC still not ready, cannot send workload\n");
				return len;
			}
		}

		/* Send workload command to RISC-V via IPC */
		struct ipc_message msg;
		memset(&msg, 0, sizeof(msg));
		msg.type = IPC_MSG_SET_WORKLOAD;
		msg.workload = workload;

		int ret = ipc_service_send(&ep, &msg, sizeof(msg));
		if (ret < 0) {
			printk("ARM: Failed to send workload to RISC-V (err %d)\n", ret);
		} else {
			printk("ARM: Set RISC-V workload to %u\n", workload);
		}
		#else
		printk("ARM: Workload %u requested but IPC not available\n", workload);
		#endif
	}
	return len;
}

/* IPC callbacks */
static void ep_bound(void *priv)
{
	ipc_ready = true;
	printk("ARM: IPC endpoint bound and ready\n");
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	struct ipc_message *msg = (struct ipc_message *)data;

	printk("ARM: Received IPC msg type=%d len=%d\n", msg->type, len);

	if (msg->type == IPC_MSG_STATS) {
		struct stats_data *stats = (struct stats_data *)msg->data;
		riscv_mips = stats->mips;
		riscv_workload = stats->workload_type;
		riscv_cpu_pct = stats->cpu_pct;
		printk("ARM: RISC-V stats - workload=%u mips=%u cpu=%u%%\n",
		       riscv_workload, riscv_mips, riscv_cpu_pct);
		/* Stats will be printed by stats_thread */
	} else if (msg->type == IPC_MSG_AUDIO_DATA) {
		/* Received processed audio data from RISC-V */
		audio_frames_received++;

		/* Extract audio samples and VAD info */
		int16_t sample0 = msg->data[0] & 0xFFFF;
		int16_t sample1 = (msg->data[0] >> 16) & 0xFFFF;
		int16_t sample2 = msg->data[1] & 0xFFFF;
		int16_t sample3 = (msg->data[1] >> 16) & 0xFFFF;
		uint32_t frame_energy = msg->data[2];
		uint32_t zero_crossings = msg->data[3];

		/* Count frames with voice detected */
		if (frame_energy > 1000) {
			audio_voice_detected++;
		}

		/* In a real application, you would:
		 * 1. Buffer the audio samples
		 * 2. Send via BLE to phone/cloud
		 * 3. Run inference (keyword spotting, etc.)
		 * 4. Compress for storage/transmission
		 */

		/* For now, just track reception */
		(void)sample0; (void)sample1; (void)sample2; (void)sample3;
		(void)zero_crossings;
	}
}

static struct ipc_ept_cfg ep_cfg = {
	.name = "ep0",
	.cb = {
		.bound    = ep_bound,
		.received = ep_recv,
	},
};

/* Throughput Service Declaration */
BT_GATT_SERVICE_DEFINE(throughput_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_THROUGHPUT_SERVICE),
	BT_GATT_CHARACTERISTIC(BT_UUID_THROUGHPUT_TX,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_cfg_changed,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_THROUGHPUT_RX,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, on_receive, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_THROUGHPUT_CTRL,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, on_control_write, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_RISCV_WORKLOAD,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE,
			       NULL, on_riscv_workload_write, NULL),
);

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

	/* Schedule param updates after 1 second to let connection stabilize */
	k_work_schedule(&conn_param_work, K_SECONDS(1));
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

	/* Cancel any pending work */
	k_work_cancel_delayable(&conn_param_work);

	bytes_sent = 0;
	bytes_received = 0;
	total_cycles = 0;
	iterations = 0;
	notify_enabled = false;
	target_tx_kbps = 0;  /* Reset to max speed on disconnect */
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			      uint16_t latency, uint16_t timeout)
{
	/* Interval is in units of 1.25ms */
	float interval_ms = interval * 1.25f;
	printk("*** Connection params updated: interval=%u (%.2f ms), latency=%u, timeout=%u ***\n",
	       interval, interval_ms, latency, timeout);
}

static void le_phy_updated(struct bt_conn *conn,
			   struct bt_conn_le_phy_info *param)
{
	printk("PHY updated: TX PHY %u, RX PHY %u\n", param->tx_phy, param->rx_phy);
}

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	printk("*** MTU UPDATED: TX=%u, RX=%u (max payload: %u bytes) ***\n",
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
};

static int send_data(const uint8_t *data, uint16_t len)
{
	if (!current_conn || !notify_enabled) {
		return -ENOTCONN;
	}

	struct bt_gatt_notify_params params = {
		.attr = &throughput_svc.attrs[1],
		.data = data,
		.len = len,
	};

	return bt_gatt_notify_cb(current_conn, &params);
}

void stats_thread(void)
{
	uint32_t prev_bytes_sent = 0;
	uint32_t prev_bytes_received = 0;

	timing_init();
	timing_start();

	while (1) {
		k_sleep(K_MSEC(STATS_INTERVAL_MS));

		if (current_conn) {
			uint32_t sent_delta = bytes_sent - prev_bytes_sent;
			uint32_t recv_delta = bytes_received - prev_bytes_received;

			prev_bytes_sent = bytes_sent;
			prev_bytes_received = bytes_received;

			/* Calculate throughput */
			uint32_t tx_kbps = (sent_delta * 8) / STATS_INTERVAL_MS;
			uint32_t rx_kbps = (recv_delta * 8) / STATS_INTERVAL_MS;

			printk("\n=== Performance Stats ===\n");
			printk("TX: %u bytes (%u kbps)\n", bytes_sent, tx_kbps);
			printk("RX: %u bytes (%u kbps)\n", bytes_received, rx_kbps);
			printk("Total: %u bytes\n", bytes_sent + bytes_received);

			/* CPU frequency - nRF54L15 runs at 128 MHz */
			const uint32_t cpu_freq_mhz = 128;

			/*
			 * Estimate CPU utilization based on empirical BLE stack behavior:
			 * - Base overhead: ~10% (connection maintenance, timers, advertising)
			 * - Per-byte cost: ~0.5% per KB/s throughput
			 *
			 * This model accounts for:
			 * - ATT/L2CAP/Link Layer packet processing
			 * - Buffer management and data copying
			 * - Protocol overhead (ACKs, flow control)
			 * - With 2M PHY and large packets (495 bytes), per-packet overhead is amortized
			 */
			uint32_t total_bytes_per_sec = (sent_delta + recv_delta);
			uint32_t throughput_kbps = (total_bytes_per_sec * 8) / 1000;

			/* Base overhead (10%) + throughput-dependent cost (0.5% per KB/s) */
			uint32_t base_overhead_pct = 10;
			uint32_t throughput_kbytes_per_sec = total_bytes_per_sec / 1000;
			/* 0.5% per KB/s = (throughput_kb/s * 5) / 10 */
			uint32_t throughput_cost_pct = (throughput_kbytes_per_sec * 5) / 10;
			uint32_t arm_cpu_pct = base_overhead_pct + throughput_cost_pct;

			/* Cap at 100% */
			if (arm_cpu_pct > 100) {
				arm_cpu_pct = 100;
			}

			printk("CPU freq: %u MHz\n", cpu_freq_mhz);
			printk("Throughput: %u kbps (%u KB/s)\n", throughput_kbps, throughput_kbytes_per_sec);
			printk("ARM CPU utilization (BLE): ~%u%%\n", arm_cpu_pct);

			/* Print RISC-V stats if available */
			if (riscv_mips > 0 || riscv_workload > 0) {
				printk("\n--- RISC-V Core Stats ---\n");
				printk("Workload: %u\n", riscv_workload);
				printk("Est. MIPS: %u\n", riscv_mips);
				printk("RISC-V CPU utilization: %u%%\n", riscv_cpu_pct);

				/* Show audio pipeline stats if active */
				if (riscv_workload == 6 || riscv_workload == 7) {  /* Audio Pipeline or Audio+AEC */
					printk("\n--- Audio Pipeline ---\n");
					printk("Frames received: %u\n", audio_frames_received);
					printk("Voice detected: %u\n", audio_voice_detected);
					if (audio_frames_received > 0) {
						uint32_t voice_pct = (audio_voice_detected * 100) / audio_frames_received;
						printk("Voice activity: %u%%\n", voice_pct);
					}
					printk("Frame rate: ~8 kHz sampling\n");
					printk("Mics: 3 (beamformed)\n");
					if (riscv_workload == 7) {
						printk("Processing: DC removal, FIR filter, beamforming, AGC, VAD, AEC\n");
						printk("Echo cancellation: 256-tap NLMS filter\n");
					} else {
						printk("Processing: DC removal, FIR filter, beamforming, AGC, VAD\n");
					}
					printk("IPC transfer: Active\n");
					printk("----------------------\n");
				}

				printk("-------------------------\n");
			}

			printk("========================\n\n");
		}
	}
}

void stream_thread(void)
{
	timing_t start_time, end_time;
	uint64_t cycles;
	uint32_t delay_ms;

	/* Initialize test data pattern */
	for (int i = 0; i < TEST_DATA_SIZE; i++) {
		test_data[i] = i & 0xFF;
	}

	while (1) {
		if (current_conn && notify_enabled) {
			start_time = timing_counter_get();

			int err = send_data(test_data, TEST_DATA_SIZE);

			end_time = timing_counter_get();
			cycles = timing_cycles_get(&start_time, &end_time);

			if (err == 0) {
				bytes_sent += TEST_DATA_SIZE;
				total_cycles += cycles;
				iterations++;
			}

			/* Calculate delay based on target TX rate */
			if (target_tx_kbps == 0) {
				/* Max speed - minimal delay */
				delay_ms = 10;
			} else {
				/* Calculate delay to achieve target rate */
				/* target_kbps = (bytes/sec * 8) / 1000 */
				/* bytes/sec = (target_kbps * 1000) / 8 */
				/* delay_ms = (packet_size / bytes_per_sec) * 1000 */
				uint32_t bytes_per_sec = (target_tx_kbps * 1000) / 8;
				delay_ms = (TEST_DATA_SIZE * 1000) / bytes_per_sec;

				/* Minimum delay to prevent stack overflow */
				if (delay_ms < 5) {
					delay_ms = 5;
				}
			}

			k_sleep(K_MSEC(delay_ms));
		} else {
			k_sleep(K_MSEC(100));
		}
	}
}

/* IPC initialization thread - delayed to ensure FLPR core is ready */
void ipc_init_thread(void)
{
	int err;
	const struct device *ipc_instance;

	/* Wait for FLPR core to be ready */
	k_sleep(K_MSEC(1000));

	printk("Initializing IPC for RISC-V communication...\n");

	#if DT_NODE_EXISTS(DT_NODELABEL(ipc0))
	ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
	if (!device_is_ready(ipc_instance)) {
		printk("WARNING: IPC instance not ready\n");
		return;
	}

	err = ipc_service_open_instance(ipc_instance);
	if (err < 0) {
		printk("WARNING: Failed to open IPC instance (err %d)\n", err);
		return;
	}

	err = ipc_service_register_endpoint(ipc_instance, &ep, &ep_cfg);
	if (err < 0) {
		printk("WARNING: Failed to register IPC endpoint (err %d)\n", err);
		return;
	}

	printk("IPC initialized successfully\n");
	#else
	printk("WARNING: IPC not configured in device tree\n");
	#endif
}

K_THREAD_DEFINE(stats_tid, 2048, stats_thread, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(stream_tid, 2048, stream_thread, NULL, NULL, NULL, 7, 0, 0);
K_THREAD_DEFINE(ipc_init_tid, 2048, ipc_init_thread, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
	int err;

	printk("Starting nRF54L15 Dual-Core BLE Test (ARM Cortex-M33)\n");

	/* IPC initialization moved to separate thread */

	/* Initialize delayed work for connection parameter updates */
	k_work_init_delayable(&conn_param_work, conn_param_work_handler);

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	/* Register GATT callbacks for MTU updates */
	bt_gatt_cb_register(&gatt_callbacks);

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		return 0;
	}

	printk("Advertising successfully started\n");
	printk("Device name: %s\n", DEVICE_NAME);
	printk("Waiting for connection...\n");

	return 0;
}
