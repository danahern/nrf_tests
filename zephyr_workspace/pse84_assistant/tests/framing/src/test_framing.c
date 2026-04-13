/*
 * Ztest suite for src/framing.c.
 *
 * Mirrors host/assistant_bridge/tests/test_framing.py one-for-one so
 * the wire format is verified from both ends. Keep them in sync: if a
 * Python test is added there, add the C equivalent here.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "framing.h"

/* --- Shared capture state for the streaming-parser callback ------------
 *
 * Tests push frames through frame_parser_feed; the callback appends
 * into this struct so assertions can run after feed() returns.
 */

#define CAP_MAX_FRAMES 8
#define CAP_MAX_PAYLOAD 64

struct captured {
	uint8_t type;
	uint8_t seq;
	uint16_t len;
	uint8_t payload[CAP_MAX_PAYLOAD];
};

struct capture {
	struct captured frames[CAP_MAX_FRAMES];
	int n;
};

static void capture_cb(uint8_t type, uint8_t seq,
		       const uint8_t *payload, uint16_t len, void *user)
{
	struct capture *c = user;

	zassert_true(c->n < CAP_MAX_FRAMES, "capture overflow");
	zassert_true(len <= CAP_MAX_PAYLOAD, "payload overflow");

	c->frames[c->n].type = type;
	c->frames[c->n].seq = seq;
	c->frames[c->n].len = len;
	if (len > 0) {
		memcpy(c->frames[c->n].payload, payload, len);
	}
	c->n++;
}

/* --- frame_encode / decode round-trip --------------------------------- */

ZTEST(framing, test_audio_frame_round_trip)
{
	const uint8_t payload[] = {
		0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44,
		0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44,
		0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44,
		0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44,
		0x11, 0x22, 0x33, 0x44, 0x11, 0x22, 0x33, 0x44,
	};
	uint8_t out[FRAME_HEADER_LEN + sizeof(payload)];

	int n = frame_encode(FRAME_TYPE_AUDIO, 7, payload, sizeof(payload),
			     out, sizeof(out));
	zassert_equal(n, (int)sizeof(out), "wrong frame length");
	zassert_equal(out[0], FRAME_TYPE_AUDIO, "wrong type byte");
	zassert_equal(out[1], 7, "wrong seq byte");
	/* u16 LE length */
	zassert_equal(out[2], sizeof(payload) & 0xFF, "low byte wrong");
	zassert_equal(out[3], (sizeof(payload) >> 8) & 0xFF, "high byte wrong");
	zassert_mem_equal(out + FRAME_HEADER_LEN, payload, sizeof(payload),
			  "payload mismatch");
}

ZTEST(framing, test_empty_payload)
{
	uint8_t out[FRAME_HEADER_LEN];
	int n = frame_encode(FRAME_TYPE_CTRL_START_LISTEN, 0, NULL, 0,
			     out, sizeof(out));
	zassert_equal(n, FRAME_HEADER_LEN, "header-only frame wrong length");
	zassert_equal(out[0], FRAME_TYPE_CTRL_START_LISTEN, "type");
	zassert_equal(out[2], 0, "len low");
	zassert_equal(out[3], 0, "len high");
}

ZTEST(framing, test_all_frame_types)
{
	const uint8_t types[] = {
		FRAME_TYPE_AUDIO,
		FRAME_TYPE_CTRL_STATE,
		FRAME_TYPE_CTRL_START_LISTEN,
		FRAME_TYPE_CTRL_STOP_LISTEN,
		FRAME_TYPE_TEXT_CHUNK,
		FRAME_TYPE_TEXT_END,
	};
	for (size_t i = 0; i < sizeof(types); i++) {
		uint8_t out[16];
		const uint8_t pl[] = "hello";
		int n = frame_encode(types[i], 3, pl, sizeof(pl) - 1,
				     out, sizeof(out));
		zassert_true(n > 0, "encode failed for type 0x%02x", types[i]);
		zassert_equal(out[0], types[i], "wrong type");
		zassert_true(frame_type_is_known(out[0]), "type not known");
	}
}

ZTEST(framing, test_seq_wraps_at_u8)
{
	uint8_t out[8];
	int n = frame_encode(FRAME_TYPE_AUDIO, 255, (const uint8_t *)"x", 1,
			     out, sizeof(out));
	zassert_equal(n, 5, "expected 5 bytes");
	zassert_equal(out[1], 255, "seq byte");
}

ZTEST(framing, test_encode_out_cap_too_small)
{
	uint8_t out[3]; /* smaller than header */
	int n = frame_encode(FRAME_TYPE_AUDIO, 0, NULL, 0, out, sizeof(out));
	zassert_equal(n, FRAME_ERR_OUT_CAP, "expected out-cap error");
}

ZTEST(framing, test_encode_null_output)
{
	int n = frame_encode(FRAME_TYPE_AUDIO, 0, NULL, 0, NULL, 0);
	zassert_equal(n, FRAME_ERR_INVALID_ARG, "expected invalid-arg");
}

