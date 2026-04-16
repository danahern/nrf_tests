/*
 * Phase 4 BLE bring-up public contract.
 *
 * Call ble_init() from main() after core subsystems (audio, link,
 * ipc_service) are up — bt_enable() triggers the CYW55513 firmware
 * download which takes ~1–2 s; doing it later keeps early-boot logs
 * clean.
 */
#ifndef PSE84_ASSISTANT_BLE_H_
#define PSE84_ASSISTANT_BLE_H_

#include <zephyr/kernel.h>

int ble_init(void);

/* Block caller until bt_enable completed (or timeout). Returns 0 if
 * BT came up within the timeout, -EAGAIN on timeout. When
 * CONFIG_BT is disabled, returns 0 immediately (no-op). */
int ble_wait_ready(k_timeout_t timeout);

#endif
