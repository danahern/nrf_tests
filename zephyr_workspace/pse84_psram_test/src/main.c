#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	k_msleep(2000);
	printk("== smif0+smif1 combined probe ==\n");
	k_msleep(200);

	/* SMIF1 / PSRAM (S70KS1283 HyperRAM, 16 MB).
	 * NS XIP aperture: 0x6400_0000 .. 0x64FF_FFFF.
	 * Read/write test — expect pre-state 0 (or stale canary),
	 * write distinct canary, read back same value.
	 */
	printk("-- SMIF1 PSRAM read/write --\n"); k_msleep(100);
	volatile uint32_t *psram0 = (volatile uint32_t *)0x64000000U;
	volatile uint32_t *psram1 = (volatile uint32_t *)0x64010000U;
	volatile uint32_t *psramN = (volatile uint32_t *)0x64800000U; /* +8 MB */

	printk("pre  @0x64000000 = 0x%08x\n", psram0[0]); k_msleep(50);
	printk("pre  @0x64010000 = 0x%08x\n", psram1[0]); k_msleep(50);
	printk("pre  @0x64800000 = 0x%08x\n", psramN[0]); k_msleep(50);

	psram0[0] = 0xCAFEBABEU;
	psram1[0] = 0xDEADBEEFU;
	psramN[0] = 0x12345678U;
	__DSB(); __ISB();

	printk("post @0x64000000 = 0x%08x (want CAFEBABE)\n", psram0[0]); k_msleep(50);
	printk("post @0x64010000 = 0x%08x (want DEADBEEF)\n", psram1[0]); k_msleep(50);
	printk("post @0x64800000 = 0x%08x (want 12345678)\n", psramN[0]); k_msleep(100);

	/* SMIF0 / Octal NOR (S28HS01GT 1 Gbit = 128 MB, CS0 OPI DDR).
	 * With CONFIG_INFINEON_SMIF_OCTAL=y, 64 MB accessible via XIP at
	 * 0x6000_0000 .. 0x63FF_FFFF (upper 64 MB would need MMIO).
	 * Read-only probe across the aperture.
	 */
	printk("-- SMIF0 Octal NOR read (XIP, 64 MB aperture) --\n"); k_msleep(100);
	volatile const uint32_t *f_m55  = (volatile const uint32_t *)0x60500000U; /* M55 img start */
	volatile const uint32_t *f_8M   = (volatile const uint32_t *)0x60800000U;
	volatile const uint32_t *f_16M  = (volatile const uint32_t *)0x61000000U; /* past quad chip */
	volatile const uint32_t *f_32M  = (volatile const uint32_t *)0x62000000U;
	volatile const uint32_t *f_60M  = (volatile const uint32_t *)0x63C00000U; /* near end */

	printk("M55 img  @0x60500000 = 0x%08x (vector[0] = initial SP)\n", f_m55[0]); k_msleep(50);
	printk("+8 MB    @0x60800000 = 0x%08x\n", f_8M[0]); k_msleep(50);
	printk("+16 MB   @0x61000000 = 0x%08x (proves >16 MB — octal active)\n", f_16M[0]); k_msleep(50);
	printk("+32 MB   @0x62000000 = 0x%08x\n", f_32M[0]); k_msleep(50);
	printk("+60 MB   @0x63C00000 = 0x%08x (near end of 64 MB XIP)\n", f_60M[0]); k_msleep(100);

	printk("== probe done ==\n");
	while (1) { k_msleep(1000); }
	return 0;
}
