/* Rolling buffer and byte source (R11, R15, R48): ring wrap-around and edges, memory mode and its
 * virtual window, and stream mode's three paths (replay from the ring or the inflate window, live
 * reads, skipping forward after a seek), with rewinds when a target has left both. */
#include "stream_fixture.h"

#define METHOD_STORED 0u
#define DATA_MAX 200000u
#define RING_BIG 65536u  /* reaches further back than the inflate window */
#define RING_SMALL 4096u /* the inflate window reaches further back than this */

static uint8_t window[BARTAP_WINDOW_SIZE];
static uint8_t ring_big[RING_BIG];
static uint8_t ring_small[RING_SMALL];
static bartap_stream_t stream;
static bartap_rolling_t rolling;
static bartap_bytes_t bytes;
static uint8_t data[DATA_MAX];
static uint8_t out[DATA_MAX];
static uint32_t rng = 0x13579BDFu;

static uint32_t make_data(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)fixture_random(&rng);
    }
    return bt_crc32(0, data, len);
}

/* ---- Card reads, counted, so a replay can be shown not to touch the card (R48) ---- */

static const uint8_t *card_bytes;
static uint32_t card_reads;

static int counting_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)user;
    card_reads++;
    memcpy(buf, card_bytes + offset, len);
    return 0;
}

/* A stored entry of data[0..len), read through counting_read. */
static void open_entry(fixture_t *f, uint32_t len)
{
    uint32_t crc = make_data(len);

    fixture_build(f, data, len, METHOD_STORED, len, crc);
    card_bytes = f->bytes;
    f->source.read = counting_read;
    f->source.user = NULL;
    CHECK(bt_stream_open(&stream, &f->source, &f->entry, window) == BARTAP_OK);
}

/* One read, checked against data at the model's position, which it then advances. Returns whether
 * it moved, so a loop over it stops on a failure instead of spinning on it. */
static bool read_and_check(uint32_t *pos, uint32_t len, uint32_t cap)
{
    uint32_t got = UINT32_MAX;
    bartap_err_t err = bt_bytes_read(&bytes, out, cap, &got);

    if (*pos == len) {
        CHECK(err == BARTAP_END);
        CHECK(got == 0);
        return false;
    }
    CHECK(err == BARTAP_OK);
    CHECK(got > 0 && got <= cap && got <= len - *pos);
    if (err != BARTAP_OK || got == 0 || got > cap || got > len - *pos) {
        return false;
    }
    CHECK(memcmp(out, data + *pos, got) == 0);
    *pos += got;
    return true;
}

/* Reads everything from the model's position to the end. */
static void read_to_end(uint32_t *pos, uint32_t len)
{
    while (*pos < len) {
        if (!read_and_check(pos, len, 1u + fixture_random(&rng) % 5000u)) {
            return; /* a failed read; read_and_check has already reported it */
        }
    }
    read_and_check(pos, len, 1);
}

/* ---- Rolling buffer ---- */

static void test_rolling_init(void)
{
    static const uint32_t bad[] = {0, 3, 6, 100, 4095, 0x80000001u};
    static const uint32_t good[] = {1, 2, 8, 4096, 0x80000000u};

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(bartap_rolling_init(&rolling, ring_small, bad[i]) == BARTAP_ERR_ARG);
        CHECK(bartap_rolling_init(&rolling, NULL, bad[i]) == BARTAP_ERR_ARG);
    }
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        CHECK(bartap_rolling_init(&rolling, NULL, good[i]) == BARTAP_OK);
        CHECK(rolling.delivered == 0);
        CHECK(!rolling.checkpoints[0].valid && !rolling.checkpoints[1].valid);
    }
}

