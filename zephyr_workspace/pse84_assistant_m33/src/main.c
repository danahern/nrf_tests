/*
 * PSE84 Voice Assistant — M33 companion (silent).
 *
 * M33 just runs soc_late_init_hook (CONFIG_SOC_PSE84_M55_ENABLE=y in
 * sysbuild/enable_cm55.conf → ifx_pse84_cm55_startup handles MPC/PPC
 * attribution + CM55 release) and idles. No SERIAL, no IPC/MBOX —
 * any peripheral driver probe here bus-faults because M33 doesn't
 * have PPC access until soc_late_init_hook, which runs AFTER driver
 * init. See prj.conf for the incompatible configs list.
 */
#include <zephyr/kernel.h>

int main(void)
{
	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}
