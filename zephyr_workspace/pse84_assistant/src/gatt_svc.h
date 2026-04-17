/*
 * PSE84 Voice Assistant — GATT data service.
 *
 * Custom service with TX (notify) + RX (write) characteristics.
 * Same framing protocol as L2CAP CoC but over GATT — works with
 * bleak on every platform.
 */
#ifndef PSE84_ASSISTANT_GATT_SVC_H_
#define PSE84_ASSISTANT_GATT_SVC_H_

#include <stdbool.h>
#include <stdint.h>

/* Custom service UUIDs */
#define ASST_SERVICE_UUID_VAL \
	BT_UUID_128_ENCODE(0xa0e70001, 0xe8b0, 0x4aba, 0x8200, 0xa0e7a0e7a0e7)
#define ASST_TX_CHAR_UUID_VAL \
	BT_UUID_128_ENCODE(0xa0e70002, 0xe8b0, 0x4aba, 0x8200, 0xa0e7a0e7a0e7)
#define ASST_RX_CHAR_UUID_VAL \
	BT_UUID_128_ENCODE(0xa0e70003, 0xe8b0, 0x4aba, 0x8200, 0xa0e7a0e7a0e7)

int gatt_svc_init(void);

/* Send a pre-encoded frame via GATT notify. Returns 0 on success,
 * -ENOTCONN if no subscribed client. Caller owns the buffer. */
int gatt_svc_send(const uint8_t *data, uint16_t len);

bool gatt_svc_is_connected(void);

#endif
