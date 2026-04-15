/*
 * Copyright (c) 2026 PSE84 Voice Assistant contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ztest suite for drivers/mbox/mbox_pse84.c, wired against the fake
 * Cy_IPC_Drv_* shim in ../fake_cy_ipc. native_sim only.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <cy_ipc_drv.h>

/* native_sim software IRQ trigger — forward-declared to avoid depending
 * on the board-private irq_handler.h include path. */
extern void posix_sw_set_pending_IRQ(unsigned int IRQn);

#define MBOX_NODE   DT_NODELABEL(mbox_test)
#define MBOX_IRQ    DT_IRQN(MBOX_NODE)

#define TEST_CHAN_TX 0U   /* driver cell 0 -> PDL channel 8 */
#define TEST_CHAN_RX 1U   /* driver cell 1 -> PDL channel 9 */

#define PDL_CHAN_TX 8U
#define PDL_CHAN_RX 9U
#define INTR_IDX    0U

static const struct device *const mbox_dev = DEVICE_DT_GET(MBOX_NODE);

struct rx_record {
	mbox_channel_id_t cell;
	uint32_t value;
	unsigned int count;
};

static struct rx_record g_rx;

static void rx_callback(const struct device *dev, mbox_channel_id_t cell,
			void *user_data, struct mbox_msg *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	g_rx.cell = cell;
	g_rx.count++;
	if (data != NULL && data->data != NULL && data->size >= sizeof(uint32_t)) {
		memcpy(&g_rx.value, data->data, sizeof(uint32_t));
	}
}

static void setup(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_cy_ipc_reset();
	memset(&g_rx, 0, sizeof(g_rx));
}

ZTEST(mbox_pse84, test_device_is_ready)
{
	zassert_true(device_is_ready(mbox_dev), "mbox device not ready");
}

ZTEST(mbox_pse84, test_mtu_and_max_channels)
{
	zassert_equal(mbox_mtu_get(mbox_dev), (int)sizeof(uint32_t),
		      "MTU should be 4 (single u32 DATA register)");
	zassert_equal(mbox_max_channels_get(mbox_dev), 2U,
		      "test overlay declares two channels");
}

ZTEST(mbox_pse84, test_send_writes_expected_pdl_sequence)
{
	fake_cy_ipc_reset();

	uint32_t payload = 0xDEADBEEF;
	struct mbox_msg msg = {.data = &payload, .size = sizeof(payload)};

	int ret = mbox_send(mbox_dev, TEST_CHAN_TX, &msg);

	zassert_equal(ret, 0, "send returned %d", ret);
	zassert_equal(fake_cy_ipc_trace_len(), 3U,
		      "expected Acquire + Write + ReleaseNotify, got %zu calls",
		      fake_cy_ipc_trace_len());

	const struct fake_cy_ipc_call *c0 = fake_cy_ipc_trace_get(0);
	const struct fake_cy_ipc_call *c1 = fake_cy_ipc_trace_get(1);
	const struct fake_cy_ipc_call *c2 = fake_cy_ipc_trace_get(2);

	zassert_equal(c0->id, FAKE_CY_IPC_CALL_ACQUIRE_NOTIFY, "first call must be AcquireNotify");
	zassert_equal(c1->id, FAKE_CY_IPC_CALL_WRITE_DATA, "second call must be WriteDataValue");
	zassert_equal(c1->arg0, payload, "payload should be routed to DATA register verbatim");
	zassert_equal(c2->id, FAKE_CY_IPC_CALL_RELEASE_NOTIFY, "third call must be ReleaseNotify");
	zassert_equal(c2->arg0, 1U << INTR_IDX,
		      "release mask should target the intr-index we configured");
}

ZTEST(mbox_pse84, test_send_oversized_rejected)
{
	uint8_t big[8] = {0};
	struct mbox_msg msg = {.data = big, .size = sizeof(big)};

	zassert_equal(mbox_send(mbox_dev, TEST_CHAN_TX, &msg), -EMSGSIZE,
		      "messages > 4 bytes must be rejected");
}

