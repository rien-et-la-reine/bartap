/* The inflater against zlib, the reference implementation (R36): everything zlib writes must decode
 * to the original bytes, and damaged data must fail cleanly or be caught by the end checks (R29,
 * R31). Each case goes through a stream, so it also covers the window and the refills. */
#include <zlib.h>

#include "stream_fixture.h"

#define DATA_MAX 300000u
#define METHOD_DEFLATE 8u

static uint8_t data[DATA_MAX];
static uint8_t window[BARTAP_WINDOW_SIZE];
static bartap_stream_t stream;
static uint32_t rng = 0x12345678u;

/* ---- zlib ---- */

/* Raw deflate. A nonzero flush_every also splits the output into blocks, and adds the empty stored
 * blocks that flushing writes. */
static uint8_t *compress_bytes(const uint8_t *in, uint32_t len, int level, int strategy,
                               uint32_t flush_every, uint32_t *out_len)
{
    z_stream z;
    size_t cap = (size_t)len + len / 8u + 65536u;
    uint8_t *out = fixture_alloc(cap);
    uint32_t pos = 0;
    int flushes = 0;

    memset(&z, 0, sizeof z);
    CHECK(deflateInit2(&z, level, Z_DEFLATED, -15, 8, strategy) == Z_OK);
    z.next_out = out;
    z.avail_out = (uInt)cap;
    while (flush_every != 0 && pos < len) {
        uint32_t chunk = len - pos < flush_every ? len - pos : flush_every;
        z.next_in = (Bytef *)(uintptr_t)(in + pos);
        z.avail_in = chunk;
        CHECK(deflate(&z, flushes % 2 != 0 ? Z_FULL_FLUSH : Z_SYNC_FLUSH) == Z_OK);
        pos += chunk;
        flushes++;
    }
    z.next_in = (Bytef *)(uintptr_t)(in + pos);
    z.avail_in = len - pos;
    CHECK(deflate(&z, Z_FINISH) == Z_STREAM_END);
    *out_len = (uint32_t)z.total_out;
    CHECK(deflateEnd(&z) == Z_OK);
    return out;
}

static uint32_t reference_crc(const uint8_t *bytes, uint32_t len)
{
    return (uint32_t)crc32(0L, bytes, len);
}

/* ---- Data with different shapes ---- */

static void make_text(uint32_t len)
{
    static uint8_t words[800][10];
    for (size_t i = 0; i < 800u; i++) {
        for (size_t j = 0; j < 10u; j++) {
            words[i][j] = (uint8_t)('a' + fixture_random(&rng) % 26u);
        }
    }
    for (uint32_t pos = 0; pos < len;) {
        const uint8_t *word = words[fixture_random(&rng) % 800u];
        uint32_t word_len = 2u + fixture_random(&rng) % 8u;
        for (uint32_t i = 0; i < word_len && pos < len; i++) {
            data[pos++] = word[i];
        }
        if (pos < len) {
            data[pos++] = ' ';
        }
    }
}

/* Forces matches at the longest distance DEFLATE has. */
static void make_periodic(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        data[i] = i < 32768u ? (uint8_t)fixture_random(&rng) : data[i - 32768u];
    }
}

static void make_runs(uint32_t len)
{
    for (uint32_t pos = 0; pos < len;) {
        uint8_t byte = (uint8_t)fixture_random(&rng);
        uint32_t run = 1u + fixture_random(&rng) % 600u;
        while (run-- != 0 && pos < len) {
            data[pos++] = byte;
        }
    }
}

static void make_random(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)fixture_random(&rng);
    }
}

static void make_counting(uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)i;
    }
}

static void make_zeros(uint32_t len)
{
    memset(data, 0, len);
}

static const struct {
    const char *name;
    void (*make)(uint32_t len);
    uint32_t len;
} shapes[] = {
    {"empty", make_zeros, 0},          {"one byte", make_random, 1},
    {"short text", make_text, 200},    {"text", make_text, 150000},
    {"random", make_random, 100000},   {"periodic", make_periodic, 131072},
    {"runs", make_runs, 200000},       {"counting", make_counting, 80000},
    {"zeros", make_zeros, DATA_MAX},
};
#define SHAPE_COUNT (sizeof shapes / sizeof shapes[0])

