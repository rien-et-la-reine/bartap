/* Phase 1: results, warnings, I/O gate, memory source, CRC-32, name hash. */
#include <string.h>

#include "check.h"
#include "internal.h"

static void test_strerror(void)
{
    static const struct {
        bartap_err_t err;
        const char *name;
    } names[] = {
        {BARTAP_OK, "BARTAP_OK"},
        {BARTAP_END, "BARTAP_END"},
        {BARTAP_MORE, "BARTAP_MORE"},
        {BARTAP_ERR_IO, "BARTAP_ERR_IO"},
        {BARTAP_ERR_CORRUPT, "BARTAP_ERR_CORRUPT"},
        {BARTAP_ERR_UNSUPPORTED, "BARTAP_ERR_UNSUPPORTED"},
        {BARTAP_ERR_ARG, "BARTAP_ERR_ARG"},
        {BARTAP_ERR_BUFFER_TOO_SMALL, "BARTAP_ERR_BUFFER_TOO_SMALL"},
        {BARTAP_ERR_WRONG_BOOK, "BARTAP_ERR_WRONG_BOOK"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        CHECK(strcmp(bartap_strerror(names[i].err), names[i].name) == 0);
    }
    CHECK(strcmp(bartap_strerror((bartap_err_t)-999), "BARTAP_ERR_UNKNOWN") == 0);

    /* Statuses are non-negative and errors negative, so callers can test the sign. */
    CHECK(BARTAP_OK == 0 && BARTAP_END > 0 && BARTAP_MORE > 0);
    CHECK(BARTAP_ERR_IO < 0 && BARTAP_ERR_CORRUPT < 0 && BARTAP_ERR_WRONG_BOOK < 0);
}

static void test_limits(void)
{
    /* version 1 + book hash 4 + spine 2 + block 4 + char 4 + chapter CRC 4 + bookmark CRC 4 (R12) */
    CHECK(BARTAP_BOOKMARK_SIZE == 1 + 4 + 2 + 4 + 4 + 4 + 4);
    CHECK(BARTAP_TEXT_MIN == 4);
    CHECK(BARTAP_DEPTH_MAX == 32);
}

static struct {
    int calls;
    void *user;
    bartap_warning_t code;
    uint16_t spine;
    uint32_t offset;
} warn_seen;

static void warn_spy(void *user, bartap_warning_t code, uint16_t spine, uint32_t offset)
{
    warn_seen.calls++;
    warn_seen.user = user;
    warn_seen.code = code;
    warn_seen.spine = spine;
    warn_seen.offset = offset;
}

static void test_warnings(void)
{
    /* NULL callback is valid; bits are sticky and accumulate. */
    bartap_warnings_t w = {0, NULL, NULL, 0};
    bt_warn(&w, BARTAP_WARN_CONTROL_CHAR, 1, 2);
    bt_warn(&w, BARTAP_WARN_DEPTH_EXCEEDED, 1, 2);
    bt_warn(&w, BARTAP_WARN_CONTROL_CHAR, 1, 2);
    CHECK(w.bits == ((uint32_t)BARTAP_WARN_CONTROL_CHAR | (uint32_t)BARTAP_WARN_DEPTH_EXCEEDED));

    /* The callback fires at each occurrence, with its context. */
    int token = 0;
    bartap_warnings_t c = {0, warn_spy, &token, 0};
    bt_warn(&c, BARTAP_WARN_BROKEN_LINK, 7, 1234);
    bt_warn(&c, BARTAP_WARN_BROKEN_LINK, BARTAP_NO_SPINE, 5678);
    CHECK(warn_seen.calls == 2);
    CHECK(warn_seen.user == &token);
    CHECK(warn_seen.code == BARTAP_WARN_BROKEN_LINK);
    CHECK(warn_seen.spine == BARTAP_NO_SPINE);
    CHECK(warn_seen.offset == 5678);

    /* During a replay the callback is quiet below quiet_below, but the bit is still set (R49). */
    c.quiet_below = 100;
    c.bits = 0;
    bt_warn(&c, BARTAP_WARN_CONTROL_CHAR, 1, 99);
    CHECK(warn_seen.calls == 2 && c.bits == (uint32_t)BARTAP_WARN_CONTROL_CHAR);
    bt_warn(&c, BARTAP_WARN_CONTROL_CHAR, 1, 100);
    CHECK(warn_seen.calls == 3);
}

static int read_calls;
static int read_result;

static int counting_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)user;
    (void)offset;
    (void)buf;
    (void)len;
    read_calls++;
    return read_result;
}