ZTEST(mbox_pse84, test_send_invalid_channel)
{
	zassert_equal(mbox_send(mbox_dev, 7U, NULL), -EINVAL,
		      "send to out-of-range cell must return -EINVAL");
}

ZTEST(mbox_pse84, test_isr_dispatches_to_enabled_callback)
{
	/* Arrange */
	zassert_equal(mbox_register_callback(mbox_dev, TEST_CHAN_RX, rx_callback, NULL), 0);
	zassert_equal(mbox_set_enabled(mbox_dev, TEST_CHAN_RX, true), 0);

	fake_cy_ipc_reset();

	/* Act: pretend the peer released PDL channel 9 with value 0xC0FFEE. */
	const uint32_t payload = 0x00C0FFEEU;

	fake_cy_ipc_simulate_release(PDL_CHAN_RX, INTR_IDX, payload);
	/* Re-arm the release_mask the driver configured in init — fake_cy_ipc_reset()
	 * above cleared it. */
	IPC_INTR_STRUCT_Type *intr = Cy_IPC_Drv_GetIntrBaseAddr(INTR_IDX);
	Cy_IPC_Drv_SetInterruptMask(intr, (1U << PDL_CHAN_TX) | (1U << PDL_CHAN_RX), 0U);
	fake_cy_ipc_simulate_release(PDL_CHAN_RX, INTR_IDX, payload);

	posix_sw_set_pending_IRQ(MBOX_IRQ);
	/* Let the simulated NVIC dispatch the handler. */
	k_sleep(K_MSEC(1));

	/* Assert */
	zassert_equal(g_rx.count, 1U, "callback should fire exactly once");
	zassert_equal(g_rx.cell, TEST_CHAN_RX, "callback cell id wrong");
	zassert_equal(g_rx.value, payload, "callback got wrong DATA value");

	/* Cleanup for subsequent tests */
	(void)mbox_set_enabled(mbox_dev, TEST_CHAN_RX, false);
}

ZTEST(mbox_pse84, test_isr_disabled_channel_suppresses_callback)
{
	/* Callback is registered but the channel is *not* enabled. */
	zassert_equal(mbox_register_callback(mbox_dev, TEST_CHAN_RX, rx_callback, NULL), 0);
	/* ensure disabled */
	(void)mbox_set_enabled(mbox_dev, TEST_CHAN_RX, false);

	fake_cy_ipc_reset();

	IPC_INTR_STRUCT_Type *intr = Cy_IPC_Drv_GetIntrBaseAddr(INTR_IDX);

	Cy_IPC_Drv_SetInterruptMask(intr, (1U << PDL_CHAN_TX) | (1U << PDL_CHAN_RX), 0U);
	fake_cy_ipc_simulate_release(PDL_CHAN_RX, INTR_IDX, 0x11223344U);

	posix_sw_set_pending_IRQ(MBOX_IRQ);
	k_sleep(K_MSEC(1));

	zassert_equal(g_rx.count, 0U,
		      "callback must NOT fire on a disabled channel");
}

ZTEST(mbox_pse84, test_set_enabled_rejects_invalid_channel)
{
	zassert_equal(mbox_set_enabled(mbox_dev, 5U, true), -EINVAL);
}

ZTEST(mbox_pse84, test_set_enabled_idempotent)
{
	(void)mbox_set_enabled(mbox_dev, TEST_CHAN_TX, false);
	zassert_equal(mbox_set_enabled(mbox_dev, TEST_CHAN_TX, false), -EALREADY);
	zassert_equal(mbox_set_enabled(mbox_dev, TEST_CHAN_TX, true), 0);
	zassert_equal(mbox_set_enabled(mbox_dev, TEST_CHAN_TX, true), -EALREADY);
	(void)mbox_set_enabled(mbox_dev, TEST_CHAN_TX, false);
}

ZTEST_SUITE(mbox_pse84, NULL, NULL, setup, NULL, NULL);
