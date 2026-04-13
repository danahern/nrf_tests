#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	k_msleep(2000);
	printk("== psram test ==\n");
	k_msleep(200);

	volatile uint32_t *p = (volatile uint32_t *)0x64000000U;

	printk("read [0]...\n");
	k_msleep(200);
	uint32_t r0 = p[0];
	printk("[0] = 0x%08x\n", r0);
	k_msleep(200);

	printk("write 0xDEADBEEF @[0]...\n");
	k_msleep(200);
	p[0] = 0xDEADBEEFU;
	printk("readback: 0x%08x\n", p[0]);

	while (1) { k_msleep(1000); }
	return 0;
}
