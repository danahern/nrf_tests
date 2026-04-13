#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	k_msleep(2000);
	printk("== smif1 range probe ==\n");
	k_msleep(200);

	printk("(M33 init either completed cleanly or crashed; fault dump above if any)\n");
	printk("-- now M55 probing 0x64xxxxxx --\n"); k_msleep(200);

	printk("0x64000000 (start)...\n"); k_msleep(200);
	uint32_t a = *(volatile uint32_t *)0x64000000U;
	printk("  = 0x%08x\n", a); k_msleep(200);

	printk("0x64010000 (+64K)...\n"); k_msleep(200);
	uint32_t b = *(volatile uint32_t *)0x64010000U;
	printk("  = 0x%08x\n", b); k_msleep(200);

	while (1) { k_msleep(1000); }
	return 0;
}