static const int levels[] = {0, 1, 6, 9};
static const int strategies[] = {Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED};

/* ---- Cases ---- */

/* Opens and reads one entry, and returns the final result. */
static bartap_err_t read_entry(const uint8_t *comp, uint32_t comp_len, uint32_t uncompressed_size,
                               uint32_t crc, uint8_t *out, uint32_t out_size, uint32_t *total,
                               bool check_reread)
{
    fixture_t f;
    bartap_err_t err;

    fixture_build(&f, comp, comp_len, METHOD_DEFLATE, uncompressed_size, crc);
    *total = 0;
    err = bt_stream_open(&stream, &f.source, &f.entry, window);
    if (err == BARTAP_OK) {
        err = fixture_drain(&stream, out, out_size, total, &rng, check_reread);
    }
    fixture_free(&f);
    return err;
}

static void report_if_failed(int before, const char *shape, int level, int strategy)
{
    if (check_failures != before) {
        printf("  in %s, level %d, strategy %d\n", shape, level, strategy);
    }
}

static void test_round_trips(void)
{
    static uint8_t out[DATA_MAX];

    for (size_t s = 0; s < SHAPE_COUNT; s++) {
        uint32_t len = shapes[s].len;
        shapes[s].make(len);
        uint32_t crc = reference_crc(data, len);

        for (size_t l = 0; l < sizeof levels / sizeof levels[0]; l++) {
            for (size_t t = 0; t < sizeof strategies / sizeof strategies[0]; t++) {
                int before = check_failures;
                uint32_t comp_len;
                uint32_t total;
                uint8_t *comp = compress_bytes(data, len, levels[l], strategies[t], 0, &comp_len);

                CHECK(read_entry(comp, comp_len, len, crc, out, DATA_MAX, &total, true) ==
                      BARTAP_END);
                CHECK(total == len);
                CHECK(len == 0 || memcmp(out, data, len) == 0);
                free(comp);
                report_if_failed(before, shapes[s].name, levels[l], strategies[t]);
            }
        }

        int before = check_failures;
        uint32_t comp_len;
        uint32_t total;
        uint8_t *comp = compress_bytes(data, len, 6, Z_DEFAULT_STRATEGY, 7001, &comp_len);
        CHECK(read_entry(comp, comp_len, len, crc, out, DATA_MAX, &total, true) == BARTAP_END);
        CHECK(total == len);
        CHECK(len == 0 || memcmp(out, data, len) == 0);
        free(comp);
        report_if_failed(before, shapes[s].name, 6, -1);
    }
}

/* Truncated data, a wrong size either way, and a wrong CRC are each corrupt. */
static void test_damage(void)
{
    static uint8_t out[DATA_MAX];
    enum { LEN = 40000 };
    static const int damage_levels[] = {0, 1, 9};

    for (size_t s = 0; s < SHAPE_COUNT; s++) {
        uint32_t len = shapes[s].len < LEN ? shapes[s].len : LEN;
        if (len == 0) {
            continue;
        }
        shapes[s].make(len);
        uint32_t crc = reference_crc(data, len);

        for (size_t l = 0; l < sizeof damage_levels / sizeof damage_levels[0]; l++) {
            int before = check_failures;
            uint32_t comp_len;
            uint32_t total;
            uint8_t *comp = compress_bytes(data, len, damage_levels[l], Z_DEFAULT_STRATEGY, 0,
                                           &comp_len);
            const uint32_t cuts[] = {0, 1, comp_len / 2u, comp_len - 2u, comp_len - 1u};

            for (size_t c = 0; c < sizeof cuts / sizeof cuts[0]; c++) {
                if (cuts[c] < comp_len) {
                    CHECK(read_entry(comp, cuts[c], len, crc, out, DATA_MAX, &total, false) ==
                          BARTAP_ERR_CORRUPT);
                }
            }
            CHECK(read_entry(comp, comp_len, len - 1u, crc, out, DATA_MAX, &total, false) ==
                  BARTAP_ERR_CORRUPT);
            CHECK(total <= len - 1u);
            CHECK(read_entry(comp, comp_len, len / 2u, crc, out, DATA_MAX, &total, false) ==
                  BARTAP_ERR_CORRUPT);
            CHECK(total <= len / 2u);
            CHECK(read_entry(comp, comp_len, len + 1u, crc, out, DATA_MAX, &total, false) ==
                  BARTAP_ERR_CORRUPT);
            CHECK(read_entry(comp, comp_len, len, crc ^ 1u, out, DATA_MAX, &total, false) ==
                  BARTAP_ERR_CORRUPT);
            free(comp);
            report_if_failed(before, shapes[s].name, damage_levels[l], Z_DEFAULT_STRATEGY);
        }
    }
}

