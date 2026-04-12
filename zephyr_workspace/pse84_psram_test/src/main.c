/*
 * PSE84 PSRAM (S70KS1283 HyperRAM on SMIF1) test.
 *
 * The M33 secure companion has already initialized SMIF1 via
 * ifx_pse84_psram_init (gated on CONFIG_INFINEON_SMIF_PSRAM).
 * By the time this runs on M55, 0x64000000 should be read/write
 * memory-mapped to 16 MB of HyperRAM at ~400 MBps DDR.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define PSRAM_BASE 0x64000000U
#define PSRAM_SIZE (16U * 1024U * 1024U)

static volatile uint32_t *const psram = (volatile uint32_t *)PSRAM_BASE;

static void test_single_word(void)
{
	psram[0] = 0xDEADBEEFU;
	psram[1] = 0xCAFEBABEU;

	uint32_t r0 = psram[0];
	uint32_t r1 = psram[1];

	printk("single: [0]=0x%08x (expect 0xDEADBEEF) [1]=0x%08x (expect 0xCAFEBABE)\n",
	       r0, r1);
}

static void test_walking_pattern(void)
{
	/* Write an incrementing pattern to the first 256 KB and verify. */
	const uint32_t count = 256U * 1024U / 4U;
	uint32_t i, mismatches = 0;

	for (i = 0; i < count; i++) {
		psram[i] = i ^ 0xA5A5A5A5U;
	}

	for (i = 0; i < count; i++) {
		uint32_t expected = i ^ 0xA5A5A5A5U;
		uint32_t got = psram[i];

		if (got != expected) {
			if (mismatches < 4) {
				printk("mismatch @[%u]: 0x%08x != 0x%08x\n",
				       i, got, expected);
			}
			mismatches++;
		}
	}
	printk("walk: %u words, %u mismatches\n", count, mismatches);
}

static void test_endpoints(void)
{
	/* Poke first and last 4 bytes of the 16 MB aperture. */
	psram[0] = 0x11111111U;
	psram[(PSRAM_SIZE - 4U) / 4U] = 0x22222222U;

	printk("endpoints: [0x%08x]=0x%08x (expect 0x11111111)\n",
	       PSRAM_BASE, psram[0]);
	printk("endpoints: [0x%08x]=0x%08x (expect 0x22222222)\n",
	       PSRAM_BASE + PSRAM_SIZE - 4U,
	       psram[(PSRAM_SIZE - 4U) / 4U]);
}

int main(void)
{
	printk("=== PSE84 PSRAM test ===\n");
	printk("PSRAM base: 0x%08x  size: %u MB\n",
	       PSRAM_BASE, PSRAM_SIZE / (1024U * 1024U));

	k_msleep(500);

	test_single_word();
	test_walking_pattern();
	test_endpoints();

	printk("=== PSRAM test done ===\n");

	while (1) {
		k_msleep(1000);
	}
	return 0;
}
