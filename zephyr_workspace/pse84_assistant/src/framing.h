/*
 * L2CAP CoC framing codec for the PSE84 voice assistant.
 *
 * Wire format (little-endian, 4-byte header + payload):
 *
 *     | u8 type | u8 seq | u16 len | payload[len] |
 *
 * Byte-for-byte compatible with host/assistant_bridge/framing.py (Track B).
 * Both ends speak this framing; the M33 BLE shim does NOT interpret
 * payloads — it is a dumb L2CAP <-> ipc_service pump.
 */

#ifndef PSE84_ASSISTANT_FRAMING_H_
#define PSE84_ASSISTANT_FRAMING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_HEADER_LEN 4
#define FRAME_MAX_PAYLOAD_LEN 0xFFFFu /* u16 wire length */

enum frame_type {
	FRAME_TYPE_AUDIO             = 0x01, /* Opus 20 ms frame. */
	FRAME_TYPE_CTRL_STATE        = 0x10, /* u8 state (IDLE/LISTEN/THINK/RESPOND). */
	FRAME_TYPE_CTRL_START_LISTEN = 0x11,
	FRAME_TYPE_CTRL_STOP_LISTEN  = 0x12,
	FRAME_TYPE_TEXT_CHUNK        = 0x20, /* UTF-8, partial. */
	FRAME_TYPE_TEXT_END          = 0x21, /* End-of-response sentinel. */
};

/* Error codes. Negative so they don't collide with byte counts returned
 * from successful encode calls.
 */
#define FRAME_ERR_INVALID_ARG   (-1)
#define FRAME_ERR_PAYLOAD_RANGE (-2) /* payload > FRAME_MAX_PAYLOAD_LEN */
#define FRAME_ERR_OUT_CAP       (-3) /* output buffer too small */
#define FRAME_ERR_UNKNOWN_TYPE  (-4) /* streaming parser saw a bad type byte */
#define FRAME_ERR_OVERFLOW      (-5) /* parser buffer capacity exceeded */

/**
 * Encode one frame into a flat byte buffer.
 *
 * @param type      Frame type (any u8 value; well-known values in
 *                  enum frame_type).
 * @param seq       Sequence number (u8). Wraps at 255.
 * @param payload   Source payload, may be NULL iff @p len == 0.
 * @param len       Payload length in bytes. Must be <= FRAME_MAX_PAYLOAD_LEN.
 * @param out       Output buffer.
 * @param out_cap   Capacity of @p out in bytes. Must be >= FRAME_HEADER_LEN + len.
 * @return bytes written (== FRAME_HEADER_LEN + len) on success, negative
 *         FRAME_ERR_* on failure.
 */
int frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload,
		 uint16_t len, uint8_t *out, int out_cap);

/* --- Streaming parser --------------------------------------------------
 *
 * BLE SDUs do not guarantee per-frame alignment: a single SDU may carry
 * a partial frame, multiple frames, or a frame split across multiple
 * SDUs. The streaming parser buffers the in-flight frame inside a
 * caller-owned scratch buffer and emits complete frames via a callback.
 *
 * Mirrors Track B's StreamingFrameParser.feed() semantics:
 *  - partial inputs buffered silently;
 *  - full frames yielded in order;
 *  - unknown frame types surface as FRAME_ERR_UNKNOWN_TYPE and the
 *    offending byte is dropped so the caller can resync.
 */

typedef void (*frame_cb_t)(uint8_t type, uint8_t seq,
			   const uint8_t *payload, uint16_t len,
			   void *user);

struct frame_parser {
	uint8_t *buf;
	int cap;        /* capacity in bytes */
	int len;        /* bytes currently buffered */
};

typedef struct frame_parser frame_parser_t;

/**
 * Initialise a parser with a caller-owned scratch buffer.
 *
 * @param p        Parser to initialise.
 * @param storage  Backing storage. Must outlive @p p.
 * @param cap      Capacity of @p storage in bytes. Must be at least
 *                 FRAME_HEADER_LEN + FRAME_MAX_PAYLOAD_LEN to guarantee
 *                 any legal frame can be buffered; smaller capacities
 *                 work if the application constrains itself to smaller
 *                 payloads.
 * @return 0 on success, FRAME_ERR_INVALID_ARG on bad args.
 */
int frame_parser_init(frame_parser_t *p, uint8_t *storage, int cap);

/**
 * Feed bytes to the parser. Complete frames are delivered via @p cb.
 *
 * @param p       Initialised parser.
 * @param buf     Incoming bytes (may be NULL iff @p len == 0).
 * @param len     Byte count.
 * @param cb      Frame callback; invoked once per complete frame in
 *                order. The payload pointer is valid only for the
 *                duration of the callback; copy if you need it.
 * @param user    Opaque cookie passed back to @p cb.
 * @return number of complete frames delivered (>=0) on success,
 *         negative FRAME_ERR_* on failure. On FRAME_ERR_UNKNOWN_TYPE
 *         the parser stays usable — one byte of the offending frame is
 *         consumed so the caller may retry / resync.
 */
int frame_parser_feed(frame_parser_t *p, const uint8_t *buf, int len,
		      frame_cb_t cb, void *user);

/**
 * Drop any buffered partial frame. Useful after a disconnect.
 */
void frame_parser_reset(frame_parser_t *p);

/**
 * True iff @p type is one of the well-known frame types.
 * Matches Track B's _KNOWN_TYPES membership test.
 */
bool frame_type_is_known(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif /* PSE84_ASSISTANT_FRAMING_H_ */
