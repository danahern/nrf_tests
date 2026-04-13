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

	/* SMIF0 / NOR flash (XIP read test).
	 * Default boot: S25FS128S Quad NOR on CS1, 16 MB aperture
	 * 0x6000_0000 .. 0x60FF_FFFF. M55 XIP runs from 0x6050_0000.
	 *
	 * With CONFIG_INFINEON_SMIF_OCTAL=y, boot switches to S28HS01GT
	 * Octal NOR on CS0 (1 Gbit = 128 MB — "our 1 Gbit flash"), with
	 * 64 MB accessible via XIP and upper 64 MB requiring MMIO. That
	 * transition is currently blocked by a Cy_SMIF_BusyCheck hang
	 * (separate debug — see handoff §?).
	 *
	 * For this build (Quad NOR), stay inside the 16 MB aperture.
	 */
	printk("-- SMIF0 Quad NOR read (XIP, 16 MB aperture) --\n"); k_msleep(100);
	volatile const uint32_t *f_base = (volatile const uint32_t *)0x60500000U; /* M55 image start */
	volatile const uint32_t *f_mid  = (volatile const uint32_t *)0x60800000U; /* 8 MB mark */
	volatile const uint32_t *f_end  = (volatile const uint32_t *)0x60F00000U; /* 15 MB — inside 16 MB */

	printk("img start @0x60500000 = 0x%08x (M55 vector[0] = initial SP)\n", f_base[0]); k_msleep(50);
	printk("mid       @0x60800000 = 0x%08x\n", f_mid[0]); k_msleep(50);
	printk("near end  @0x60F00000 = 0x%08x (likely 0xFFFFFFFF — erased)\n", f_end[0]); k_msleep(100);

	printk("== probe done ==\n");
	while (1) { k_msleep(1000); }
	return 0;
}
