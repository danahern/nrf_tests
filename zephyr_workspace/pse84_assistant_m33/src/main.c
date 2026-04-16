/*
 * PSE84 Voice Assistant — M33 companion.
 *
 * Diagnostic build for HardFault RCA: main() writes a marker to SRAM
 * (0x34002000+4 = past vector table) then busy-loops incrementing
 * a counter. No k_sleep → Zephyr idle thread never runs → no WFI.
 *
 * If main() is reached, the SRAM marker will read as 0xBEEF0000+.
 * If M33 HardFaults before main(), the marker stays at whatever
 * Zephyr's .bss/.data init leaves (typically 0).
 *
 * Post-diagnostic this gets replaced with the real IPC endpoint flow.
 */
#include <zephyr/kernel.h>

#define DIAG_MARKER_ADDR 0x34039f00U

int main(void)
{
	volatile uint32_t *marker = (volatile uint32_t *)DIAG_MARKER_ADDR;
	volatile uint32_t counter = 0;

	*marker = 0xBEEF0001U;

	while (1) {
		counter++;
		*marker = 0xBEEF0000U | (counter & 0xFFFFU);
	}
	return 0;
}
