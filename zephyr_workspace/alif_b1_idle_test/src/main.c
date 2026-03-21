/*
 * Alif B1 Deep Sleep Idle Test
 *
 * Enters lowest power idle state (STOP mode) with periodic 1-second wakeup.
 * All peripherals disabled for minimum power consumption.
 * Target: 700 nA (per Alif B1 datasheet).
 */

#include <zephyr/kernel.h>

int main(void)
{
	while (1) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}