static void test_read_gate(void)
{
    bartap_source_t src = {counting_read, NULL, 100};
    uint8_t buf[1];

    /* In range reaches the callback. */
    read_calls = 0;
    read_result = 0;
    CHECK(bt_read(&src, 0, buf, 1) == BARTAP_OK);
    CHECK(bt_read(&src, 99, buf, 1) == BARTAP_OK);
    CHECK(bt_read(&src, 0, buf, 100) == BARTAP_OK);
    CHECK(read_calls == 3);

    /* Out of range never does, including ranges that wrap a uint32_t. */
    read_calls = 0;
    CHECK(bt_read(&src, 100, buf, 1) == BARTAP_ERR_CORRUPT);
    CHECK(bt_read(&src, 101, buf, 0) == BARTAP_ERR_CORRUPT);
    CHECK(bt_read(&src, 0, buf, 101) == BARTAP_ERR_CORRUPT);
    CHECK(bt_read(&src, 99, buf, 2) == BARTAP_ERR_CORRUPT);
    CHECK(bt_read(&src, 0xFFFFFFFFu, buf, 2) == BARTAP_ERR_CORRUPT);
    CHECK(bt_read(&src, 2, buf, 0xFFFFFFFFu) == BARTAP_ERR_CORRUPT);
    CHECK(read_calls == 0);

    /* An empty read in range succeeds without touching the card. */
    CHECK(bt_read(&src, 100, buf, 0) == BARTAP_OK);
    CHECK(read_calls == 0);

    /* Any callback failure is one I/O error (R37). */
    read_result = 1;
    CHECK(bt_read(&src, 0, buf, 1) == BARTAP_ERR_IO);
    read_result = -5;
    CHECK(bt_read(&src, 0, buf, 1) == BARTAP_ERR_IO);
}

static void test_memory_source(void)
{
    static const uint8_t data[] = {10, 20, 30, 40, 50};
    bartap_source_t src;
    uint8_t buf[5] = {0};

    CHECK(bartap_source_memory(&src, data, sizeof data) == BARTAP_OK);
    CHECK(src.size == sizeof data);
    CHECK(bt_read(&src, 1, buf, 3) == BARTAP_OK);
    CHECK(buf[0] == 20 && buf[1] == 30 && buf[2] == 40 && buf[3] == 0);
    CHECK(bt_read(&src, 0, buf, 5) == BARTAP_OK);
    CHECK(memcmp(buf, data, sizeof data) == 0);
    CHECK(bt_read(&src, 3, buf, 3) == BARTAP_ERR_CORRUPT);

    /* An empty archive is a valid source with nothing to read. */
    CHECK(bartap_source_memory(&src, NULL, 0) == BARTAP_OK);
    CHECK(bt_read(&src, 0, buf, 0) == BARTAP_OK);
    CHECK(bt_read(&src, 0, buf, 1) == BARTAP_ERR_CORRUPT);

    CHECK(bartap_source_memory(NULL, data, 1) == BARTAP_ERR_ARG);
    CHECK(bartap_source_memory(&src, NULL, 1) == BARTAP_ERR_ARG);
}

static void test_crc32(void)
{
    static const uint8_t check[] = "123456789";

    CHECK(bt_crc32(0, NULL, 0) == 0);
    CHECK(bt_crc32(0, check, 9) == 0xCBF43926u); /* the CRC-32 check value */

    /* Continuing across a split gives the same result at every split point. */
    for (size_t split = 0; split <= 9; split++) {
        CHECK(bt_crc32(bt_crc32(0, check, split), check + split, 9 - split) == 0xCBF43926u);
    }
}

static uint32_t hash_str(const char *s)
{
    return bt_name_hash(BT_NAME_HASH_INIT, s, strlen(s));
}

static void test_name_hash(void)
{
    /* Published FNV-1a 32 vectors, on bytes folding leaves alone. Frozen: bookmarks store this. */
    CHECK(hash_str("") == 0x811C9DC5u);
    CHECK(hash_str("a") == 0xE40C292Cu);
    CHECK(hash_str("foobar") == 0xBF9CF968u);

    /* ASCII letters fold, always (R43). */
    CHECK(hash_str("FOOBAR") == hash_str("foobar"));
    CHECK(hash_str("OEBPS/Chapter1.XHTML") == hash_str("oebps/chapter1.xhtml"));
    CHECK(hash_str("a") != hash_str("b"));

    /* The neighbours of each end of the range do not fold. */
    CHECK(hash_str("@") != hash_str("`"));
    CHECK(hash_str("[") != hash_str("{"));

    /* No Unicode folding: UTF-8 for E-acute and e-acute stay distinct (R43). */
    CHECK(hash_str("\xC3\x89") != hash_str("\xC3\xA9"));

    /* Continuing across a split gives the same result at every split point. */
    static const char name[] = "OEBPS/Text/Ch01.xhtml";
    const size_t len = sizeof name - 1;
    for (size_t split = 0; split <= len; split++) {
        uint32_t h = bt_name_hash(BT_NAME_HASH_INIT, name, split);
        CHECK(bt_name_hash(h, name + split, len - split) == hash_str(name));
    }
}

int main(void)
{
    test_strerror();
    test_limits();
    test_warnings();
    test_read_gate();
    test_memory_source();
    test_crc32();
    test_name_hash();
    return CHECK_RESULT();
}
