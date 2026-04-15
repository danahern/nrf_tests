/*
 * Copyright (c) 2026 PSE84 Voice Assistant contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake Cy_IPC_Drv_* implementation for ztest. See cy_ipc_drv.h.
 */

#include "cy_ipc_drv.h"

#include <stddef.h>
#include <string.h>

static struct fake_ipc_struct       g_ipc[FAKE_CY_IPC_CHANNELS];
static struct fake_ipc_intr_struct  g_intr[FAKE_CY_IPC_INTERRUPTS];
static struct fake_cy_ipc_call      g_trace[FAKE_CY_IPC_TRACE_DEPTH];
static size_t                       g_trace_len;

static void trace_push(enum fake_cy_ipc_call_id id, const void *base,
		       uint32_t a0, uint32_t a1)
{
	if (g_trace_len >= FAKE_CY_IPC_TRACE_DEPTH) {
		return;
	}
	g_trace[g_trace_len++] = (struct fake_cy_ipc_call){
		.id   = id,
		.base = (uintptr_t)base,
		.arg0 = a0,
		.arg1 = a1,
	};
}

void fake_cy_ipc_reset(void)
{
	memset(g_ipc, 0, sizeof(g_ipc));
	memset(g_intr, 0, sizeof(g_intr));
	g_trace_len = 0;
}

size_t fake_cy_ipc_trace_len(void)
{
	return g_trace_len;
}

const struct fake_cy_ipc_call *fake_cy_ipc_trace_get(size_t idx)
{
	if (idx >= g_trace_len) {
		return NULL;
	}
	return &g_trace[idx];
}

void fake_cy_ipc_simulate_release(uint32_t channel, uint32_t intr_index, uint32_t data_value)
{
	if (channel >= FAKE_CY_IPC_CHANNELS || intr_index >= FAKE_CY_IPC_INTERRUPTS) {
		return;
	}
	g_ipc[channel].data = data_value;
	g_intr[intr_index].pending_release |= 1U << channel;
}

IPC_STRUCT_Type *Cy_IPC_Drv_GetIpcBaseAddress(uint32_t ipcIndex)
{
	if (ipcIndex >= FAKE_CY_IPC_CHANNELS) {
		return NULL;
	}
	return &g_ipc[ipcIndex];
}

IPC_INTR_STRUCT_Type *Cy_IPC_Drv_GetIntrBaseAddr(uint32_t ipcIntrIndex)
{
	if (ipcIntrIndex >= FAKE_CY_IPC_INTERRUPTS) {
		return NULL;
	}
	return &g_intr[ipcIntrIndex];
}

void Cy_IPC_Drv_AcquireNotify(IPC_STRUCT_Type *base, uint32_t notifyEventIntr)
{
	trace_push(FAKE_CY_IPC_CALL_ACQUIRE_NOTIFY, base, notifyEventIntr, 0);
}

void Cy_IPC_Drv_ReleaseNotify(IPC_STRUCT_Type *base, uint32_t notifyEventIntr)
{
	trace_push(FAKE_CY_IPC_CALL_RELEASE_NOTIFY, base, notifyEventIntr, 0);
}

void Cy_IPC_Drv_WriteDataValue(IPC_STRUCT_Type *base, uint32_t dataValue)
{
	if (base != NULL) {
		base->data = dataValue;
	}
	trace_push(FAKE_CY_IPC_CALL_WRITE_DATA, base, dataValue, 0);
}

uint32_t Cy_IPC_Drv_ReadDataValue(IPC_STRUCT_Type const *base)
{
	uint32_t v = (base != NULL) ? base->data : 0U;

	trace_push(FAKE_CY_IPC_CALL_READ_DATA, base, v, 0);
	return v;
}

void Cy_IPC_Drv_SetInterruptMask(IPC_INTR_STRUCT_Type *base,
				 uint32_t ipcReleaseMask, uint32_t ipcNotifyMask)
{
	if (base != NULL) {
		base->release_mask = ipcReleaseMask;
	}
	trace_push(FAKE_CY_IPC_CALL_SET_INTERRUPT_MASK, base, ipcReleaseMask, ipcNotifyMask);
}

uint32_t Cy_IPC_Drv_GetInterruptStatusMasked(IPC_INTR_STRUCT_Type const *base)
{
	if (base == NULL) {
		return 0U;
	}
	/* Encode just the release mask into the low 16 bits. */
	return base->pending_release & base->release_mask;
}

uint32_t Cy_IPC_Drv_ExtractReleaseMask(uint32_t intMask)
{
	return intMask & 0xFFFFU;
}

uint32_t Cy_IPC_Drv_ExtractAcquireMask(uint32_t intMask)
{
	return (intMask >> 16U) & 0xFFFFU;
}

void Cy_IPC_Drv_ClearInterrupt(IPC_INTR_STRUCT_Type *base,
			       uint32_t ipcReleaseMask, uint32_t ipcNotifyMask)
{
	if (base != NULL) {
		base->pending_release &= ~ipcReleaseMask;
	}
	trace_push(FAKE_CY_IPC_CALL_CLEAR_INTERRUPT, base, ipcReleaseMask, ipcNotifyMask);
}
