/* Helpers for tests that read entries through a stream: a one-entry archive and a read loop. */
#ifndef BARTAP_TEST_STREAM_FIXTURE_H
#define BARTAP_TEST_STREAM_FIXTURE_H

#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "internal.h"

/* Deterministic, so a failure reproduces. */
static inline uint32_t fixture_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline void *fixture_alloc(size_t size)
{
    void *p = malloc(size != 0 ? size : 1u);
    if (p == NULL) {
        abort();
    }
    return p;
}

enum {
    FIXTURE_LOCAL_OFFSET = 7,  /* the entry does not start at 0 */
    FIXTURE_NAME_LEN = 5,
    FIXTURE_EXTRA_LEN = 300,   /* above 255, so the length's high byte matters */
    FIXTURE_DATA_OFFSET = FIXTURE_LOCAL_OFFSET + 30 + FIXTURE_NAME_LEN + FIXTURE_EXTRA_LEN,
    FIXTURE_TRAILER = 16       /* stands in for what follows an entry in a real archive */
};

typedef struct {
    uint8_t *bytes;
    bartap_source_t source;
    bartap_entry_t entry;
} fixture_t;

/* Written byte by byte, not with bt_put_*, so the test does not share code with what it checks. */
static inline void fixture_build(fixture_t *f, const uint8_t *data, uint32_t data_len,
                                 uint16_t method, uint32_t uncompressed_size, uint32_t crc)
{
    uint32_t size = FIXTURE_DATA_OFFSET + data_len + FIXTURE_TRAILER;
    uint8_t *local;

    f->bytes = fixture_alloc(size);
    memset(f->bytes, 0x5A, size);
    local = f->bytes + FIXTURE_LOCAL_OFFSET;
    local[0] = 0x50;
    local[1] = 0x4B;
    local[2] = 0x03;
    local[3] = 0x04;
    local[26] = FIXTURE_NAME_LEN & 0xFF;
    local[27] = FIXTURE_NAME_LEN >> 8;
    local[28] = FIXTURE_EXTRA_LEN & 0xFF;
    local[29] = FIXTURE_EXTRA_LEN >> 8;
    if (data_len != 0) {
        memcpy(f->bytes + FIXTURE_DATA_OFFSET, data, data_len);
    }
    CHECK(bartap_source_memory(&f->source, f->bytes, size) == BARTAP_OK);

    memset(&f->entry, 0, sizeof f->entry);
    f->entry.local_offset = FIXTURE_LOCAL_OFFSET;
    f->entry.compressed_size = data_len;
    f->entry.uncompressed_size = uncompressed_size;
    f->entry.crc32 = crc;
    f->entry.method = method;
}

static inline void fixture_free(fixture_t *f)
{
    free(f->bytes);
}

/* Everything re-readable now must match what was delivered, and the edges must be refused. */
static inline void fixture_check_reread(const bartap_stream_t *stream, const uint8_t *out,
                                        uint32_t total, uint32_t *rng)
{
    uint8_t copy[512];
    uint32_t oldest = stream->window_write - stream->window_valid;

    CHECK(stream->delivered == total);
    CHECK(bt_stream_reread(stream, total, copy, 1) == BARTAP_ERR_ARG);
    CHECK(bt_stream_reread(stream, UINT32_MAX, copy, 2) == BARTAP_ERR_ARG);
    if (oldest > 0 && oldest <= total) {
        CHECK(bt_stream_reread(stream, oldest - 1u, copy, 1) == BARTAP_END);
    }
    if (oldest < total) {
        uint32_t offset = oldest + fixture_random(rng) % (total - oldest);
        uint32_t len = fixture_random(rng) % (total - offset + 1u);
        if (len > sizeof copy) {
            len = sizeof copy;
        }
        CHECK(bt_stream_reread(stream, offset, copy, len) == BARTAP_OK);
        CHECK(len == 0 || memcmp(copy, out + offset, len) == 0);
    }
}

/* Reads to the stream's end or failure in calls of varying sizes, and returns that result. */
static inline bartap_err_t fixture_drain(bartap_stream_t *stream, uint8_t *out, uint32_t out_size,
                                         uint32_t *total, uint32_t *rng, bool check_reread)
{
    *total = 0;
    for (;;) {
        uint32_t r = fixture_random(rng);
        uint32_t cap = (r & 1u) != 0 ? BARTAP_WINDOW_SIZE : 1u + (r >> 1) % 5000u;
        uint32_t got = UINT32_MAX;
        uint8_t spare;

        if (cap > out_size - *total) {
            cap = out_size - *total;
        }
        /* With out full, one byte is still asked for: the stream must not have one to give. */
        bartap_err_t err = cap != 0 ? bt_stream_read(stream, out + *total, cap, &got)
                                    : bt_stream_read(stream, &spare, 1, &got);
        if (err != BARTAP_OK) {
            CHECK(got == 0);
            return err;
        }
        CHECK(got > 0);
        if (cap == 0) {
            return BARTAP_ERR_BUFFER_TOO_SMALL;
        }
        CHECK(got <= cap);
        *total += got;
        if (check_reread) {
            fixture_check_reread(stream, out, *total, rng);
        }
    }
}

#endif /* BARTAP_TEST_STREAM_FIXTURE_H */
