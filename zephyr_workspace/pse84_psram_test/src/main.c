#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* Diagnose M55→SMIF1 bus routing. Each probe uses aligned loads and
 * prints BEFORE and AFTER so a bus fault between prints identifies
 * which address the bridge rejected.
 */
int main(void)
{
	k_msleep(2000);
	printk("== psram bridge probe ==\n");
	k_msleep(200);

	printk("1. SMIF0 CTL reg @0x54440000 read (known good)...\n");
	k_msleep(200);
	uint32_t smif0_ctl = *(volatile uint32_t *)0x54440000U;
	printk("   SMIF0 CTL = 0x%08x\n", smif0_ctl);
	k_msleep(200);

	printk("2. SMIF1 CTL reg @0x54480000 read...\n");
	k_msleep(200);
	uint32_t smif1_ctl = *(volatile uint32_t *)0x54480000U;
	printk("   SMIF1 CTL = 0x%08x\n", smif1_ctl);
	k_msleep(200);

	printk("3. SMIF1 XIP @0x64000000 read...\n");
	k_msleep(200);
	uint32_t x = *(volatile uint32_t *)0x64000000U;
	printk("   XIP[0] = 0x%08x\n", x);

	while (1) { k_msleep(1000); }
	return 0;
}