ZTEST(framing, test_encode_null_payload_with_len)
{
	uint8_t out[16];
	int n = frame_encode(FRAME_TYPE_AUDIO, 0, NULL, 4, out, sizeof(out));
	zassert_equal(n, FRAME_ERR_INVALID_ARG, "expected invalid-arg");
}

/* --- streaming parser -------------------------------------------------- */

#define PARSER_CAP (FRAME_HEADER_LEN + 512)

struct test_fixture {
	frame_parser_t parser;
	uint8_t storage[PARSER_CAP];
	struct capture cap;
};

static struct test_fixture fx;

static void *framing_setup(void)
{
	return &fx;
}

static void framing_before(void *f)
{
	ARG_UNUSED(f);
	memset(&fx, 0, sizeof(fx));
	int rc = frame_parser_init(&fx.parser, fx.storage, sizeof(fx.storage));
	zassert_equal(rc, 0, "parser_init");
}

ZTEST(framing, test_single_complete_frame)
{
	uint8_t out[FRAME_HEADER_LEN + 3];
	frame_encode(FRAME_TYPE_AUDIO, 1, (const uint8_t *)"abc", 3,
		     out, sizeof(out));
	int delivered = frame_parser_feed(&fx.parser, out, sizeof(out),
					  capture_cb, &fx.cap);
	zassert_equal(delivered, 1, "one frame expected");
	zassert_equal(fx.cap.n, 1, "capture");
	zassert_equal(fx.cap.frames[0].type, FRAME_TYPE_AUDIO, "type");
	zassert_equal(fx.cap.frames[0].seq, 1, "seq");
	zassert_equal(fx.cap.frames[0].len, 3, "len");
	zassert_mem_equal(fx.cap.frames[0].payload, "abc", 3, "payload");
}

ZTEST(framing, test_multiple_frames_one_feed)
{
	uint8_t f1[FRAME_HEADER_LEN + 2];
	uint8_t f2[FRAME_HEADER_LEN + 2];
	frame_encode(FRAME_TYPE_AUDIO, 1, (const uint8_t *)"aa", 2,
		     f1, sizeof(f1));
	frame_encode(FRAME_TYPE_TEXT_CHUNK, 2, (const uint8_t *)"bb", 2,
		     f2, sizeof(f2));

	uint8_t buf[sizeof(f1) + sizeof(f2)];
	memcpy(buf, f1, sizeof(f1));
	memcpy(buf + sizeof(f1), f2, sizeof(f2));

	int delivered = frame_parser_feed(&fx.parser, buf, sizeof(buf),
					  capture_cb, &fx.cap);
	zassert_equal(delivered, 2, "two frames");
	zassert_equal(fx.cap.frames[0].type, FRAME_TYPE_AUDIO, "f1 type");
	zassert_equal(fx.cap.frames[1].type, FRAME_TYPE_TEXT_CHUNK, "f2 type");
}

ZTEST(framing, test_partial_header_buffered)
{
	uint8_t frame[FRAME_HEADER_LEN + 3];
	frame_encode(FRAME_TYPE_AUDIO, 1, (const uint8_t *)"xyz", 3,
		     frame, sizeof(frame));
	/* Feed one byte at a time — parser should accumulate silently. */
	for (size_t i = 0; i < sizeof(frame); i++) {
		int d = frame_parser_feed(&fx.parser, &frame[i], 1,
					  capture_cb, &fx.cap);
		if (i < sizeof(frame) - 1) {
			zassert_equal(d, 0, "premature delivery at i=%zu", i);
		} else {
			zassert_equal(d, 1, "final byte delivers");
		}
	}
	zassert_equal(fx.cap.n, 1, "one frame");
	zassert_mem_equal(fx.cap.frames[0].payload, "xyz", 3, "payload");
}

ZTEST(framing, test_frame_split_across_feeds)
{
	uint8_t frame[FRAME_HEADER_LEN + 11];
	frame_encode(FRAME_TYPE_AUDIO, 1, (const uint8_t *)"hello world", 11,
		     frame, sizeof(frame));
	/* Split mid-payload. */
	int d1 = frame_parser_feed(&fx.parser, frame, 6, capture_cb, &fx.cap);
	zassert_equal(d1, 0, "partial yields nothing");
	zassert_equal(fx.cap.n, 0, "capture still empty");
	int d2 = frame_parser_feed(&fx.parser, frame + 6,
				   sizeof(frame) - 6, capture_cb, &fx.cap);
	zassert_equal(d2, 1, "rest delivers");
	zassert_equal(fx.cap.n, 1, "one frame captured");
	zassert_mem_equal(fx.cap.frames[0].payload, "hello world", 11, "payload");
}

