/*
 * PSE84 Voice Assistant — M33 companion (silent + IPC).
 *
 * Layout (SRAM markers — read via openocd `mdw`):
 *   0x34039f00  general state word: 0xBEEF<counter>
 *                 counter ticks fast in main loop
 *   0x34039f04  IPC state word: 0xC0FFE<state>
 *                 0=before init, 1=device-not-ready, 2=open-failed,
 *                 3=register-failed, 4=registered, 5=bound,
 *                 6=first-rx-received
 *   0x34039f08  IPC rx counter
 *   0x34039f0c  IPC last rx-byte size
 *   0x34039f10  scratch buffer (256 B) for last rx payload
 *
 * No SERIAL → no console_out → no SCB2 fault. mbox driver uses NS
 * alias 0x422A0000 which Cortex-M33 Secure can still access after
 * cy_ppc_init flips PERI0 to NS.
 *
 * M33-Secure also needs Cy_IPC_Pipe_Config + Cy_IPC_Sema_Init at
 * main() entry — M55 gets this via soc_early_init_hook on its side,
 * but the CM33-Secure SOC code path has no equivalent and Zephyr
 * ipc_service icmsg backend asserts z_spin_lock_valid without it.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>

#include "cy_ipc_pipe.h"
#include "cy_ipc_sema.h"

#define DIAG_MARKER_ADDR  0x34039f00U
#define IPC_STATE_ADDR    0x34039f04U
#define IPC_RX_COUNT_ADDR 0x34039f08U
#define IPC_LAST_LEN_ADDR 0x34039f0cU
#define IPC_SCRATCH_ADDR  0x34039f10U
#define IPC_SCRATCH_SIZE  256U

static volatile uint32_t *const diag_marker  = (volatile uint32_t *)DIAG_MARKER_ADDR;
static volatile uint32_t *const ipc_state    = (volatile uint32_t *)IPC_STATE_ADDR;
static volatile uint32_t *const ipc_rx_count = (volatile uint32_t *)IPC_RX_COUNT_ADDR;
static volatile uint32_t *const ipc_last_len = (volatile uint32_t *)IPC_LAST_LEN_ADDR;
static volatile uint8_t  *const ipc_scratch  = (volatile uint8_t  *)IPC_SCRATCH_ADDR;

#define IPC_STATE(s) (0xC0FFE000U | (s))

#define CY_IPC_MAX_ENDPOINTS (8UL)
static cy_stc_ipc_pipe_ep_t systemIpcPipeEpArray[CY_IPC_MAX_ENDPOINTS];

static void ep_recv(const void *data, size_t len, void *priv)
{
	ARG_UNUSED(priv);
	(*ipc_rx_count)++;
	*ipc_last_len = (uint32_t)len;
	if (*ipc_state < IPC_STATE(6)) {
		*ipc_state = IPC_STATE(6);
	}
	size_t copy = (len < IPC_SCRATCH_SIZE) ? len : IPC_SCRATCH_SIZE;
	const uint8_t *p = data;
	for (size_t i = 0; i < copy; i++) {
		ipc_scratch[i] = p[i];
	}
}

static void ep_bound(void *priv)
{
	ARG_UNUSED(priv);
	*ipc_state = IPC_STATE(5);
}

int main(void)
{
	*diag_marker  = 0xBEEF0001U;
	*ipc_state    = IPC_STATE(0);
	*ipc_rx_count = 0U;
	*ipc_last_len = 0U;

	Cy_IPC_Pipe_Config(systemIpcPipeEpArray);
	(void)Cy_IPC_Sema_Init(IPC0_SEMA_CH_NUM, 0UL, NULL);

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
		*ipc_state = IPC_STATE(1);
	} else if (ipc_service_open_instance(ipc_dev) < 0) {
		*ipc_state = IPC_STATE(2);
	} else if (ipc_service_register_endpoint(ipc_dev, &ep, &cfg) < 0) {
		*ipc_state = IPC_STATE(3);
	} else {
		*ipc_state = IPC_STATE(4);
	}

	volatile uint32_t counter = 0;
	while (1) {
		counter++;
		*diag_marker = 0xBEEF0000U | (counter & 0xFFFFU);
		/* k_yield (not k_sleep!) — yields CPU without queuing a
		 * timeout. icmsg work queue + ISR back-half can run here.
		 * k_sleep faults M33 in z_add_timeout assertion (the
		 * MCWDT/timer subsystem isn't fully sane on M33-Secure
		 * after PPC re-attribution). */
		k_yield();
	}
	return 0;
}
