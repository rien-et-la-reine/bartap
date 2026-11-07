/* Finding the central directory (R24, R29, R30, R40). Archives are built by hand, because most of
 * these cases are ones no zip tool writes. Only the end records matter here, so the directory's own
 * bytes are filler. */
#include <string.h>

#include "check.h"
#include "internal.h"

#define SIGNATURE_END 0x06054B50u
#define SIGNATURE_LOCATOR 0x07064B50u
#define SIGNATURE_ZIP64 0x06064B50u
#define END_SIZE 22u
#define LOCATOR_SIZE 20u
#define ZIP64_SIZE 56u
#define COMMENT_MAX 65535u
#define RECORD_MIN 46u

/* Large enough for 65,535 of the smallest possible records. */
static uint8_t archive[3300000];
static uint32_t archive_len;

/* ---- Building archives. Written byte by byte, sharing no code with what is tested ---- */

static void put16(uint32_t at, uint32_t value)
{
    archive[at] = (uint8_t)(value & 0xFFu);
    archive[at + 1u] = (uint8_t)(value >> 8);
}

static void put32(uint32_t at, uint32_t value)
{
    put16(at, value & 0xFFFFu);
    put16(at + 2u, value >> 16);
}

/* Filler that can never be read as a signature. */
static uint32_t add_filler(uint32_t len)
{
    uint32_t at = archive_len;
    memset(archive + at, 0xEE, len);
    archive_len += len;
    return at;
}

static uint32_t add_end_on_disks(uint32_t disk, uint32_t dir_disk, uint32_t count, uint32_t size,
                                 uint32_t offset, uint32_t comment_len)
{
    uint32_t at = add_filler(END_SIZE);
    put32(at, SIGNATURE_END);
    put16(at + 4u, disk);
    put16(at + 6u, dir_disk);
    put16(at + 8u, count);
    put16(at + 10u, count);
    put32(at + 12u, size);
    put32(at + 16u, offset);
    put16(at + 20u, comment_len);
    return at;
}

static uint32_t add_end(uint32_t count, uint32_t size, uint32_t offset, uint32_t comment_len)
{
    return add_end_on_disks(0, 0, count, size, offset, comment_len);
}

/* A directory of 92 bytes at offset 8, room for 2 entries, and an end record with no comment. */
static void build_simple(void)
{
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 0);
}

/* ---- Checking results ---- */

static void check_found(uint32_t count, uint32_t size, uint32_t offset)
{
    bartap_source_t source;
    bartap_directory_t dir;

    CHECK(bartap_source_memory(&source, archive, archive_len) == BARTAP_OK);
    CHECK(bt_dir_init(&dir, &source, NULL, 0) == BARTAP_OK);
    CHECK(dir.source == &source);
    CHECK(dir.raw == NULL);
    CHECK(dir.count == count);
    CHECK(dir.size == size);
    CHECK(dir.offset == offset);
}

/* A failure must leave dir alone. */
static void check_fails(bartap_err_t want)
{
    bartap_source_t source;
    bartap_directory_t dir;
    bartap_directory_t untouched;

    memset(&dir, 0x77, sizeof dir);
    memcpy(&untouched, &dir, sizeof dir);
    CHECK(bartap_source_memory(&source, archive, archive_len) == BARTAP_OK);
    CHECK(bt_dir_init(&dir, &source, NULL, 0) == want);
    CHECK(memcmp(&dir, &untouched, sizeof dir) == 0);
}

/* ---- Finding the end record ---- */

