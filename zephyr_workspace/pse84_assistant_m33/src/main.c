/*
 * PSE84 Voice Assistant — M33 companion.
 *
 * Replaces samples/basic/minimal in the kit_pse84_eval sysbuild
 * companion slot. ifx_pse84_cm55_startup() has already run via the
 * SoC's soc_late_init_hook by the time main() is entered; so this
 * thread is what keeps M33 alive and will eventually host:
 *   - Zephyr Bluetooth (HCI UART to onboard CYW55513)        [Phase 4]
 *   - ipc_service peer for the M55 endpoint registered in    [Phase 0b.7+]
 *     src/main.c on the M55 side ('assistant' endpoint on
 *     assistant_ipc0).
 *
 * For Phase 0b.7 the loop is just a heartbeat log — proof that the
 * new companion image boots in place of samples/basic/minimal.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pse84_assistant_m33, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("=== PSE84 M33 companion (Phase 0b.7 skeleton) ===");

	uint32_t tick = 0;

	while (1) {
		k_sleep(K_SECONDS(5));
		LOG_INF("m33 heartbeat tick=%u", ++tick);
	}
	return 0;
}
