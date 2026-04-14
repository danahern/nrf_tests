/*
 * PSE84 Voice Assistant — host link layer.
 *
 * Consumes incoming L2CAP-style frames from the host bridge over the
 * same UART that ships PCM hex dumps back. For this PoC the UART is the
 * Zephyr console uart2 — the bridge writes binary-framed TEXT_CHUNK /
 * TEXT_END packets into /dev/cu.usbmodemXXXX and we parse them here,
 * route TEXT_CHUNK payloads into the LVGL reply label, and drive the
 * state machine IDLE → RESPONDING → IDLE on frame boundaries.
 *
 * Requires CONFIG_SHELL=n so the shell subsystem doesn't swallow our
 * RX bytes. Uses the Zephyr uart_irq API (no async DMA, no ring buffer
 * needed at this frame rate).
 */

#ifndef PSE84_ASSISTANT_LINK_H_
#define PSE84_ASSISTANT_LINK_H_

int link_init(void);

#endif /* PSE84_ASSISTANT_LINK_H_ */
