#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	k_msleep(2000);
	printk("== psram test ==\n");

	volatile uint32_t *status = (volatile uint32_t *)0x26250000U;
	printk("stage:  0x%08x\n", status[0]);
	printk("setup:  0x%08x\n", status[1]);
	printk("xip:    0x%08x\n", status[2]);
	printk("we:     0x%08x\n", status[3]);
	printk("m33rd:  0x%08x (expect 0x11223344)\n", status[4]);
	k_msleep(200);

	printk("now read 0x64000000...\n");
	k_msleep(200);
	volatile uint32_t *p = (volatile uint32_t *)0x64000000U;
	uint32_t r = p[0];
	printk("read ok: 0x%08x\n", r);

	while (1) { k_msleep(1000); }
	return 0;
}
