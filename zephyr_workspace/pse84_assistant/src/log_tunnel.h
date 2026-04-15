/*
 * PSE84 Voice Assistant — M55 log tunnel (Phase 0b.8).
 *
 * Replaces the default CONFIG_LOG_BACKEND_UART with a custom backend
 * that forwards every LOG_INF/LOG_WRN/LOG_ERR line over the
 * 'assistant' ipc_service endpoint to the M33 companion. The M33
 * main() registers the matching peer endpoint and writes received
 * bytes to its own printk (which owns the real uart2 console).
 *
 * Net effect: no more UART contention between the two cores — M33
 * is the single writer on uart2, M55 is pure ipc_service client for
 * log output. Direct printk() on M55 (e.g. audio.c's PCM hex dump)
 * still goes to UART via the default handler, so Phase 2 audio
 * capture continues to work unchanged; the dual-owner window there
 * is short enough that interleave is rare and tolerable.
 *
 * Must be called AFTER ipc_service has opened the 'assistant'
 * endpoint. Installation happens once in main(); no teardown.
 */
#ifndef PSE84_ASSISTANT_LOG_TUNNEL_H_
#define PSE84_ASSISTANT_LOG_TUNNEL_H_

#include <stdbool.h>

struct ipc_ept;

/* Hand the tunnel the open 'assistant' endpoint so it can
 * ipc_service_send on every log line. Safe to call before the M33
 * peer binds — queued bytes before bind are simply dropped.
 */
void log_tunnel_attach_endpoint(struct ipc_ept *ep);

/* Toggle whether log_tunnel forwards bytes via ipc_service_send.
 * Wire to the M33 endpoint .bound callback so the LOG thread doesn't
 * block on icmsg send before the peer is up. */
void log_tunnel_set_peer_bound(bool bound);

#endif /* PSE84_ASSISTANT_LOG_TUNNEL_H_ */
