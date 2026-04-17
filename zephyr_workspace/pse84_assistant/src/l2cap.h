/*
 * PSE84 Voice Assistant — L2CAP CoC data channel.
 *
 * Registers an L2CAP server on a fixed LE_PSM. When a macOS host
 * connects and opens a CoC channel, incoming SDUs are fed through
 * the framing parser (same dispatch as the UART link layer) and
 * outgoing frames can be pushed via l2cap_send_frame().
 */
#ifndef PSE84_ASSISTANT_L2CAP_H_
#define PSE84_ASSISTANT_L2CAP_H_

#include <stdbool.h>
#include <stdint.h>

#define L2CAP_PSM 0x0080

int l2cap_init(void);

/* Send a pre-encoded frame over the L2CAP channel. Returns 0 on
 * success, -ENOTCONN if no peer, negative errno on send failure.
 * Caller owns the data buffer; l2cap_send_frame copies into a
 * net_buf internally. */
int l2cap_send_frame(const uint8_t *data, uint16_t len);

/* True if a peer is connected and the CoC channel is open. */
bool l2cap_is_connected(void);

#endif
