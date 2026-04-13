#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	k_msleep(2000);
	printk("== smif1 range probe ==\n");
	k_msleep(200);

	printk("(M33 init either completed cleanly or crashed; fault dump above if any)\n");
	printk("-- M55 PSRAM read/write test --\n"); k_msleep(200);

	volatile uint32_t *p0 = (volatile uint32_t *)0x64000000U;
	volatile uint32_t *p1 = (volatile uint32_t *)0x64010000U;
	volatile uint32_t *pN = (volatile uint32_t *)0x64800000U; /* +8 MB */

	printk("pre  @0x64000000 = 0x%08x\n", p0[0]); k_msleep(100);
	printk("pre  @0x64010000 = 0x%08x\n", p1[0]); k_msleep(100);
	printk("pre  @0x64800000 = 0x%08x\n", pN[0]); k_msleep(100);

	printk("writing canaries...\n"); k_msleep(100);
	p0[0] = 0xCAFEBABEU;
	p1[0] = 0xDEADBEEFU;
	pN[0] = 0x12345678U;
	__DSB(); __ISB();

	printk("post @0x64000000 = 0x%08x (want CAFEBABE)\n", p0[0]); k_msleep(100);
	printk("post @0x64010000 = 0x%08x (want DEADBEEF)\n", p1[0]); k_msleep(100);
	printk("post @0x64800000 = 0x%08x (want 12345678)\n", pN[0]); k_msleep(100);

	while (1) { k_msleep(1000); }
	return 0;
}
