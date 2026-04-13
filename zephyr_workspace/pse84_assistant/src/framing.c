/*
 * L2CAP framing codec — see framing.h.
 *
 * Pure C, no Zephyr kernel dependencies so the same translation unit
 * can be linked into ztest suites on native_sim and (eventually) a
 * host-side fuzz harness against Track B's Python codec.
 */

#include "framing.h"

#include <string.h>

bool frame_type_is_known(uint8_t type)
{
	switch (type) {
	case FRAME_TYPE_AUDIO:
	case FRAME_TYPE_CTRL_STATE:
	case FRAME_TYPE_CTRL_START_LISTEN:
	case FRAME_TYPE_CTRL_STOP_LISTEN:
	case FRAME_TYPE_TEXT_CHUNK:
	case FRAME_TYPE_TEXT_END:
		return true;
	default:
		return false;
	}
}

int frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload,
		 uint16_t len, uint8_t *out, int out_cap)
{
	if (out == NULL) {
		return FRAME_ERR_INVALID_ARG;
	}
	if (len > 0 && payload == NULL) {
		return FRAME_ERR_INVALID_ARG;
	}
	/* u16 len is always in-range by type; keep the macro comparison for
	 * symmetry with Track B (which raises if > 0xFFFF).
	 */
	if ((unsigned)len > FRAME_MAX_PAYLOAD_LEN) {
		return FRAME_ERR_PAYLOAD_RANGE;
	}
	const int total = FRAME_HEADER_LEN + (int)len;
	if (out_cap < total) {
		return FRAME_ERR_OUT_CAP;
	}

	out[0] = type;
	out[1] = seq;
	out[2] = (uint8_t)(len & 0xFF);
	out[3] = (uint8_t)((len >> 8) & 0xFF);
	if (len > 0) {
		memmove(out + FRAME_HEADER_LEN, payload, len);
	}
	return total;
}

int frame_parser_init(frame_parser_t *p, uint8_t *storage, int cap)
{
	if (p == NULL || storage == NULL || cap < FRAME_HEADER_LEN) {
		return FRAME_ERR_INVALID_ARG;
	}
	p->buf = storage;
	p->cap = cap;
	p->len = 0;
	return 0;
}

void frame_parser_reset(frame_parser_t *p)
{
	if (p != NULL) {
		p->len = 0;
	}
}

/* Pop @p n bytes from the front of p->buf. */
static void parser_consume(frame_parser_t *p, int n)
{
	if (n <= 0) {
		return;
	}
	if (n >= p->len) {
		p->len = 0;
		return;
	}
	memmove(p->buf, p->buf + n, (size_t)(p->len - n));
	p->len -= n;
}

int frame_parser_feed(frame_parser_t *p, const uint8_t *buf, int len,
		      frame_cb_t cb, void *user)
{
	if (p == NULL || cb == NULL || len < 0) {
		return FRAME_ERR_INVALID_ARG;
	}
	if (len > 0 && buf == NULL) {
		return FRAME_ERR_INVALID_ARG;
	}

	int delivered = 0;
	int in_off = 0;

	while (in_off < len || p->len >= FRAME_HEADER_LEN) {
		/* Pull input into the working buffer in chunks sized to the
		 * next frame. Bounds-check before copying. We need at least
		 * FRAME_HEADER_LEN bytes in p->buf to know the payload size.
		 */
		if (p->len < FRAME_HEADER_LEN) {
			int need = FRAME_HEADER_LEN - p->len;
			int avail = len - in_off;
			int take = (avail < need) ? avail : need;
			if (take == 0) {
				break;
			}
			if (p->len + take > p->cap) {
				/* Header alone shouldn't exhaust cap per
				 * the init contract, but guard anyway.
				 */
				return FRAME_ERR_OVERFLOW;
			}
			memcpy(p->buf + p->len, buf + in_off, (size_t)take);
			p->len += take;
			in_off += take;
			if (p->len < FRAME_HEADER_LEN) {
				break;
			}
		}

		/* Parse header. */
		const uint8_t  type = p->buf[0];
		const uint8_t  seq  = p->buf[1];
		const uint16_t plen = (uint16_t)(p->buf[2] | ((uint16_t)p->buf[3] << 8));
		const int total = FRAME_HEADER_LEN + (int)plen;

		if (total > p->cap) {
			/* Frame can never fit — caller-provided storage is
			 * too small for this payload size. Drop the frame
			 * boundary (consume header) so the caller sees the
			 * error once instead of looping forever.
			 */
			parser_consume(p, FRAME_HEADER_LEN);
			return FRAME_ERR_OVERFLOW;
		}

		/* Early type check — Track B's streaming parser does the
		 * known-type check before committing the payload so that an
		 * unknown type byte only costs one byte of resync, not the
		 * whole declared payload.
		 */
		if (!frame_type_is_known(type)) {
			/* Drop the offending byte and surface the error. */
			parser_consume(p, 1);
			return FRAME_ERR_UNKNOWN_TYPE;
		}

		/* Fill payload. */
		if (p->len < total) {
			int need = total - p->len;
			int avail = len - in_off;
			int take = (avail < need) ? avail : need;
			if (take == 0) {
				break;
			}
			memcpy(p->buf + p->len, buf + in_off, (size_t)take);
			p->len += take;
			in_off += take;
			if (p->len < total) {
				break;
			}
		}

		/* Deliver. */
		cb(type, seq, (plen > 0) ? (p->buf + FRAME_HEADER_LEN) : NULL,
		   plen, user);
		delivered++;
		parser_consume(p, total);
	}

	return delivered;
}
