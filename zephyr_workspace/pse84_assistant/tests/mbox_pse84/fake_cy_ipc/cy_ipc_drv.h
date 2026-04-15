/*
 * Copyright (c) 2026 PSE84 Voice Assistant contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only shim for the PSE84 Cy_IPC_Drv_* PDL. Exposes just the
 * subset that drivers/mbox/mbox_pse84.c consumes, plus a recording
 * harness so ztest can assert on the call sequence.
 *
 * Not a functional emulator — it simulates just enough state (the
 * 32-bit DATA register + pending release-mask) for the driver's
 * send/ISR paths to be exercised end-to-end.
 */

#ifndef MBOX_PSE84_FAKE_CY_IPC_DRV_H_
#define MBOX_PSE84_FAKE_CY_IPC_DRV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_CY_IPC_CHANNELS       16U
#define FAKE_CY_IPC_INTERRUPTS     16U

struct fake_ipc_struct {
	uint32_t data;
};

struct fake_ipc_intr_struct {
	uint32_t release_mask;      /* mask enabled for release event */
	uint32_t pending_release;   /* bits set by ReleaseNotify, cleared by ClearInterrupt */
};

typedef struct fake_ipc_struct      IPC_STRUCT_Type;
typedef struct fake_ipc_intr_struct IPC_INTR_STRUCT_Type;

/* PDL call trace entry — every call the driver makes is appended. */
enum fake_cy_ipc_call_id {
	FAKE_CY_IPC_CALL_ACQUIRE_NOTIFY,
	FAKE_CY_IPC_CALL_RELEASE_NOTIFY,
	FAKE_CY_IPC_CALL_WRITE_DATA,
	FAKE_CY_IPC_CALL_READ_DATA,
	FAKE_CY_IPC_CALL_CLEAR_INTERRUPT,
	FAKE_CY_IPC_CALL_SET_INTERRUPT_MASK,
};

struct fake_cy_ipc_call {
	enum fake_cy_ipc_call_id id;
	uintptr_t base;
	uint32_t arg0;
	uint32_t arg1;
};

#define FAKE_CY_IPC_TRACE_DEPTH 32

/* Test harness: reset recording, read trace. */
void fake_cy_ipc_reset(void);
size_t fake_cy_ipc_trace_len(void);
const struct fake_cy_ipc_call *fake_cy_ipc_trace_get(size_t idx);

/* Test harness: simulate peer releasing channel `channel` into the
 * release-notify IRQ struct `intr_index` with `data_value` in the
 * channel's DATA register. */
void fake_cy_ipc_simulate_release(uint32_t channel, uint32_t intr_index, uint32_t data_value);

/* PDL surface consumed by the driver. */
IPC_STRUCT_Type      *Cy_IPC_Drv_GetIpcBaseAddress(uint32_t ipcIndex);
IPC_INTR_STRUCT_Type *Cy_IPC_Drv_GetIntrBaseAddr(uint32_t ipcIntrIndex);

void     Cy_IPC_Drv_AcquireNotify(IPC_STRUCT_Type *base, uint32_t notifyEventIntr);
void     Cy_IPC_Drv_ReleaseNotify(IPC_STRUCT_Type *base, uint32_t notifyEventIntr);
void     Cy_IPC_Drv_WriteDataValue(IPC_STRUCT_Type *base, uint32_t dataValue);
uint32_t Cy_IPC_Drv_ReadDataValue(IPC_STRUCT_Type const *base);

void     Cy_IPC_Drv_SetInterruptMask(IPC_INTR_STRUCT_Type *base,
				     uint32_t ipcReleaseMask, uint32_t ipcNotifyMask);
uint32_t Cy_IPC_Drv_GetInterruptStatusMasked(IPC_INTR_STRUCT_Type const *base);
uint32_t Cy_IPC_Drv_ExtractReleaseMask(uint32_t intMask);
uint32_t Cy_IPC_Drv_ExtractAcquireMask(uint32_t intMask);
void     Cy_IPC_Drv_ClearInterrupt(IPC_INTR_STRUCT_Type *base,
				   uint32_t ipcReleaseMask, uint32_t ipcNotifyMask);

#ifdef __cplusplus
}
#endif

#endif /* MBOX_PSE84_FAKE_CY_IPC_DRV_H_ */