static void test_comments(void)
{
    build_simple();
    check_found(2, 92, 8);

    /* A signature can fall anywhere relative to the scan's chunks, so try every nearby position. */
    for (uint32_t comment = 0; comment < 600u; comment++) {
        archive_len = 0;
        add_filler(100);
        add_end(2, 92, 8, comment);
        add_filler(comment);
        check_found(2, 92, 8);
    }

    /* The longest comment there can be. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, COMMENT_MAX);
    add_filler(COMMENT_MAX);
    check_found(2, 92, 8);
}

static void test_fakes_in_comments(void)
{
    /* Signature alone, with impossible values after it: not usable, so it is skipped. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 100);
    uint32_t comment = add_filler(100);
    put32(comment + 40u, SIGNATURE_END);
    put32(comment + 52u, 0x7FFFFFFFu);
    check_found(2, 92, 8);

    /* Usable values but a comment length that misses the end: the exact match is preferred, even
     * though the scan reaches the fake first. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 100);
    comment = add_filler(100);
    archive_len = comment + 10u;
    add_end(1, 46, 0, 3);
    archive_len = comment + 100u;
    check_found(2, 92, 8);

    /* Usable and exact: a real ambiguity, and the latest wins. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 100);
    comment = add_filler(100);
    archive_len = comment + 10u;
    add_end(1, 46, 0, 100u - 10u - END_SIZE);
    archive_len = comment + 100u;
    check_found(1, 46, 0);
}

/* R30: bytes after the archive do not stop a book opening. */
static void test_padding(void)
{
    for (uint32_t padding = 1; padding < 600u; padding++) {
        archive_len = 0;
        add_filler(100);
        add_end(2, 92, 8, 0);
        add_filler(padding);
        check_found(2, 92, 8);
    }

    /* After a comment too. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 10);
    add_filler(10 + 50);
    check_found(2, 92, 8);

    /* A comment length running past the end of the file, as a cut-off comment would. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 500);
    add_filler(20);
    check_found(2, 92, 8);

    /* A fake with impossible values in the padding does not hide the real one. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 0);
    add_end(1, 0x7FFFFFFFu, 0, 0);
    add_filler(50);
    check_found(2, 92, 8);

    /* With two usable and neither exact, the latest wins. */
    archive_len = 0;
    add_filler(100);
    add_end(1, 46, 0, 0);
    add_end(2, 92, 8, 0);
    add_filler(50);
    check_found(2, 92, 8);

    /* The edge of the scan: a record as far back as a comment could put it, then one further. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 0);
    add_filler(COMMENT_MAX);
    check_found(2, 92, 8);

    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 0);
    add_filler(COMMENT_MAX + 1u);
    check_fails(BARTAP_ERR_CORRUPT);
}

static void test_not_found(void)
{
    archive_len = 0;
    check_fails(BARTAP_ERR_CORRUPT);

    add_filler(END_SIZE - 1u);
    check_fails(BARTAP_ERR_CORRUPT);

    archive_len = 0;
    add_filler(1000);
    check_fails(BARTAP_ERR_CORRUPT);

    /* A signature with too few bytes after it to be a record. */
    archive_len = 0;
    add_filler(100);
    put32(archive_len - 4u, SIGNATURE_END);
    check_fails(BARTAP_ERR_CORRUPT);

    /* The smallest valid archive: an end record and nothing else. */
    archive_len = 0;
    add_end(0, 0, 0, 0);
    check_found(0, 0, 0);
}

/* ---- Values that cannot be right (R29) ---- */

static void test_bad_values(void)
{
    /* The directory has to end at or before the end record. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 0);
    check_found(2, 92, 8);

    archive_len = 0;
    add_filler(100);
    add_end(2, 93, 8, 0);
    check_fails(BARTAP_ERR_CORRUPT);

    archive_len = 0;
    add_filler(100);
    add_end(0, 0, 100, 0);
    check_found(0, 0, 100);

    archive_len = 0;
    add_filler(100);
    add_end(0, 0, 101, 0);
    check_fails(BARTAP_ERR_CORRUPT);

    /* A sum that would overflow to look small. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 0x20u, 0xFFFFFFF0u, 0);
    check_fails(BARTAP_ERR_CORRUPT);

    archive_len = 0;
    add_filler(100);
    add_end(2, 0xFFFFFFF0u, 0x20u, 0);
    check_fails(BARTAP_ERR_CORRUPT);

    /* Every entry takes at least a record's fixed part. */
    archive_len = 0;
    add_filler(100);
    add_end(2, RECORD_MIN * 2u, 0, 0);
    check_found(2, RECORD_MIN * 2u, 0);

    archive_len = 0;
    add_filler(100);
    add_end(2, RECORD_MIN * 2u - 1u, 0, 0);
    check_fails(BARTAP_ERR_CORRUPT);
}

static void test_split_archives(void)
{
    archive_len = 0;
    add_filler(100);
    add_end_on_disks(1, 0, 2, 92, 8, 0);
    check_fails(BARTAP_ERR_UNSUPPORTED);

    archive_len = 0;
    add_filler(100);
    add_end_on_disks(0, 1, 2, 92, 8, 0);
    check_fails(BARTAP_ERR_UNSUPPORTED);

    /* A placeholder disk number needs a ZIP64 record to replace it. */
    archive_len = 0;
    add_filler(100);
    add_end_on_disks(0xFFFF, 0, 2, 92, 8, 0);
    check_fails(BARTAP_ERR_UNSUPPORTED);

    /* Unsupported says more than corrupt, when no candidate is usable. */
    archive_len = 0;
    add_filler(100);
    add_end_on_disks(1, 0, 2, 92, 8, 0);
    add_end(1, 0x7FFFFFFFu, 0, 0);
    add_filler(10);
    check_fails(BARTAP_ERR_UNSUPPORTED);

    /* A usable candidate still wins over one that is not. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 0);
    add_end_on_disks(1, 0, 1, 46, 0, 0);
    add_filler(10);
    check_found(2, 92, 8);
}

/* ---- ZIP64 (R40) ---- */

typedef struct {
    uint32_t record;  /* where the record was put */
    uint32_t locator;
} zip64_at_t;

/* A directory of 92 bytes at 8, then the ZIP64 record and locator, then an end record of
 * placeholders. Cases damage what they need to before the search runs. */
static zip64_at_t build_zip64(void)
{
    zip64_at_t at;

    archive_len = 0;
    add_filler(100);
    at.record = add_filler(ZIP64_SIZE);
    memset(archive + at.record, 0, ZIP64_SIZE);
    put32(at.record, SIGNATURE_ZIP64);
    put32(at.record + 32u, 2);
    put32(at.record + 40u, 92);
    put32(at.record + 48u, 8);
    at.locator = add_filler(LOCATOR_SIZE);
    memset(archive + at.locator, 0, LOCATOR_SIZE);
    put32(at.locator, SIGNATURE_LOCATOR);
    put32(at.locator + 8u, at.record);
    put32(at.locator + 16u, 1);
    add_end(0xFFFF, UINT32_MAX, UINT32_MAX, 0);
    return at;
}

static void test_zip64(void)
{
    zip64_at_t at = build_zip64();
    check_found(2, 92, 8);

    /* Any one placeholder is enough to look. */
    archive_len = at.locator + LOCATOR_SIZE;
    add_end(0xFFFF, 92, 8, 0);
    check_found(2, 92, 8);

    archive_len = at.locator + LOCATOR_SIZE;
    add_end(2, UINT32_MAX, 8, 0);
    check_found(2, 92, 8);

    archive_len = at.locator + LOCATOR_SIZE;
    add_end(2, 92, UINT32_MAX, 0);
    check_found(2, 92, 8);

    archive_len = at.locator + LOCATOR_SIZE;
    add_end_on_disks(0xFFFF, 0xFFFF, 0xFFFF, UINT32_MAX, UINT32_MAX, 0);
    check_found(2, 92, 8);

    /* With no placeholder, the classic values stand and the ZIP64 record is not consulted. */
    archive_len = at.locator + LOCATOR_SIZE;
    add_end(1, 46, 0, 0);
    check_found(1, 46, 0);

    /* Padding works the same way. */
    build_zip64();
    add_filler(40);
    check_found(2, 92, 8);

    /* Values above 32 bits are unsupported (R40). */
    static const uint32_t high_words[] = {36, 44, 52};
    for (size_t i = 0; i < sizeof high_words / sizeof high_words[0]; i++) {
        at = build_zip64();
        put32(at.record + high_words[i], 1);
        check_fails(BARTAP_ERR_UNSUPPORTED);
    }
    at = build_zip64();
    put32(at.locator + 12u, 1);
    check_fails(BARTAP_ERR_UNSUPPORTED);

    /* Split archives, in each field that can say so. */
    at = build_zip64();
    put32(at.locator + 4u, 1);
    check_fails(BARTAP_ERR_UNSUPPORTED);
    at = build_zip64();
    put32(at.locator + 16u, 2);
    check_fails(BARTAP_ERR_UNSUPPORTED);
    at = build_zip64();
    put32(at.record + 16u, 1);
    check_fails(BARTAP_ERR_UNSUPPORTED);
    at = build_zip64();
    put32(at.record + 20u, 1);
    check_fails(BARTAP_ERR_UNSUPPORTED);

    /* A locator that does not lead to a record. */
    at = build_zip64();
    put32(at.record, SIGNATURE_ZIP64 ^ 1u);
    check_fails(BARTAP_ERR_CORRUPT);
    at = build_zip64();
    put32(at.locator + 8u, UINT32_MAX);
    check_fails(BARTAP_ERR_CORRUPT);

    /* An otherwise valid record moved one byte later, so it would overlap the locator. */
    at = build_zip64();
    memmove(archive + at.record + 1u, archive + at.record, ZIP64_SIZE - 1u);
    put32(at.locator + 8u, at.record + 1u);
    check_fails(BARTAP_ERR_CORRUPT);

    /* The directory has to end before the record, not just before the end record. */
    at = build_zip64();
    put32(at.record + 40u, at.record - 8u);
    put32(at.record + 32u, 1);
    check_found(1, at.record - 8u, 8);
    at = build_zip64();
    put32(at.record + 40u, at.record - 8u + 1u);
    put32(at.record + 32u, 1);
    check_fails(BARTAP_ERR_CORRUPT);

    /* Values from the record are checked like any others. */
    at = build_zip64();
    put32(at.record + 32u, 3);
    check_fails(BARTAP_ERR_CORRUPT);

    /* An end record too near the file's start to have a locator before it. */
    archive_len = 0;
    add_filler(LOCATOR_SIZE - 1u);
    add_end(2, UINT32_MAX, 0, 0);
    check_fails(BARTAP_ERR_CORRUPT);
}

/* With no locator, the classic values are real, placeholder or not. */
static void test_placeholder_without_zip64(void)
{
    uint32_t size = 0xFFFFu * RECORD_MIN;

    archive_len = 0;
    add_filler(size);
    add_end(0xFFFF, size, 0, 0);
    check_found(0xFFFF, size, 0);

    /* One byte short of room for them all. */
    archive_len = 0;
    add_filler(size);
    add_end(0xFFFF, size - 1u, 0, 0);
    check_fails(BARTAP_ERR_CORRUPT);

    archive_len = 0;
    add_filler(100);
    add_end(0, UINT32_MAX, 0, 0);
    check_fails(BARTAP_ERR_CORRUPT);

    archive_len = 0;
    add_filler(100);
    add_end(0, 0, UINT32_MAX, 0);
    check_fails(BARTAP_ERR_CORRUPT);
}

/* ---- Raw or none (R24) ---- */

static void test_modes(void)
{
    static uint8_t buf[200];
    bartap_source_t source;
    bartap_directory_t dir;

    archive_len = 0;
    add_filler(100);
    for (uint32_t i = 0; i < 100u; i++) {
        archive[i] = (uint8_t)i;
    }
    add_end(2, 92, 8, 0);
    CHECK(bartap_source_memory(&source, archive, archive_len) == BARTAP_OK);

    /* Fits, with room and exactly. */
    static const uint32_t fits[] = {sizeof buf, 92};
    for (size_t i = 0; i < sizeof fits / sizeof fits[0]; i++) {
        memset(buf, 0, sizeof buf);
        CHECK(bt_dir_init(&dir, &source, buf, fits[i]) == BARTAP_OK);
        CHECK(dir.raw == buf);
        CHECK(memcmp(buf, archive + 8, 92) == 0);
        CHECK(buf[92] == 0);
    }

    /* One byte too small is ignored whole, never used in part. */
    memset(buf, 0, sizeof buf);
    CHECK(bt_dir_init(&dir, &source, buf, 91) == BARTAP_OK);
    CHECK(dir.raw == NULL);
    CHECK(buf[0] == 0 && buf[8] == 0);
    CHECK(dir.size == 92 && dir.offset == 8 && dir.count == 2);

    /* A buffer of no size is only enough for no directory. */
    CHECK(bt_dir_init(&dir, &source, buf, 0) == BARTAP_OK);
    CHECK(dir.raw == NULL);
    CHECK(bt_dir_init(&dir, &source, NULL, 200) == BARTAP_OK);
    CHECK(dir.raw == NULL);

    archive_len = 0;
    add_end(0, 0, 0, 0);
    CHECK(bartap_source_memory(&source, archive, archive_len) == BARTAP_OK);
    CHECK(bt_dir_init(&dir, &source, buf, 0) == BARTAP_OK);
    CHECK(dir.raw == buf);
}

/* ---- I/O failures (R37) ---- */

static int io_reads_left;

static int failing_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)user;
    if (io_reads_left == 0) {
        return 1;
    }
    io_reads_left--;
    memcpy(buf, archive + offset, len);
    return 0;
}

