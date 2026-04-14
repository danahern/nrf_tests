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

int ble_init(void);

#endif