/* Appends of every kind, including longer than the ring, then every offset read back. */
static void test_ring_contents(void)
{
    static const uint32_t sizes[] = {1, 8, 64};

    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t size = sizes[s];
        uint32_t at = 0;
        uint8_t copy[64];

        make_data(2000);
        CHECK(bartap_rolling_init(&rolling, ring_small, size) == BARTAP_OK);
        while (at < 1800u) {
            uint32_t len = fixture_random(&rng) % (3u * size + 2u);
            bt_ring_append(&rolling, data + at, len);
            at += len;
            CHECK(rolling.delivered == at);

            for (uint32_t offset = at > 200u ? at - 200u : 0; offset <= at; offset++) {
                uint32_t n = at - offset < sizeof copy ? at - offset : (uint32_t)sizeof copy;
                bartap_err_t err = bt_ring_read(&rolling, offset, copy, n);
                if (at - offset <= size) {
                    CHECK(err == BARTAP_OK);
                    CHECK(n == 0 || memcmp(copy, data + offset, n) == 0);
                } else {
                    CHECK(err == BARTAP_END);
                }
                /* len 0 only asks whether the offset is held, and takes a NULL buf. */
                CHECK(bt_ring_read(&rolling, offset, NULL, 0) ==
                      (at - offset <= size ? BARTAP_OK : BARTAP_END));
            }
            /* Not delivered yet. */
            CHECK(bt_ring_read(&rolling, at + 1u, copy, 0) == BARTAP_ERR_ARG);
            CHECK(bt_ring_read(&rolling, at, copy, 1) == BARTAP_ERR_ARG);
            CHECK(bt_ring_read(&rolling, UINT32_MAX, copy, 2) == BARTAP_ERR_ARG);
        }
    }
}

/* Memory mode's virtual window counts offsets but stores nothing. */
static void test_ring_virtual(void)
{
    uint8_t byte;

    CHECK(bartap_rolling_init(&rolling, NULL, 16) == BARTAP_OK);
    bt_ring_append(&rolling, data, 40);
    CHECK(rolling.delivered == 40);
    CHECK(bt_ring_read(&rolling, 39, &byte, 1) == BARTAP_END);
    CHECK(bt_ring_read(&rolling, 41, &byte, 0) == BARTAP_ERR_ARG);
}

/* ---- Memory mode (R11) ---- */

static void test_memory(void)
{
    static const uint32_t lens[] = {0, 1, 1000};

    for (size_t l = 0; l < sizeof lens / sizeof lens[0]; l++) {
        uint32_t len = lens[l];
        uint32_t pos = 0;
        uint32_t got;

        make_data(len);
        CHECK(bartap_rolling_init(&rolling, NULL, 64) == BARTAP_OK);
        rolling.delivered = 999; /* left over from another source: init must clear it */
        bt_bytes_init_memory(&bytes, data, len, &rolling);
        CHECK(rolling.delivered == 0);
        CHECK(bt_bytes_read(&bytes, out, 0, &got) == BARTAP_ERR_ARG);

        read_to_end(&pos, len);
        CHECK(rolling.delivered == len);
        read_and_check(&pos, len, 7); /* the end repeats */

        /* Seeking back and re-reading doesn't count the bytes again. */
        if (len > 0) {
            pos = len / 2u;
            CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
            read_to_end(&pos, len);
            CHECK(rolling.delivered == len);
        }
        CHECK(bt_bytes_seek(&bytes, len + 1u) == BARTAP_ERR_ARG);
        CHECK(bt_bytes_seek(&bytes, len) == BARTAP_OK);
        pos = len;
        read_and_check(&pos, len, 1);
    }

    /* A forward seek past bytes never read: the count still covers them, gap included. */
    uint32_t pos = 0;
    make_data(1000);
    CHECK(bartap_rolling_init(&rolling, NULL, 64) == BARTAP_OK);
    bt_bytes_init_memory(&bytes, data, 1000, &rolling);
    read_and_check(&pos, 1000, 10);
    pos = 500;
    CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
    read_and_check(&pos, 1000, 10);
    CHECK(rolling.delivered == 510);

    /* No rolling buffer at all. */
    pos = 0;
    bt_bytes_init_memory(&bytes, data, 1000, NULL);
    read_to_end(&pos, 1000);
}