/* ---- Hand-built blocks: what zlib never writes ---- */

typedef struct {
    uint8_t bytes[8192];
    uint32_t bits;
} bits_t;

/* Values go in least significant bit first. */
static void put(bits_t *b, uint32_t value, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (((value >> i) & 1u) != 0) {
            b->bytes[b->bits / 8u] |= (uint8_t)(1u << (b->bits % 8u));
        }
        b->bits++;
    }
}

/* Huffman codes go in most significant bit first. */
static void put_code(bits_t *b, uint32_t code, uint32_t count)
{
    for (uint32_t i = count; i-- != 0;) {
        put(b, (code >> i) & 1u, 1);
    }
}

/* A final fixed-Huffman block's header and a dynamic block's, up to its code-length lengths. */
static void put_fixed(bits_t *b)
{
    put(b, 1, 1);
    put(b, 1, 2);
}

static void put_dynamic(bits_t *b, uint32_t hlit, uint32_t hdist, uint32_t hclen)
{
    put(b, 1, 1);
    put(b, 2, 2);
    put(b, hlit, 5);
    put(b, hdist, 5);
    put(b, hclen, 4);
}

/* A failing case must fail at the check it targets, not at the end checks that would catch almost
 * anything. So it must deliver nothing, and any size limit is out of reach, unless want_out says
 * what a decoder without that check would produce: then size and CRC are right for those bytes. */
static void expect(const char *name, const uint8_t *bytes, uint32_t len, bartap_err_t want,
                   const char *want_out)
{
    static uint8_t out[70000];
    uint32_t want_len = (uint32_t)strlen(want_out);
    uint32_t size = want == BARTAP_END || want_len != 0 ? want_len : 60000u;
    uint32_t crc = reference_crc((const uint8_t *)want_out, want_len);
    uint32_t total;
    int before = check_failures;

    CHECK(read_entry(bytes, len, size, crc, out, sizeof out, &total, false) == want);
    if (want == BARTAP_END) {
        CHECK(total == want_len && memcmp(out, want_out, want_len) == 0);
    } else {
        CHECK(total == 0);
    }
    if (check_failures != before) {
        printf("  in %s\n", name);
    }
}

static void expect_bits(const char *name, const bits_t *b, bartap_err_t want, const char *want_out)
{
    expect(name, b->bytes, (b->bits + 7u) / 8u, want, want_out);
}

