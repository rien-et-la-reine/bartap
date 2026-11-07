/* Entry streams (R22, R26, R29, R31): opening, end checks, sticky results, rewinding, re-reading
 * and I/O failures. Real compressed data is in diff/test_inflate_zlib.c: here, deflate entries hold
 * stored blocks, which is enough to exercise both methods' paths through a stream. */
#include "stream_fixture.h"

#define METHOD_STORED 0u
#define METHOD_DEFLATE 8u
#define DATA_MAX 200000u

static uint8_t window[BARTAP_WINDOW_SIZE];
static bartap_stream_t stream;
static uint8_t data[DATA_MAX];
static uint8_t out[DATA_MAX + 16u];
static uint32_t rng = 0x2468ACE1u;

static uint32_t make_data(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)fixture_random(&rng);
    }
    return bt_crc32(0, data, len);
}

/* The same bytes as a stored entry, or as a deflate entry of final stored blocks. */
static void build_entry(fixture_t *f, uint32_t len, uint16_t method, uint32_t crc)
{
    if (method == METHOD_STORED) {
        fixture_build(f, data, len, method, len, crc);
        return;
    }
    enum { BLOCK_MAX = 65535, BLOCK_OVERHEAD = 5 };
    uint32_t blocks = len / BLOCK_MAX + 1u;
    uint8_t *comp = fixture_alloc((size_t)len + (size_t)blocks * BLOCK_OVERHEAD);
    uint32_t in = 0;
    uint32_t pos = 0;

    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t size = len - in < BLOCK_MAX ? len - in : BLOCK_MAX;
        comp[pos++] = b + 1u == blocks ? 1u : 0u;
        comp[pos++] = (uint8_t)(size & 0xFFu);
        comp[pos++] = (uint8_t)(size >> 8);
        comp[pos++] = (uint8_t)(~size & 0xFFu);
        comp[pos++] = (uint8_t)((~size >> 8) & 0xFFu);
        if (size != 0) {
            memcpy(comp + pos, data + in, size);
        }
        pos += size;
        in += size;
    }
    fixture_build(f, comp, pos, method, len, crc);
    free(comp);
}

static const uint16_t methods[] = {METHOD_STORED, METHOD_DEFLATE};

static void test_round_trip(void)
{
    static const uint32_t lens[] = {0, 1, 100, 257, 258, 259, BARTAP_WINDOW_SIZE - 1u,
                                    BARTAP_WINDOW_SIZE, BARTAP_WINDOW_SIZE + 1u, DATA_MAX};
    for (size_t m = 0; m < 2u; m++) {
        for (size_t l = 0; l < sizeof lens / sizeof lens[0]; l++) {
            fixture_t f;
            uint32_t total;
            uint32_t crc = make_data(lens[l]);

            build_entry(&f, lens[l], methods[m], crc);
            CHECK(bt_stream_open(&stream, &f.source, &f.entry, window) == BARTAP_OK);
            CHECK(fixture_drain(&stream, out, lens[l], &total, &rng, true) == BARTAP_END);
            CHECK(total == lens[l]);
            CHECK(lens[l] == 0 || memcmp(out, data, lens[l]) == 0);
            fixture_free(&f);
        }
    }
}

/* R22: the directory decides. A local header that disagrees is not consulted. */
static void test_local_header_ignored(void)
{
    fixture_t f;
    uint32_t total;
    uint32_t crc = make_data(500);

    build_entry(&f, 500, METHOD_DEFLATE, crc);
    memset(f.bytes + FIXTURE_LOCAL_OFFSET + 8, 0, 2);     /* method */
    memset(f.bytes + FIXTURE_LOCAL_OFFSET + 14, 0xFF, 12); /* CRC and sizes */
    CHECK(bt_stream_open(&stream, &f.source, &f.entry, window) == BARTAP_OK);
    CHECK(fixture_drain(&stream, out, 500, &total, &rng, false) == BARTAP_END);
    CHECK(total == 500 && memcmp(out, data, 500) == 0);
    fixture_free(&f);
}

static void check_open_fails(const fixture_t *f, const bartap_entry_t *entry, bartap_err_t want)
{
    uint8_t byte;
    uint32_t got = 99;

    CHECK(bt_stream_open(&stream, &f->source, entry, window) == want);

    /* A stream that never opened only repeats why. */
    CHECK(bt_stream_read(&stream, &byte, 1, &got) == want);
    CHECK(got == 0);
    CHECK(bt_stream_rewind(&stream) == want);
    CHECK(bt_stream_read(&stream, &byte, 1, &got) == want);
    CHECK(bt_stream_reread(&stream, 0, &byte, 1) == BARTAP_ERR_ARG);
}