/* ---- Stream mode (R15, R48) ---- */

/* A seek back that the ring covers but the inflate window doesn't: no card reads to catch up. */
static void test_replay_from_ring(void)
{
    fixture_t f;
    uint32_t pos = 0;

    open_entry(&f, DATA_MAX);
    CHECK(bartap_rolling_init(&rolling, ring_big, RING_BIG) == BARTAP_OK);
    bt_bytes_init_stream(&bytes, &stream, &rolling);
    while (pos < 150000u && read_and_check(&pos, DATA_MAX, 4000)) {
    }
    uint32_t live = stream.delivered;
    uint32_t target = live - 50000u;
    CHECK(bt_stream_reread(&stream, target, NULL, 0) == BARTAP_END); /* only the ring has it */

    uint32_t reads = card_reads;
    pos = target;
    CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
    while (pos < live && read_and_check(&pos, DATA_MAX, 3000)) {
    }
    CHECK(pos == live);
    CHECK(card_reads == reads);
    CHECK(stream.delivered == live && rolling.delivered == live);

    /* Past the ring's newest byte, reading carries on from the live stream (R48 invariant 3). */
    read_to_end(&pos, DATA_MAX);
    CHECK(card_reads > reads);
    CHECK(rolling.delivered == DATA_MAX);
    fixture_free(&f);
}

/* A seek back that only the inflate window covers, with a small ring and with none. */
static void test_replay_from_window(void)
{
    for (int with_ring = 0; with_ring < 2; with_ring++) {
        fixture_t f;
        uint32_t pos = 0;
        bartap_rolling_t *r = NULL;

        open_entry(&f, DATA_MAX);
        if (with_ring) {
            CHECK(bartap_rolling_init(&rolling, ring_small, RING_SMALL) == BARTAP_OK);
            r = &rolling;
        }
        bt_bytes_init_stream(&bytes, &stream, r);
        /* The window holds only what came since its last refill, from 0 to 32 KiB back, so read on
         * until it reaches further back than the small ring. */
        while ((pos < 100000u ||
                stream.delivered - (stream.window_write - stream.window_valid) <= RING_SMALL) &&
               read_and_check(&pos, DATA_MAX, 1000)) {
        }
        uint32_t live = stream.delivered;
        uint32_t target = stream.window_write - stream.window_valid; /* its oldest byte */
        CHECK(bt_stream_reread(&stream, target, NULL, 0) == BARTAP_OK);
        CHECK(target == 0 || bt_stream_reread(&stream, target - 1u, NULL, 0) == BARTAP_END);
        CHECK(r == NULL || bt_ring_read(r, target, NULL, 0) == BARTAP_END);

        uint32_t reads = card_reads;
        pos = target;
        CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
        while (pos < live && read_and_check(&pos, DATA_MAX, 3000)) {
        }
        CHECK(pos == live);
        CHECK(card_reads == reads);
        CHECK(stream.delivered == live);
        CHECK(r == NULL || r->delivered == live); /* replayed bytes aren't appended again */
        read_to_end(&pos, DATA_MAX);
        fixture_free(&f);
    }
}

/* A seek back past the ring and the window rewinds, and the ring starts over (R48 invariant 4). */
static void test_rewind(void)
{
    fixture_t f;
    uint32_t pos = 0;

    open_entry(&f, DATA_MAX);
    CHECK(bartap_rolling_init(&rolling, ring_big, RING_BIG) == BARTAP_OK);
    bt_bytes_init_stream(&bytes, &stream, &rolling);
    read_to_end(&pos, DATA_MAX);
    rolling.checkpoints[0].valid = 1;
    rolling.checkpoints[1].valid = 1;

    pos = 1000;
    CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
    CHECK(stream.delivered == 0 && rolling.delivered == 0);
    CHECK(!rolling.checkpoints[0].valid && !rolling.checkpoints[1].valid);

    /* The next read skips up to the target, and the skipped bytes still go into the ring. */
    read_and_check(&pos, DATA_MAX, 500);
    CHECK(rolling.delivered == pos && stream.delivered == pos);
    read_to_end(&pos, DATA_MAX); /* END only if the CRC was folded once per byte */
    fixture_free(&f);
}

