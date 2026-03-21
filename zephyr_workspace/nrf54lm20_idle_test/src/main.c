/*
 * nRF54LM20 Deep Sleep Idle Test
 *
 * Enters lowest power idle state with periodic 1-second RTC wakeup.
 * All peripherals disabled for minimum power consumption.
 */

#include <zephyr/kernel.h>

int main(void)
{
	while (1) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}