static void test_hand_built(void)
{
    static const uint8_t empty_stored[] = {0x01, 0x00, 0x00, 0xFF, 0xFF};
    static const uint8_t bad_nlen[] = {0x01, 0x02, 0x00, 0xFD, 0xFE, 'a', 'b'};
    static const uint8_t reserved[] = {0x07, 0x00, 0x00, 0x00};
    static const uint8_t stored_then_fixed[] = {0x00, 0x02, 0x00, 0xFD, 0xFF, 'a', 'b', 0x03, 0x00};
    bits_t b;

    expect("empty stored block", empty_stored, sizeof empty_stored, BARTAP_END, "");
    expect("stored block's length check", bad_nlen, sizeof bad_nlen, BARTAP_ERR_CORRUPT, "ab");
    expect("reserved block type", reserved, sizeof reserved, BARTAP_ERR_CORRUPT, "");
    expect("stored then fixed", stored_then_fixed, sizeof stored_then_fixed, BARTAP_END, "ab");

    /* Length 3 at distance 1, with nothing written yet. */
    memset(&b, 0, sizeof b);
    put_fixed(&b);
    put_code(&b, 1, 7);
    put_code(&b, 0, 5);
    put_code(&b, 0, 7);
    expect_bits("distance before the start", &b, BARTAP_ERR_CORRUPT, "");

    /* A literal 'A', then distance 2 where only one byte exists. */
    memset(&b, 0, sizeof b);
    put_fixed(&b);
    put_code(&b, 0x30u + 'A', 8);
    put_code(&b, 1, 7);
    put_code(&b, 1, 5);
    put_code(&b, 0, 7);
    expect_bits("distance one too far", &b, BARTAP_ERR_CORRUPT, "");

    /* Fixed codes reach two symbols that DEFLATE leaves undefined. */
    memset(&b, 0, sizeof b);
    put_fixed(&b);
    put_code(&b, 0x30u + 'A', 8);
    put_code(&b, 1, 7);
    put_code(&b, 30, 5);
    put_code(&b, 0, 7);
    expect_bits("distance symbol 30", &b, BARTAP_ERR_CORRUPT, "");

    memset(&b, 0, sizeof b);
    put_fixed(&b);
    put_code(&b, 0xC6, 8);
    put_code(&b, 0, 7);
    expect_bits("literal symbol 286", &b, BARTAP_ERR_CORRUPT, "");

    memset(&b, 0, sizeof b);
    put_dynamic(&b, 30, 0, 0);
    put(&b, 0, 20);
    expect_bits("too many literal codes", &b, BARTAP_ERR_CORRUPT, "");

    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 30, 0);
    put(&b, 0, 20);
    expect_bits("too many distance codes", &b, BARTAP_ERR_CORRUPT, "");

    /* The code-length order starts 16, 17, 18, 0. */
    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 0);
    put(&b, 0, 12);
    put(&b, 0, 16);
    expect_bits("no code-length codes", &b, BARTAP_ERR_CORRUPT, "");

    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 0);
    put(&b, 1, 3);
    put(&b, 1, 3);
    put(&b, 1, 3);
    put(&b, 0, 3);
    put(&b, 0, 16);
    expect_bits("over-subscribed code-length codes", &b, BARTAP_ERR_CORRUPT, "");

    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 0);
    put(&b, 0, 6);
    put(&b, 2, 3);
    put(&b, 0, 3);
    put(&b, 0, 16);
    expect_bits("incomplete code-length codes", &b, BARTAP_ERR_CORRUPT, "");

    /* From here, one-bit codes: symbol 0 is 0, and the other symbol given a length is 1. */
    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 0);
    put(&b, 1, 3);
    put(&b, 0, 3);
    put(&b, 0, 3);
    put(&b, 1, 3);
    put_code(&b, 1, 1);
    put(&b, 0, 2);
    expect_bits("repeat with nothing before it", &b, BARTAP_ERR_CORRUPT, "");

    /* 257 + 1 lengths are declared: 138 zeros twice runs past them. */
    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 0);
    put(&b, 0, 3);
    put(&b, 0, 3);
    put(&b, 1, 3);
    put(&b, 1, 3);
    put_code(&b, 1, 1);
    put(&b, 127, 7);
    put_code(&b, 1, 1);
    put(&b, 127, 7);
    expect_bits("repeat past the end", &b, BARTAP_ERR_CORRUPT, "");

    /* 138 + 120 zeros fill every length exactly, so symbol 256 has no code. */
    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 0);
    put(&b, 0, 3);
    put(&b, 0, 3);
    put(&b, 1, 3);
    put(&b, 1, 3);
    put_code(&b, 1, 1);
    put(&b, 127, 7);
    put_code(&b, 1, 1);
    put(&b, 109, 7);
    expect_bits("no codes at all", &b, BARTAP_ERR_CORRUPT, "");

    /* Literal 0 is the only code, so a block of zero bits decodes to more than a window of zeros
     * and could never end. The code-length codes are 1 = 0 and 18 = 1. */
    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 14);
    put(&b, 0, 3);
    put(&b, 0, 3);
    put(&b, 1, 3);
    for (uint32_t i = 0; i < 14u; i++) {
        put(&b, 0, 3);
    }
    put(&b, 1, 3);
    put_code(&b, 0, 1);
    put_code(&b, 1, 1);
    put(&b, 127, 7);
    put_code(&b, 1, 1);
    put(&b, 108, 7);
    b.bits = sizeof b.bytes * 8u;
    expect_bits("no end-of-block code", &b, BARTAP_ERR_CORRUPT, "");

    /* A single one-bit literal code, for end of block, and no distance codes. zlib allows this. The
     * code-length codes are 18 = 0, 0 = 10 and 1 = 11. */
    memset(&b, 0, sizeof b);
    put_dynamic(&b, 0, 0, 14);
    put(&b, 0, 3);
    put(&b, 0, 3);
    put(&b, 1, 3);
    put(&b, 2, 3);
    for (uint32_t i = 0; i < 13u; i++) {
        put(&b, 0, 3);
    }
    put(&b, 2, 3);
    put_code(&b, 0, 1);
    put(&b, 127, 7);
    put_code(&b, 0, 1);
    put(&b, 107, 7);
    put_code(&b, 3, 2);
    put_code(&b, 2, 2);
    put_code(&b, 0, 1);
    expect_bits("single literal code", &b, BARTAP_END, "");
}

