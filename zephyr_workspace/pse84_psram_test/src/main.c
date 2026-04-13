#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	k_msleep(2000);
	printk("== smif1 range probe ==\n");
	k_msleep(200);

	printk("0x64000000 (start)...\n"); k_msleep(200);
	uint32_t a = *(volatile uint32_t *)0x64000000U;
	printk("  = 0x%08x\n", a); k_msleep(200);

	printk("0x64010000 (+64K)...\n"); k_msleep(200);
	uint32_t b = *(volatile uint32_t *)0x64010000U;
	printk("  = 0x%08x\n", b); k_msleep(200);

	printk("0x66000000 (+32M, outside cached region)...\n"); k_msleep(200);
	uint32_t c = *(volatile uint32_t *)0x66000000U;
	printk("  = 0x%08x\n", c);

	while (1) { k_msleep(1000); }
	return 0;
}
