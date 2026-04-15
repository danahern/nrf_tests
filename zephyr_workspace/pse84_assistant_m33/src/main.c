/*
 * PSE84 Voice Assistant — M33 companion.
 *
 * Phase 0b.8: M33 owns uart2 and acts as the log printer for both
 * cores. M55 disables CONFIG_LOG_BACKEND_UART and routes its
 * LOG_INF/WRN/ERR lines via the 'assistant' ipc_service endpoint
 * (icmsg over infineon,pse84-mbox). Here on M33 we register the peer
 * endpoint and printk every received byte straight to uart2.
 *
 * Phase 4 will add Bluetooth (HCI UART -> CYW55513) on top of this
 * skeleton; the same ipc endpoint will gain a framed-data path then.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys/printk.h>

#include "cy_syslib.h"
#include "cy_sysclk.h"
#include "cy_syspm.h"

LOG_MODULE_REGISTER(pse84_assistant_m33, LOG_LEVEL_INF);

/* Release CM55 from CPU_WAIT. policy_oem_octal makes extended-boot
 * launch the M33 image but does NOT self-release CM55 (confirmed by
 * reading CM55 registers post-boot: PC=0x0003ff00, SP=0x100).
 * Extended-boot ALSO already ran the MPC/PPC/SAU init per policy, so
 * calling Zephyr's ifx_pse84_cm55_startup() (which re-runs
 * cy_mpc_init) bus-faults on APPCPUSS MPC writes. Instead do the
 * minimum: enable PD1 + clocks + SOCMEM, then release via
 * Cy_SysEnableCM55 (which handles APPCPU PPU + vector + CPU_WAIT). */
#define CM55_BOOT_WAIT_TIME_USEC 100U
static void release_cm55(void)
{
	/* Our m55_xip partition is at 0x60500000 (SMIF0 NS S-AHB alias).
	 * CM55 boots in Secure state. Use Secure-alias 0x70500000 for
	 * CM55_S_VECTOR_TABLE_BASE so the initial MSP/PC fetches go
	 * through Secure bus. Same physical SMIF0 memory. */
	const uintptr_t m55_vectors = DT_REG_ADDR(DT_NODELABEL(m55_xip));
	const uintptr_t m55_vectors_s = m55_vectors | 0x10000000UL;
	printk("[m33] bringing up CM55: PD1 + peri groups\n");

	Cy_System_EnablePD1();
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_CM55_TCM_512K_PERI_NR,
				     CY_MMIO_CM55_TCM_512K_GROUP_NR,
				     CY_MMIO_CM55_TCM_512K_SLAVE_NR,
				     CY_MMIO_CM55_TCM_512K_CLK_HF_NR);
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SMIF0_PERI_NR,
				     CY_MMIO_SMIF0_GROUP_NR,
				     CY_MMIO_SMIF0_SLAVE_NR,
				     CY_MMIO_SMIF0_CLK_HF_NR);
	Cy_SysClk_PeriGroupSlaveInit(CY_MMIO_SMIF01_PERI_NR,
				     CY_MMIO_SMIF01_GROUP_NR,
				     CY_MMIO_SMIF01_SLAVE_NR,
				     CY_MMIO_SMIF01_CLK_HF_NR);
	Cy_SysEnableSOCMEM(true);

	printk("[m33] pre-release CM55: CTL=0x%08lx STATUS=0x%08lx CMD=0x%08lx S_VEC=0x%08lx NS_VEC=0x%08lx\n",
	       (unsigned long)MXCM55->CM55_CTL,
	       (unsigned long)MXCM55->CM55_STATUS,
	       (unsigned long)MXCM55->CM55_CMD,
	       (unsigned long)MXCM55->CM55_S_VECTOR_TABLE_BASE,
	       (unsigned long)MXCM55->CM55_NS_VECTOR_TABLE_BASE);

	/* CRITICAL: set S_VECTOR_TABLE_BASE BEFORE Cy_SysEnableCM55.
	 * ARMv8.1-M CM55 boots Secure and fetches MSP/PC from the Secure
	 * VTOR at reset. Cy_SysEnableCM55 only writes NS_VECTOR_TABLE_BASE,
	 * so if S_VECTOR stays at 0x10000000 (ITCM default, pre-cleared to
	 * zeros) CM55 jumps to NULL reset handler and faults silently.
	 *
	 * S_VEC uses Secure SBUS alias 0x70500000, NS_VEC uses NS SBUS
	 * 0x60500000 — same physical SMIF0 NOR flash.
	 */
	MXCM55->CM55_S_VECTOR_TABLE_BASE = m55_vectors_s;
	MXCM55->CM55_NS_VECTOR_TABLE_BASE = m55_vectors;

	printk("[m33] vectors set, calling Cy_SysEnableCM55 (waitus=100000)\n");
	Cy_SysEnableCM55(MXCM55, m55_vectors, 100000U);

	printk("[m33] post-PDL: CTL=0x%08lx STATUS=0x%08lx CMD=0x%08lx\n",
	       (unsigned long)MXCM55->CM55_CTL,
	       (unsigned long)MXCM55->CM55_STATUS,
	       (unsigned long)MXCM55->CM55_CMD);

	/* Explicit reset pulse — CM55_CMD.RESET=1 with correct key 0x05FA.
	 * HW auto-clears RESET bit, producing a reset pulse on the core.
	 * The PDL's Cy_SysCM55Reset does exactly this. Use infinite wait
	 * so we block until HW confirms the reset completed. */
	Cy_SysCM55Reset(MXCM55, CY_SYS_CORE_WAIT_INFINITE);

	/* Belt-and-braces CPU_WAIT clear with the correct key-position
	 * write (0x05FA in [31:16]). */
	MXCM55->CM55_CTL = 0;

	k_sleep(K_MSEC(200));

	printk("[m33] final: CTL=0x%08lx STATUS=0x%08lx CMD=0x%08lx S_VEC=0x%08lx\n",
	       (unsigned long)MXCM55->CM55_CTL,
	       (unsigned long)MXCM55->CM55_STATUS,
	       (unsigned long)MXCM55->CM55_CMD,
	       (unsigned long)MXCM55->CM55_S_VECTOR_TABLE_BASE);
}

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

	release_cm55();

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
