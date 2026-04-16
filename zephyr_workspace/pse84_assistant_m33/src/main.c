/*
 * PSE84 Voice Assistant — M33 companion.
 *
 * Phase 0b.8: M33 owns uart2 and acts as the log printer for both
 * cores. M55 routes its LOG_INF/WRN/ERR lines via the 'assistant'
 * ipc_service endpoint (icmsg over infineon,pse84-mbox); here on M33
 * we register the peer endpoint and printk every received byte
 * straight to uart2 with a "[m55] " prefix per line.
 *
 * CM55 release is handled by soc_late_init_hook via
 * CONFIG_SOC_PSE84_M55_ENABLE=y → ifx_pse84_cm55_startup() in
 * soc/infineon/edge/pse84/security_config/pse84_boot.c. That path
 * runs cy_mpc_init / cy_ppc_init / Cy_SysEnableCM55 cleanly when
 * extended-boot enters via the alt-boot path
 * (policy_oem_octal.json: oem_alt_boot=true,
 * oem_alt_app_address=0x70100000, P17.6 high on kit_pse84_eval
 * forces alt selection). Under alt_boot=false, re-running
 * cy_mpc_init bus-faults on APPCPUSS MPC rows extended-boot has
 * already attributed — so alt_boot is the compat sweet spot.
 *
 * Phase 4 will add Bluetooth (HCI UART → CYW55513) on top of this
 * skeleton; the same ipc endpoint will gain a framed-data path then.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys/printk.h>
LOG_MODULE_REGISTER(pse84_assistant_m33, LOG_LEVEL_INF);

static void ep_recv(const void *data, size_t len, void *priv)
{
	ARG_UNUSED(priv);
	/* Tag relayed M55 log lines with "[m55] " at each logical line
	 * start so the serial console can tell cores apart. State is
	 * carried across calls because log_output may flush a line as
	 * multiple ipc_service_send chunks.
	 */
	static bool at_line_start = true;
	const char *p = data;
	for (size_t i = 0; i < len; i++) {
		char c = p[i];
		if (at_line_start && c != '\n' && c != '\r') {
			printk("[m55] ");
			at_line_start = false;
		}
		printk("%c", c);
		if (c == '\n') {
			at_line_start = true;
		}
	}
}

static void ep_bound(void *priv)
{
	ARG_UNUSED(priv);
	printk("[m33] ipc 'assistant' endpoint bound — log tunnel live\n");
}

int main(void)
{
	printk("=== PSE84 M33 companion (Phase 0b.8 log sink) ===\n");

	const struct device *ipc_dev =
		DEVICE_DT_GET(DT_NODELABEL(assistant_ipc0));
	static struct ipc_ept ep;
	static const struct ipc_ept_cfg cfg = {
		.name = "assistant",
		.cb = {
			.bound = ep_bound,
			.received = ep_recv,
		},
	};

	if (!device_is_ready(ipc_dev)) {
		printk("[m33] assistant_ipc0 not ready\n");
	} else if (ipc_service_open_instance(ipc_dev) < 0) {
		printk("[m33] ipc_service_open_instance failed\n");
	} else if (ipc_service_register_endpoint(ipc_dev, &ep, &cfg) < 0) {
		printk("[m33] ipc_service_register_endpoint failed\n");
	} else {
		printk("[m33] ipc 'assistant' endpoint registered\n");
	}

	uint32_t tick = 0;
	while (1) {
		k_sleep(K_SECONDS(5));
		printk("[m33] heartbeat tick=%u\n", ++tick);
	}
	return 0;
}