/* A seek ahead of the stream, and the edges of an entry. */
static void test_seek_ahead_and_edges(void)
{
    fixture_t f;
    uint32_t pos = 0;
    uint32_t got;

    open_entry(&f, DATA_MAX);
    CHECK(bartap_rolling_init(&rolling, ring_big, RING_BIG) == BARTAP_OK);
    bt_bytes_init_stream(&bytes, &stream, &rolling);
    CHECK(bt_bytes_read(&bytes, out, 0, &got) == BARTAP_ERR_ARG);

    pos = 120000;
    CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
    CHECK(stream.delivered == 0); /* nothing moves until the next read */
    read_and_check(&pos, DATA_MAX, 1000);
    CHECK(stream.delivered == pos && rolling.delivered == pos);

    CHECK(bt_bytes_seek(&bytes, DATA_MAX + 1u) == BARTAP_ERR_ARG);
    CHECK(bt_bytes_seek(&bytes, UINT32_MAX) == BARTAP_ERR_ARG);
    pos = DATA_MAX;
    CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
    read_and_check(&pos, DATA_MAX, 1000);
    fixture_free(&f);

    /* An empty entry ends at once. */
    open_entry(&f, 0);
    bt_bytes_init_stream(&bytes, &stream, NULL);
    pos = 0;
    read_and_check(&pos, 0, 10);
    CHECK(bt_bytes_seek(&bytes, 0) == BARTAP_OK);
    CHECK(bt_bytes_seek(&bytes, 1) == BARTAP_ERR_ARG);
    fixture_free(&f);
}

/* Random reads and seeks against a model: every byte matches wherever it was served from, and the
 * ring always tracks the stream. */
static void test_random(void)
{
    static const uint32_t ring_sizes[] = {0, RING_SMALL, RING_BIG};

    for (size_t s = 0; s < sizeof ring_sizes / sizeof ring_sizes[0]; s++) {
        fixture_t f;
        uint32_t pos = 0;
        bartap_rolling_t *r = NULL;

        open_entry(&f, DATA_MAX);
        if (ring_sizes[s] != 0) {
            uint8_t *ring = ring_sizes[s] == RING_BIG ? ring_big : ring_small;
            CHECK(bartap_rolling_init(&rolling, ring, ring_sizes[s]) == BARTAP_OK);
            r = &rolling;
        }
        bt_bytes_init_stream(&bytes, &stream, r);

        for (int op = 0; op < 3000; op++) {
            uint32_t x = fixture_random(&rng);
            if (x % 10u < 6u) {
                read_and_check(&pos, DATA_MAX, 1u + (x >> 8) % 6000u);
            } else {
                /* Mostly short hops back, sometimes anywhere. */
                uint32_t hop = (x >> 8) % 80000u;
                pos = x % 10u < 9u ? (pos > hop ? pos - hop : 0) : (x >> 4) % (DATA_MAX + 1u);
                CHECK(bt_bytes_seek(&bytes, pos) == BARTAP_OK);
            }
            CHECK(r == NULL || r->delivered == stream.delivered);
        }
        read_to_end(&pos, DATA_MAX);
        fixture_free(&f);
    }
}

int main(void)
{
    test_rolling_init();
    test_ring_contents();
    test_ring_virtual();
    test_memory();
    test_replay_from_ring();
    test_replay_from_window();
    test_rewind();
    test_seek_ahead_and_edges();
    test_random();
    return CHECK_RESULT();
}