static void test_open_failures(void)
{
    fixture_t f;
    bartap_entry_t entry;
    uint32_t crc = make_data(500);

    build_entry(&f, 500, METHOD_DEFLATE, crc);

    entry = f.entry;
    entry.method = 99;
    check_open_fails(&f, &entry, BARTAP_ERR_UNSUPPORTED);

    entry = f.entry;
    entry.local_offset += 1u;
    check_open_fails(&f, &entry, BARTAP_ERR_CORRUPT);

    entry = f.entry;
    entry.local_offset = f.source.size + 5u;
    check_open_fails(&f, &entry, BARTAP_ERR_CORRUPT);

    entry = f.entry;
    entry.local_offset = UINT32_MAX;
    check_open_fails(&f, &entry, BARTAP_ERR_CORRUPT);

    entry = f.entry;
    entry.local_offset = f.source.size - 10u; /* not even a whole header */
    check_open_fails(&f, &entry, BARTAP_ERR_CORRUPT);

    entry = f.entry;
    entry.compressed_size = f.source.size;
    check_open_fails(&f, &entry, BARTAP_ERR_CORRUPT);

    entry = f.entry;
    entry.compressed_size = UINT32_MAX;
    check_open_fails(&f, &entry, BARTAP_ERR_CORRUPT);

    /* Name and extra lengths that run past the archive. */
    memset(f.bytes + FIXTURE_LOCAL_OFFSET + 26, 0xFF, 4);
    check_open_fails(&f, &f.entry, BARTAP_ERR_CORRUPT);
    fixture_free(&f);

    /* An empty archive has no header to read. */
    bartap_source_t empty;
    CHECK(bartap_source_memory(&empty, NULL, 0) == BARTAP_OK);
    entry.local_offset = 0;
    entry.method = METHOD_STORED;
    CHECK(bt_stream_open(&stream, &empty, &entry, window) == BARTAP_ERR_CORRUPT);
}

/* R31: END only if CRC and size match, and neither the end nor a failure goes away on its own. */
static void test_end_checks(void)
{
    for (size_t m = 0; m < 2u; m++) {
        fixture_t f;
        uint32_t total;
        uint32_t got = 99;
        uint8_t byte;
        uint32_t crc = make_data(1000);

        build_entry(&f, 1000, methods[m], crc);
        bartap_entry_t good = f.entry;

        CHECK(bt_stream_open(&stream, &f.source, &good, window) == BARTAP_OK);
        CHECK(bt_stream_read(&stream, out, 0, &got) == BARTAP_ERR_ARG);
        CHECK(got == 0);
        CHECK(fixture_drain(&stream, out, 1000, &total, &rng, false) == BARTAP_END);
        CHECK(bt_stream_read(&stream, &byte, 1, &got) == BARTAP_END);
        CHECK(got == 0);

        bartap_entry_t bad = good;
        bad.crc32 ^= 1u;
        CHECK(bt_stream_open(&stream, &f.source, &bad, window) == BARTAP_OK);
        CHECK(fixture_drain(&stream, out, 1000, &total, &rng, false) == BARTAP_ERR_CORRUPT);
        CHECK(total == 1000);
        CHECK(bt_stream_read(&stream, &byte, 1, &got) == BARTAP_ERR_CORRUPT);
        CHECK(got == 0);

        /* Too small a size fails before going past it, and too large a size fails at the end. */
        bad = good;
        bad.uncompressed_size = 999;
        CHECK(bt_stream_open(&stream, &f.source, &bad, window) == BARTAP_OK);
        CHECK(fixture_drain(&stream, out, 1000, &total, &rng, false) == BARTAP_ERR_CORRUPT);
        CHECK(total <= 999);

        bad = good;
        bad.uncompressed_size = 1001;
        CHECK(bt_stream_open(&stream, &f.source, &bad, window) == BARTAP_OK);
        CHECK(fixture_drain(&stream, out, 1001, &total, &rng, false) == BARTAP_ERR_CORRUPT);

        /* Too little compressed data. */
        bad = good;
        bad.compressed_size -= 1u;
        CHECK(bt_stream_open(&stream, &f.source, &bad, window) == BARTAP_OK);
        CHECK(fixture_drain(&stream, out, 1000, &total, &rng, false) == BARTAP_ERR_CORRUPT);
        fixture_free(&f);
    }
}

