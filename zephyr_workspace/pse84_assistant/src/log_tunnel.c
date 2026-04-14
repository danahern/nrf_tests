/*
 * PSE84 Voice Assistant — M55 log backend forwarding to M33 via
 * ipc_service. See log_tunnel.h for the contract.
 */
#include "log_tunnel.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/ipc/ipc_service.h>
#include <string.h>

#define LOG_TUNNEL_BUF_SIZE 256

static struct ipc_ept *tunnel_ep;

/* Simple line buffer + output formatter using Zephyr's log_output
 * helper. On every LOG_INF/LOG_WRN/LOG_ERR the formatter calls
 * log_tunnel_char_out() once per character; we batch into the buffer
 * and flush via ipc_service_send at newline / buffer-full.
 */
static uint8_t log_tunnel_outbuf[LOG_TUNNEL_BUF_SIZE];

static int log_tunnel_char_out(uint8_t *data, size_t length, void *ctx);

LOG_OUTPUT_DEFINE(log_tunnel_output, log_tunnel_char_out,
		  log_tunnel_outbuf, sizeof(log_tunnel_outbuf));

/* Queue of bytes pending on the ipc_service endpoint. We don't hold
 * the log-output state across an async send — instead we just try
 * ipc_service_send inline. If it fails (no peer bound / short
 * shared-mem) we drop, which is fine for observability logs.
 */
static int log_tunnel_char_out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);
	if (tunnel_ep != NULL) {
		(void)ipc_service_send(tunnel_ep, data, length);
	}
	return (int)length;
}

static uint32_t log_format_current = LOG_OUTPUT_TEXT;

static void process(const struct log_backend *const backend,
		    union log_msg_generic *msg)
{
	ARG_UNUSED(backend);
	uint32_t flags = LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP |
			 LOG_OUTPUT_FLAG_FORMAT_TIMESTAMP;

	log_format_func_t out = log_format_func_t_get(log_format_current);

	out(&log_tunnel_output, &msg->log, flags);
}

static void init_backend(struct log_backend const *const backend)
{
	ARG_UNUSED(backend);
}

static void panic(struct log_backend const *const backend)
{
	ARG_UNUSED(backend);
	log_backend_std_panic(&log_tunnel_output);
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);
	log_backend_std_dropped(&log_tunnel_output, cnt);
}

static int format_set(const struct log_backend *const backend, uint32_t type)
{
	ARG_UNUSED(backend);
	log_format_current = type;
	return 0;
}

static const struct log_backend_api log_tunnel_api = {
	.process = process,
	.format_set = format_set,
	.panic = panic,
	.init = init_backend,
	.dropped = dropped,
};

LOG_BACKEND_DEFINE(log_tunnel_backend, log_tunnel_api, true);

void log_tunnel_attach_endpoint(struct ipc_ept *ep)
{
	tunnel_ep = ep;
}