/* Any read an init makes can fail, wherever it falls: the scan, a candidate, ZIP64, the buffer. */
static void check_every_read_can_fail(void)
{
    static uint8_t buf[200];
    bartap_source_t source;
    bartap_directory_t dir;
    bartap_directory_t untouched;
    int reads = 0;

    CHECK(bartap_source_memory(&source, archive, archive_len) == BARTAP_OK);
    source.read = failing_read;
    source.user = NULL;

    /* Count the reads a clean run makes, then fail each one in turn. */
    for (;;) {
        io_reads_left = reads;
        memset(&dir, 0x77, sizeof dir);
        memcpy(&untouched, &dir, sizeof dir);
        bartap_err_t err = bt_dir_init(&dir, &source, buf, sizeof buf);
        if (err == BARTAP_OK) {
            break;
        }
        CHECK(err == BARTAP_ERR_IO);
        CHECK(memcmp(&dir, &untouched, sizeof dir) == 0);
        reads++;
        if (reads > 2000) {
            CHECK(false);
            break;
        }
    }
    CHECK(reads >= 3); /* the scan, the candidate, and at least one more */
}

static void test_io_failures(void)
{
    build_simple();
    check_every_read_can_fail();

    build_zip64();
    check_every_read_can_fail();

    /* The fallback runs the whole scan, and a late failure is still a failure. */
    archive_len = 0;
    add_filler(100);
    add_end(2, 92, 8, 0);
    add_filler(1000);
    check_every_read_can_fail();
}

int main(void)
{
    test_comments();
    test_fakes_in_comments();
    test_padding();
    test_not_found();
    test_bad_values();
    test_split_archives();
    test_zip64();
    test_placeholder_without_zip64();
    test_modes();
    test_io_failures();
    return CHECK_RESULT();
}