/* ---- Damaged input: no crash or hang, and END only for bytes that really match ---- */

static void check_damaged(const uint8_t *comp, uint32_t comp_len, uint32_t size, uint32_t crc)
{
    static uint8_t out[70000];
    uint32_t total;
    bartap_err_t err = read_entry(comp, comp_len, size, crc, out, sizeof out, &total, false);

    CHECK(err != BARTAP_OK);
    CHECK(total <= size);
    if (err == BARTAP_END) {
        CHECK(total == size);
        CHECK(reference_crc(out, total) == crc);
    }
}

static void test_corruption(void)
{
    enum { LEN = 4000, ROUNDS = 800 };
    static const int fuzz_levels[] = {0, 1, 6, 9};
    static uint8_t damaged[70000];

    for (size_t l = 0; l < sizeof fuzz_levels / sizeof fuzz_levels[0]; l++) {
        for (size_t t = 0; t < 2u; t++) {
            uint32_t comp_len;
            make_text(LEN);
            uint32_t crc = reference_crc(data, LEN);
            uint8_t *comp = compress_bytes(data, LEN, fuzz_levels[l],
                                           t == 0 ? Z_DEFAULT_STRATEGY : Z_FIXED, 0, &comp_len);
            CHECK(comp_len <= sizeof damaged);

            for (uint32_t round = 0; round < ROUNDS; round++) {
                uint32_t flips = 1u + fixture_random(&rng) % 6u;
                memcpy(damaged, comp, comp_len);
                for (uint32_t i = 0; i < flips; i++) {
                    uint32_t at = fixture_random(&rng) % comp_len;
                    damaged[at] ^= (uint8_t)(1u << (fixture_random(&rng) % 8u));
                }
                check_damaged(damaged, comp_len, LEN, crc);
            }
            free(comp);
        }
    }

    /* Random bytes, under each block type. */
    for (uint32_t round = 0; round < 3000u; round++) {
        uint32_t len = fixture_random(&rng) % 800u + 1u;
        for (uint32_t i = 0; i < len; i++) {
            damaged[i] = (uint8_t)fixture_random(&rng);
        }
        damaged[0] = (uint8_t)((damaged[0] & 0xF9u) | ((round & 3u) << 1));
        check_damaged(damaged, len, 60000, fixture_random(&rng));
    }
}

int main(void)
{
    test_round_trips();
    test_damage();
    test_hand_built();
    test_corruption();
    return CHECK_RESULT();
}