ZTEST(framing, test_trailing_partial_after_complete)
{
	uint8_t f1[FRAME_HEADER_LEN + 1];
	uint8_t f2[FRAME_HEADER_LEN + 6];
	frame_encode(FRAME_TYPE_AUDIO, 1, (const uint8_t *)"a", 1,
		     f1, sizeof(f1));
	frame_encode(FRAME_TYPE_TEXT_CHUNK, 2, (const uint8_t *)"second", 6,
		     f2, sizeof(f2));

	/* Full f1 + first 3 bytes of f2. */
	uint8_t chunk[sizeof(f1) + 3];
	memcpy(chunk, f1, sizeof(f1));
	memcpy(chunk + sizeof(f1), f2, 3);
	int d1 = frame_parser_feed(&fx.parser, chunk, sizeof(chunk),
				   capture_cb, &fx.cap);
	zassert_equal(d1, 1, "only f1 delivered");
	zassert_equal(fx.cap.n, 1, "one captured");

	int d2 = frame_parser_feed(&fx.parser, f2 + 3, sizeof(f2) - 3,
				   capture_cb, &fx.cap);
	zassert_equal(d2, 1, "rest of f2 delivers");
	zassert_equal(fx.cap.n, 2, "two captured");
	zassert_equal(fx.cap.frames[1].type, FRAME_TYPE_TEXT_CHUNK, "f2 type");
	zassert_mem_equal(fx.cap.frames[1].payload, "second", 6, "f2 payload");
}

ZTEST(framing, test_unknown_type_surfaces_error)
{
	uint8_t bad[] = {0xEE, 0x00, 0x00, 0x00};
	int rc = frame_parser_feed(&fx.parser, bad, sizeof(bad),
				   capture_cb, &fx.cap);
	zassert_equal(rc, FRAME_ERR_UNKNOWN_TYPE, "expected unknown-type");
	zassert_equal(fx.cap.n, 0, "no frame delivered");

	/* After the error one byte has been consumed — the next 3 bytes
	 * still look like part of a possibly-valid frame to the parser.
	 * Feed a full valid frame on top and expect it to be picked up
	 * once the lingering bytes drain.
	 */
	frame_parser_reset(&fx.parser);
	uint8_t good[FRAME_HEADER_LEN + 2];
	frame_encode(FRAME_TYPE_AUDIO, 9, (const uint8_t *)"ok", 2,
		     good, sizeof(good));
	int d = frame_parser_feed(&fx.parser, good, sizeof(good),
				  capture_cb, &fx.cap);
	zassert_equal(d, 1, "good frame after reset");
}

ZTEST(framing, test_empty_feed_yields_nothing)
{
	int d = frame_parser_feed(&fx.parser, NULL, 0, capture_cb, &fx.cap);
	zassert_equal(d, 0, "empty feed");
	zassert_equal(fx.cap.n, 0, "capture empty");
}

ZTEST(framing, test_parser_init_rejects_small_storage)
{
	frame_parser_t p;
	uint8_t buf[2]; /* smaller than FRAME_HEADER_LEN */
	int rc = frame_parser_init(&p, buf, sizeof(buf));
	zassert_equal(rc, FRAME_ERR_INVALID_ARG, "too-small storage");
}

ZTEST(framing, test_parser_frame_larger_than_cap)
{
	frame_parser_t p;
	uint8_t buf[FRAME_HEADER_LEN + 4];
	zassert_equal(frame_parser_init(&p, buf, sizeof(buf)), 0, "init");

	/* Craft a header claiming a 1000-byte payload — bigger than cap. */
	uint8_t header[FRAME_HEADER_LEN] = {
		FRAME_TYPE_AUDIO, 0x00, 0xE8, 0x03, /* 1000 LE */
	};
	int rc = frame_parser_feed(&p, header, sizeof(header),
				   capture_cb, &fx.cap);
	zassert_equal(rc, FRAME_ERR_OVERFLOW, "expected overflow");
}

ZTEST(framing, test_parser_rejects_null_buf_with_len)
{
	int rc = frame_parser_feed(&fx.parser, NULL, 4, capture_cb, &fx.cap);
	zassert_equal(rc, FRAME_ERR_INVALID_ARG, "null buf with len>0");
}

/* --- known-type helper matches Track B's _KNOWN_TYPES ----------------- */

ZTEST(framing, test_known_types)
{
	zassert_true(frame_type_is_known(FRAME_TYPE_AUDIO), "AUDIO");
	zassert_true(frame_type_is_known(FRAME_TYPE_CTRL_STATE), "CTRL_STATE");
	zassert_true(frame_type_is_known(FRAME_TYPE_CTRL_START_LISTEN), "START");
	zassert_true(frame_type_is_known(FRAME_TYPE_CTRL_STOP_LISTEN), "STOP");
	zassert_true(frame_type_is_known(FRAME_TYPE_TEXT_CHUNK), "TEXT_CHUNK");
	zassert_true(frame_type_is_known(FRAME_TYPE_TEXT_END), "TEXT_END");
	zassert_false(frame_type_is_known(0x00), "0x00 not known");
	zassert_false(frame_type_is_known(0xFF), "0xFF not known");
	zassert_false(frame_type_is_known(0x02), "0x02 reserved (TTS)");
}

ZTEST_SUITE(framing, NULL, framing_setup, framing_before, NULL, NULL);