static void test_rewind(void)
{
    enum { LEN = 100000 };

    for (size_t m = 0; m < 2u; m++) {
        fixture_t f;
        uint32_t total;
        uint32_t got;
        uint8_t byte;
        uint32_t crc = make_data(LEN);

        build_entry(&f, LEN, methods[m], crc);
        CHECK(bt_stream_open(&stream, &f.source, &f.entry, window) == BARTAP_OK);

        /* From the end, and from each of several points partway. */
        CHECK(fixture_drain(&stream, out, LEN, &total, &rng, false) == BARTAP_END);
        static const uint32_t stops[] = {1, 5000, LEN / 2, LEN - 1};
        for (size_t s = 0; s < sizeof stops / sizeof stops[0]; s++) {
            CHECK(bt_stream_rewind(&stream) == BARTAP_OK);
            CHECK(bt_stream_reread(&stream, 0, &byte, 1) == BARTAP_ERR_ARG);
            for (uint32_t pos = 0; pos < stops[s]; pos += got) {
                CHECK(bt_stream_read(&stream, out + pos, stops[s] - pos, &got) == BARTAP_OK);
                if (got == 0) {
                    break;
                }
            }
            CHECK(bt_stream_rewind(&stream) == BARTAP_OK);
            memset(out, 0, LEN);
            CHECK(fixture_drain(&stream, out, LEN, &total, &rng, true) == BARTAP_END);
            CHECK(total == LEN && memcmp(out, data, LEN) == 0);
        }

        /* A failure clears too: the CRC check has restarted, so it fails the same way again. */
        bartap_entry_t bad = f.entry;
        bad.crc32 ^= 1u;
        CHECK(bt_stream_open(&stream, &f.source, &bad, window) == BARTAP_OK);
        CHECK(fixture_drain(&stream, out, LEN, &total, &rng, false) == BARTAP_ERR_CORRUPT);
        CHECK(bt_stream_rewind(&stream) == BARTAP_OK);
        CHECK(fixture_drain(&stream, out, LEN, &total, &rng, false) == BARTAP_ERR_CORRUPT);
        CHECK(total == LEN);
        fixture_free(&f);
    }
}

/* R26: bytes come back without disturbing the read position or the CRC check. */
static void test_reread(void)
{
    enum { LEN = 4000 };
    fixture_t f;
    uint8_t copy[64];
    uint32_t got;
    uint32_t crc = make_data(LEN);

    build_entry(&f, LEN, METHOD_DEFLATE, crc);
    CHECK(bt_stream_open(&stream, &f.source, &f.entry, window) == BARTAP_OK);
    CHECK(bt_stream_reread(&stream, 0, copy, 0) == BARTAP_OK);
    CHECK(bt_stream_reread(&stream, 0, copy, 1) == BARTAP_ERR_ARG);

    CHECK(bt_stream_read(&stream, out, 1000, &got) == BARTAP_OK);
    CHECK(got == 1000);
    CHECK(bt_stream_reread(&stream, 990, copy, 10) == BARTAP_OK);
    CHECK(memcmp(copy, data + 990, 10) == 0);
    CHECK(bt_stream_reread(&stream, 990, copy, 11) == BARTAP_ERR_ARG);
    CHECK(bt_stream_reread(&stream, 1000, copy, 0) == BARTAP_OK);

    /* Still reads on from 1000, and END proves the CRC never saw those bytes twice. */
    uint32_t pos = 1000;
    bartap_err_t err;
    while ((err = bt_stream_read(&stream, out + pos, LEN - pos + 1u, &got)) == BARTAP_OK) {
        CHECK(bt_stream_reread(&stream, 0, copy, sizeof copy) == BARTAP_OK);
        pos += got;
    }
    CHECK(err == BARTAP_END);
    CHECK(pos == LEN && memcmp(out, data, LEN) == 0);
    fixture_free(&f);
}

/* ---- I/O failures (R37) ---- */

static const uint8_t *io_bytes;
static int io_reads_left;

static int failing_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)user;
    if (io_reads_left == 0) {
        return 1;
    }
    if (io_reads_left > 0) {
        io_reads_left--;
    }
    memcpy(buf, io_bytes + offset, len);
    return 0;
}

static void test_io_failures(void)
{
    enum { LEN = 5000 };

    for (size_t m = 0; m < 2u; m++) {
        fixture_t f;
        uint32_t total;
        uint32_t got;
        uint8_t byte;
        uint32_t crc = make_data(LEN);

        build_entry(&f, LEN, methods[m], crc);
        bartap_source_t failing = f.source;
        failing.read = failing_read;
        failing.user = NULL;
        io_bytes = f.bytes;

        /* The header's read fails. */
        io_reads_left = 0;
        CHECK(bt_stream_open(&stream, &failing, &f.entry, window) == BARTAP_ERR_IO);

        /* A later read fails: it sticks, and a rewind recovers once the card does. */
        for (int ok_reads = 1; ok_reads < 5; ok_reads++) {
            io_reads_left = ok_reads;
            CHECK(bt_stream_open(&stream, &failing, &f.entry, window) == BARTAP_OK);
            CHECK(fixture_drain(&stream, out, LEN, &total, &rng, false) == BARTAP_ERR_IO);
            CHECK(bt_stream_read(&stream, &byte, 1, &got) == BARTAP_ERR_IO);

            io_reads_left = -1;
            CHECK(bt_stream_read(&stream, &byte, 1, &got) == BARTAP_ERR_IO);
            CHECK(bt_stream_rewind(&stream) == BARTAP_OK);
            CHECK(fixture_drain(&stream, out, LEN, &total, &rng, false) == BARTAP_END);
            CHECK(total == LEN && memcmp(out, data, LEN) == 0);
        }
        fixture_free(&f);
    }
}

int main(void)
{
    test_round_trip();
    test_local_header_ignored();
    test_open_failures();
    test_end_checks();
    test_rewind();
    test_reread();
    test_io_failures();
    return CHECK_RESULT();
}
