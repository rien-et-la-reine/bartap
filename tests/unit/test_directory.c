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

/* ---- Records. Each test here runs raw and then none, which must not be able to tell (R24) ---- */

#define METHOD_UNSUPPORTED 0xFFu
#define RECORD_SIGNATURE 0x02014B50u

/* The directory mode the record tests run in (R24). Hashed gets a buffer one byte short of the
 * directory, which an index of well-formed records always fits. */
enum { MODE_NONE, MODE_RAW, MODE_HASHED, MODE_COUNT };
static int dir_mode;
static uint32_t dir_start;
static uint32_t dir_size;
static uint32_t record_count;

typedef struct {
    const char *name;
    uint32_t name_len;   /* from strlen unless set */
    uint32_t method;
    uint32_t flags;
    uint32_t crc;
    uint32_t compressed;
    uint32_t uncompressed;
    uint32_t local;
    uint32_t disk;
    const uint8_t *extra;
    uint32_t extra_len;
    uint32_t comment_len;
} record_t;

static void begin_directory(void)
{
    archive_len = 0;
    add_filler(32);
    dir_start = archive_len;
    record_count = 0;
}

static uint32_t add_record(const record_t *r)
{
    uint32_t name_len = r->name_len != 0 ? r->name_len : (uint32_t)strlen(r->name);
    uint32_t at = add_filler(RECORD_MIN);

    memset(archive + at, 0, RECORD_MIN);
    put32(at, RECORD_SIGNATURE);
    put16(at + 8u, r->flags);
    put16(at + 10u, r->method);
    put32(at + 16u, r->crc);
    put32(at + 20u, r->compressed);
    put32(at + 24u, r->uncompressed);
    put16(at + 28u, name_len);
    put16(at + 30u, r->extra_len);
    put16(at + 32u, r->comment_len);
    put16(at + 34u, r->disk);
    put32(at + 42u, r->local);
    memcpy(archive + add_filler(name_len), r->name, name_len);
    if (r->extra_len != 0) {
        memcpy(archive + add_filler(r->extra_len), r->extra, r->extra_len);
    }
    add_filler(r->comment_len);
    record_count++;
    return at;
}

static void end_directory_with_count(uint32_t count)
{
    dir_size = archive_len - dir_start;
    add_end(count, dir_size, dir_start, 0);
}

static void end_directory(void)
{
    end_directory_with_count(record_count);
}

static void open_directory(bartap_directory_t *dir, bartap_source_t *source)
{
    static uint8_t buf[sizeof archive];

    CHECK(bartap_source_memory(source, archive, archive_len) == BARTAP_OK);
    uint32_t size = dir_mode == MODE_RAW ? (uint32_t)sizeof buf : 0u;
    if (dir_mode == MODE_HASHED) {
        /* the directory's size, from an unbuffered look first */
        CHECK(bt_dir_init(dir, source, NULL, 0) == BARTAP_OK);
        size = dir->size != 0 ? dir->size - 1u : 0u;
    }
    CHECK(bt_dir_init(dir, source, dir_mode == MODE_NONE ? NULL : buf, size) == BARTAP_OK);
    /* an empty directory fits any buffer, so it's raw even in hashed mode */
    CHECK((dir->raw != NULL) == (dir_mode == MODE_RAW || (dir_mode == MODE_HASHED && dir->size == 0)));
    /* hashed falls back to none on a bad record, so only its absence elsewhere is certain */
    CHECK(dir_mode == MODE_HASHED || dir->index == NULL);
}

static void test_walk(void)
{
    static const uint8_t comment_free_extra[] = {0x99, 0x99, 0x04, 0x00, 'a', 'b', 'c', 'd'};
    bartap_source_t source;
    bartap_directory_t dir;
    bt_dir_cursor_t cursor;
    bartap_entry_t entry;
    uint32_t hash;
    uint16_t len;

    begin_directory();
    record_t a = {.name = "first.xhtml", .method = 8, .crc = 0x11111111u, .compressed = 10,
                  .uncompressed = 20, .local = 30};
    record_t b = {.name = "second", .method = 0, .crc = 0x22222222u, .compressed = 40,
                  .uncompressed = 40, .local = 50, .extra = comment_free_extra,
                  .extra_len = sizeof comment_free_extra, .comment_len = 7};
    record_t c = {.name = "THIRD", .method = 8, .local = 60};
    uint32_t at_a = add_record(&a);
    uint32_t at_b = add_record(&b);
    uint32_t at_c = add_record(&c);
    end_directory();
    open_directory(&dir, &source);

    bt_dir_begin(&dir, &cursor);
    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_OK);
    CHECK(entry.dir_offset == at_a && entry.method == 8 && entry.crc32 == 0x11111111u);
    CHECK(entry.compressed_size == 10 && entry.uncompressed_size == 20 && entry.local_offset == 30);
    CHECK(len == 11 && hash == bt_name_hash(BT_NAME_HASH_INIT, "first.xhtml", 11));

    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_OK);
    CHECK(entry.dir_offset == at_b && entry.method == 0 && entry.local_offset == 50);
    CHECK(len == 6 && hash == bt_name_hash(BT_NAME_HASH_INIT, "second", 6));

    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_OK);
    CHECK(entry.dir_offset == at_c);
    CHECK(hash == bt_name_hash(BT_NAME_HASH_INIT, "third", 5)); /* folded (R43) */

    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_END);
    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_END);

    /* A new cursor starts over. */
    bt_dir_begin(&dir, &cursor);
    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_OK);
    CHECK(entry.dir_offset == at_a);

    /* A directory with no records. */
    begin_directory();
    end_directory();
    open_directory(&dir, &source);
    bt_dir_begin(&dir, &cursor);
    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_END);
}

static uint32_t count_records(const bartap_directory_t *dir, bartap_err_t want_end)
{
    bt_dir_cursor_t cursor;
    bartap_entry_t entry;
    uint32_t hash;
    uint16_t len;
    uint32_t count = 0;
    bartap_err_t err;

    bt_dir_begin(dir, &cursor);
    while ((err = bt_dir_next(dir, &cursor, &entry, &hash, &len)) == BARTAP_OK) {
        count++;
        CHECK(count <= dir->size / RECORD_MIN);
    }
    CHECK(err == want_end);
    return count;
}

/* A wrong count is a malformation, not corruption: the directory's size decides. */
static void test_wrong_count(void)
{
    bartap_source_t source;
    bartap_directory_t dir;
    record_t r = {.name = "a-name-long-enough-to-leave-room-for-a-higher-count"};

    static const uint32_t counts[] = {0, 1, 2, 3, 4};
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        begin_directory();
        add_record(&r);
        add_record(&r);
        end_directory_with_count(counts[i]);
        open_directory(&dir, &source);
        CHECK(count_records(&dir, BARTAP_END) == 2);
    }

    /* A count too high to fit at all is rejected at init. */
    begin_directory();
    add_record(&r);
    add_record(&r);
    end_directory_with_count((archive_len - dir_start) / RECORD_MIN + 1u);
    check_fails(BARTAP_ERR_CORRUPT);
}

/* Bad structure is corrupt, and stops a walk at that record. */
static void test_bad_records(void)
{
    bartap_source_t source;
    bartap_directory_t dir;
    bt_dir_cursor_t cursor;
    bartap_entry_t entry;
    uint32_t hash;
    uint16_t len;
    record_t r = {.name = "good"};

    /* A bad signature on the second record. */
    begin_directory();
    add_record(&r);
    uint32_t second = add_record(&r);
    end_directory();
    put32(second, RECORD_SIGNATURE ^ 1u);
    open_directory(&dir, &source);
    CHECK(count_records(&dir, BARTAP_ERR_CORRUPT) == 1);

    /* Failure leaves the cursor where it was. */
    bt_dir_begin(&dir, &cursor);
    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_OK);
    uint32_t stuck = cursor.offset;
    CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_ERR_CORRUPT);
    CHECK(cursor.offset == stuck);

    /* Each length running one byte past the directory. */
    static const uint32_t length_fields[] = {28, 30, 32};
    for (size_t i = 0; i < sizeof length_fields / sizeof length_fields[0]; i++) {
        begin_directory();
        uint32_t at = add_record(&r);
        end_directory();
        /* An end record 22 bytes on, so the directory's size is what stops it. */
        put16(at + length_fields[i], (i == 0 ? 4u : 0u) + 1u);
        open_directory(&dir, &source);
        CHECK(count_records(&dir, BARTAP_ERR_CORRUPT) == 0);
    }

    /* Bytes left at the end of the directory, too few to be a record. */
    begin_directory();
    add_record(&r);
    add_filler(RECORD_MIN - 1u);
    end_directory();
    open_directory(&dir, &source);
    CHECK(count_records(&dir, BARTAP_ERR_CORRUPT) == 1);
}

static void test_unreadable_entries(void)
{
    bartap_source_t source;
    bartap_directory_t dir;
    bartap_stream_t stream;
    static uint8_t window[BARTAP_WINDOW_SIZE];
    bartap_entry_t entry;

    /* Stay walkable, marked so that only opening them fails. */
    static const struct {
        uint32_t method;
        uint32_t flags;
        uint32_t disk;
        uint32_t want;
    } cases[] = {
        {0, 0, 0, 0},
        {8, 0, 0, 8},
        {8, 0x0800u, 0, 8},                 /* other flags, such as UTF-8 names, do not matter */
        {8, 1, 0, METHOD_UNSUPPORTED},      /* encrypted */
        {0, 1, 0, METHOD_UNSUPPORTED},
        {12, 0, 0, METHOD_UNSUPPORTED},     /* bzip2 */
        {99, 0, 0, METHOD_UNSUPPORTED},
        {264, 0, 0, METHOD_UNSUPPORTED},    /* would become 8 in a spine item's one byte */
        {256, 0, 0, METHOD_UNSUPPORTED},    /* would become 0 */
        {8, 0, 1, METHOD_UNSUPPORTED},      /* on another disk */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        begin_directory();
        record_t r = {.name = "entry", .method = cases[i].method, .flags = cases[i].flags,
                      .disk = cases[i].disk};
        uint32_t at = add_record(&r);
        add_record(&r);
        end_directory();
        open_directory(&dir, &source);
        CHECK(count_records(&dir, BARTAP_END) == 2);
        CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
        CHECK(entry.method == cases[i].want);
        if (cases[i].want == METHOD_UNSUPPORTED) {
            CHECK(bt_stream_open(&stream, &source, &entry, window) == BARTAP_ERR_UNSUPPORTED);
        }
    }
}

static void put_u64(uint8_t *at, uint32_t low, uint32_t high)
{
    for (uint32_t i = 0; i < 4u; i++) {
        at[i] = (uint8_t)(low >> (i * 8u));
        at[i + 4u] = (uint8_t)(high >> (i * 8u));
    }
}

/* A placeholder's value comes from the extra field, which lists only the placeholders (R40). */
static void test_zip64_records(void)
{
    bartap_source_t source;
    bartap_directory_t dir;
    bartap_entry_t entry;
    uint8_t extra[64];
    uint32_t at;

    memset(extra, 0, sizeof extra);
    extra[0] = 0x01;
    extra[2] = 28;
    put_u64(extra + 4, 111, 0);
    put_u64(extra + 12, 222, 0);
    put_u64(extra + 20, 333, 0);
    record_t all = {.name = "n", .method = 8, .uncompressed = UINT32_MAX, .compressed = UINT32_MAX,
                    .local = UINT32_MAX, .extra = extra, .extra_len = 32};
    extra[2] = 28;
    begin_directory();
    at = add_record(&all);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.uncompressed_size == 111 && entry.compressed_size == 222);
    CHECK(entry.local_offset == 333 && entry.method == 8);

    /* Each alone: the first value in the field belongs to whichever is a placeholder. */
    uint8_t one[12];
    memset(one, 0, sizeof one);
    one[0] = 0x01;
    one[2] = 8;
    put_u64(one + 4, 444, 0);
    record_t only_size = {.name = "n", .uncompressed = UINT32_MAX, .compressed = 5, .local = 6,
                          .extra = one, .extra_len = sizeof one};
    begin_directory();
    at = add_record(&only_size);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.uncompressed_size == 444 && entry.compressed_size == 5 && entry.local_offset == 6);

    record_t only_compressed = {.name = "n", .uncompressed = 5, .compressed = UINT32_MAX,
                                .local = 6, .extra = one, .extra_len = sizeof one};
    begin_directory();
    at = add_record(&only_compressed);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.uncompressed_size == 5 && entry.compressed_size == 444 && entry.local_offset == 6);

    record_t only_local = {.name = "n", .uncompressed = 5, .compressed = 6, .local = UINT32_MAX,
                           .extra = one, .extra_len = sizeof one};
    begin_directory();
    at = add_record(&only_local);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.uncompressed_size == 5 && entry.compressed_size == 6 && entry.local_offset == 444);

    /* After another extra field. */
    uint8_t two[20];
    memset(two, 0, sizeof two);
    two[0] = 0x99;
    two[1] = 0x99;
    two[2] = 4;
    memset(two + 4, 0xFF, 4); /* would not parse as a field, if it were not skipped */
    two[8] = 0x01;
    two[10] = 8;
    put_u64(two + 12, 555, 0);
    record_t later = {.name = "n", .method = 8, .uncompressed = UINT32_MAX, .extra = two,
                      .extra_len = sizeof two};
    begin_directory();
    at = add_record(&later);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.uncompressed_size == 555 && entry.method == 8);

    /* Above 32 bits: that entry cannot be read, but the walk goes on. */
    put_u64(one + 4, 1, 1);
    record_t big = {.name = "n", .method = 8, .uncompressed = UINT32_MAX, .extra = one,
                    .extra_len = sizeof one};
    begin_directory();
    at = add_record(&big);
    add_record(&(record_t){.name = "next", .method = 8});
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.method == METHOD_UNSUPPORTED);
    CHECK(count_records(&dir, BARTAP_END) == 2);

    /* A disk number placeholder, on the first disk and then not. */
    uint8_t disk[8];
    memset(disk, 0, sizeof disk);
    disk[0] = 0x01;
    disk[2] = 4;
    record_t on_disk = {.name = "n", .method = 8, .disk = 0xFFFF, .extra = disk,
                        .extra_len = sizeof disk};
    begin_directory();
    at = add_record(&on_disk);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.method == 8);

    disk[4] = 1;
    begin_directory();
    at = add_record(&on_disk);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.method == METHOD_UNSUPPORTED);

    /* A placeholder with nothing to replace it, in each way that can happen. */
    record_t none = {.name = "n", .uncompressed = UINT32_MAX};
    begin_directory();
    at = add_record(&none);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_ERR_CORRUPT);

    uint8_t other[8];
    memset(other, 0, sizeof other);
    other[0] = 0x99;
    other[2] = 4;
    record_t no_zip64 = {.name = "n", .uncompressed = UINT32_MAX, .extra = other,
                         .extra_len = sizeof other};
    begin_directory();
    at = add_record(&no_zip64);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_ERR_CORRUPT);

    /* A field too short for its placeholders. */
    one[2] = 7;
    record_t short_field = {.name = "n", .uncompressed = UINT32_MAX, .extra = one,
                            .extra_len = sizeof one};
    begin_directory();
    at = add_record(&short_field);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_ERR_CORRUPT);

    /* A field that claims more than the extra data holds. */
    one[2] = 9;
    begin_directory();
    at = add_record(&short_field);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_ERR_CORRUPT);

    /* A placeholder disk number with no room for its value. */
    disk[2] = 3;
    begin_directory();
    at = add_record(&on_disk);
    end_directory();
    open_directory(&dir, &source);
    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_ERR_CORRUPT);
}

/* Offsets come from bookmarks and image records, so any value can arrive (R9, R29). */
static void test_untrusted_offsets(void)
{
    bartap_source_t source;
    bartap_directory_t dir;
    bartap_entry_t entry;
    char name[16];
    uint16_t len;
    record_t r = {.name = "good", .method = 8};

    begin_directory();
    uint32_t at = add_record(&r);
    add_record(&r);
    end_directory();
    open_directory(&dir, &source);

    CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_OK);
    CHECK(entry.dir_offset == at);

    uint32_t end = dir.offset + dir.size;
    const uint32_t bad[] = {0, dir.offset - 1u, at + 1u, end, end + 1u, end - 1u,
                            end - RECORD_MIN + 1u, UINT32_MAX};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(bt_dir_entry(&dir, bad[i], &entry) == BARTAP_ERR_CORRUPT);
        CHECK(bt_dir_name(&dir, bad[i], name, sizeof name, &len) == BARTAP_ERR_CORRUPT);
    }
}

static void test_names(void)
{
    bartap_source_t source;
    bartap_directory_t dir;
    char name[16];
    uint16_t len;

    begin_directory();
    uint32_t at = add_record(&(record_t){.name = "MixedCase", .extra_len = 0, .comment_len = 3});
    uint32_t empty = add_record(&(record_t){.name = ""});
    end_directory();
    open_directory(&dir, &source);

    /* The real bytes, not folded (R22), with room and exactly. */
    static const uint32_t caps[] = {sizeof name, 9};
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        memset(name, 0x55, sizeof name);
        len = 0;
        CHECK(bt_dir_name(&dir, at, name, caps[i], &len) == BARTAP_OK);
        CHECK(len == 9 && memcmp(name, "MixedCase", 9) == 0);
        CHECK((uint8_t)name[9] == 0x55);
    }

    /* Too small says how much is needed, and copies nothing. */
    memset(name, 0x55, sizeof name);
    len = 0;
    CHECK(bt_dir_name(&dir, at, name, 8, &len) == BARTAP_ERR_BUFFER_TOO_SMALL);
    CHECK(len == 9 && (uint8_t)name[0] == 0x55);

    /* A name of no length needs no room. */
    len = 99;
    CHECK(bt_dir_name(&dir, empty, NULL, 0, &len) == BARTAP_OK);
    CHECK(len == 0);
}

static void check_find(const bartap_directory_t *dir, const char *name, bartap_err_t want,
                       uint32_t want_offset)
{
    bartap_entry_t entry;

    entry.dir_offset = 0;
    CHECK(bt_dir_find(dir, name, (uint16_t)strlen(name), &entry) == want);
    if (want == BARTAP_OK) {
        CHECK(entry.dir_offset == want_offset);
    }
}

static void test_find(void)
{
    bartap_source_t source;
    bartap_directory_t dir;

    begin_directory();
    uint32_t first = add_record(&(record_t){.name = "OEBPS/Text.xhtml"});
    uint32_t container = add_record(&(record_t){.name = "META-INF/container.xml"});
    uint32_t duplicate = add_record(&(record_t){.name = "oebps/text.XHTML"});
    uint32_t collision_a = add_record(&(record_t){.name = "abm3vu"});
    uint32_t collision_b = add_record(&(record_t){.name = "ab3tea"});
    uint32_t accent = add_record(&(record_t){.name = "caf\xC3\x89"});
    uint32_t at_sign = add_record(&(record_t){.name = "[a]@"});
    end_directory();
    open_directory(&dir, &source);
    CHECK((dir.index != NULL) == (dir_mode == MODE_HASHED));

    check_find(&dir, "META-INF/container.xml", BARTAP_OK, container);
    check_find(&dir, "meta-inf/CONTAINER.XML", BARTAP_OK, container); /* R43 */

    /* First in the directory wins (R44), however the name is cased. */
    check_find(&dir, "OEBPS/Text.xhtml", BARTAP_OK, first);
    check_find(&dir, "oebps/text.XHTML", BARTAP_OK, first);
    CHECK(first != duplicate);

    /* These two have the same hash and length: only the name tells them apart. */
    CHECK(bt_name_hash(BT_NAME_HASH_INIT, "abm3vu", 6) ==
          bt_name_hash(BT_NAME_HASH_INIT, "ab3tea", 6));
    check_find(&dir, "abm3vu", BARTAP_OK, collision_a);
    check_find(&dir, "ab3tea", BARTAP_OK, collision_b);

    /* Only ASCII letters fold. */
    check_find(&dir, "CAF\xC3\x89", BARTAP_OK, accent);
    check_find(&dir, "caf\xC3\xA9", BARTAP_END, 0);
    check_find(&dir, "[a]@", BARTAP_OK, at_sign);
    check_find(&dir, "{a]@", BARTAP_END, 0);
    check_find(&dir, "[a]`", BARTAP_END, 0);

    check_find(&dir, "OEBPS/Text.xhtm", BARTAP_END, 0);
    check_find(&dir, "OEBPS/Text.xhtmlx", BARTAP_END, 0);
    check_find(&dir, "", BARTAP_END, 0);

    /* By hash and length alone, as spine items match (R22, R41): the first of a colliding pair,
     * the first of names that differ only in case, nothing for a length that differs. */
    bartap_entry_t entry;
    CHECK(bt_dir_find_hash(&dir, bt_name_hash(BT_NAME_HASH_INIT, "ab3tea", 6), 6, &entry) == BARTAP_OK);
    CHECK(entry.dir_offset == collision_a);
    CHECK(bt_dir_find_hash(&dir, bt_name_hash(BT_NAME_HASH_INIT, "oebps/text.xhtml", 16), 16, &entry) ==
          BARTAP_OK);
    CHECK(entry.dir_offset == first);
    CHECK(bt_dir_find_hash(&dir, bt_name_hash(BT_NAME_HASH_INIT, "ab3tea", 6), 7, &entry) == BARTAP_END);

    /* The hash matches and the name does not: the walk must carry on to the real one. */
    begin_directory();
    add_record(&(record_t){.name = "abm3vu"});
    collision_b = add_record(&(record_t){.name = "ab3tea"});
    end_directory();
    open_directory(&dir, &source);
    check_find(&dir, "ab3tea", BARTAP_OK, collision_b);

    begin_directory();
    add_record(&(record_t){.name = "abm3vu"});
    end_directory();
    open_directory(&dir, &source);
    check_find(&dir, "ab3tea", BARTAP_END, 0);

    /* The same hash and length, first differing past the chunk a name is compared in. */
    char long_a[76];
    char long_b[76];
    memset(long_a, 'p', 70);
    memcpy(long_a + 70, "irjfa", 6);
    memset(long_b, 'p', 70);
    memcpy(long_b + 70, "1pfha", 6);
    CHECK(bt_name_hash(BT_NAME_HASH_INIT, long_a, 75) ==
          bt_name_hash(BT_NAME_HASH_INIT, long_b, 75));
    begin_directory();
    add_record(&(record_t){.name = long_a});
    end_directory();
    open_directory(&dir, &source);
    check_find(&dir, long_b, BARTAP_END, 0);

    /* The same hash, and the shorter name is the start of the longer: only length tells them. */
    CHECK(bt_name_hash(BT_NAME_HASH_INIT, "7hghaaa", 7) ==
          bt_name_hash(BT_NAME_HASH_INIT, "7hghaaa35", 9));
    begin_directory();
    add_record(&(record_t){.name = "7hghaaa35"});
    end_directory();
    open_directory(&dir, &source);
    check_find(&dir, "7hghaaa", BARTAP_END, 0);

    /* A bad record before the name is a failure, not a miss. */
    begin_directory();
    uint32_t bad = add_record(&(record_t){.name = "other"});
    add_record(&(record_t){.name = "wanted"});
    end_directory();
    put32(bad, RECORD_SIGNATURE ^ 1u);
    open_directory(&dir, &source);
    check_find(&dir, "wanted", BARTAP_ERR_CORRUPT, 0);

    /* An empty directory holds nothing. */
    begin_directory();
    end_directory();
    open_directory(&dir, &source);
    check_find(&dir, "anything", BARTAP_END, 0);
}

/* Hashed (R24): an index of every record, in directory order, when the directory doesn't fit but
 * the index does. Never partial: one entry short of room, or a bad record, leaves none. */
static void test_index(void)
{
    static uint8_t buf[4096];
    bartap_source_t source;
    bartap_directory_t dir;
    uint32_t offsets[5];
    static const char *const names[5] = {"mimetype", "a/b.xhtml", "C.png", "a/b.xhtml", ""};

    begin_directory();
    for (int i = 0; i < 5; i++) {
        offsets[i] = add_record(&(record_t){.name = names[i]});
    }
    end_directory();
    CHECK(bartap_source_memory(&source, archive, archive_len) == BARTAP_OK);

    /* exactly room for five entries */
    memset(buf, 0xAA, sizeof buf);
    CHECK(bt_dir_init(&dir, &source, buf, 5u * BARTAP_DIR_INDEX_ENTRY) == BARTAP_OK);
    CHECK(dir.raw == NULL && dir.index == buf && dir.index_count == 5);
    for (int i = 0; i < 5; i++) {
        const uint8_t *e = buf + (uint32_t)i * BARTAP_DIR_INDEX_ENTRY;
        uint32_t len = (uint32_t)strlen(names[i]);
        CHECK(bt_get_le32(e) == bt_name_hash(BT_NAME_HASH_INIT, names[i], len));
        CHECK(bt_get_le16(e + 4) == len);
        CHECK(bt_get_le32(e + 6) == offsets[i]);
    }
    /* nothing written past it */
    CHECK(buf[5u * BARTAP_DIR_INDEX_ENTRY] == 0xAA);
    /* the first of a duplicate wins, as in a walk (R44) */
    bartap_entry_t entry;
    CHECK(bt_dir_find(&dir, "A/B.xhtml", 9, &entry) == BARTAP_OK && entry.dir_offset == offsets[1]);
    CHECK(bt_dir_find(&dir, "c.PNG", 5, &entry) == BARTAP_OK && entry.dir_offset == offsets[2]);
    CHECK(bt_dir_find(&dir, "", 0, &entry) == BARTAP_OK && entry.dir_offset == offsets[4]);
    CHECK(bt_dir_find(&dir, "nope", 4, &entry) == BARTAP_END);

    /* one byte short of five entries: no index at all */
    CHECK(bt_dir_init(&dir, &source, buf, 5u * BARTAP_DIR_INDEX_ENTRY - 1u) == BARTAP_OK);
    CHECK(dir.raw == NULL && dir.index == NULL);
    CHECK(bt_dir_find(&dir, "C.png", 5, &entry) == BARTAP_OK && entry.dir_offset == offsets[2]);

    /* a bad record: none, and lookups fail as they would have without the buffer */
    put32(offsets[3], RECORD_SIGNATURE ^ 1u);
    CHECK(bt_dir_init(&dir, &source, buf, dir_size - 1u) == BARTAP_OK);
    CHECK(dir.raw == NULL && dir.index == NULL);
    CHECK(bt_dir_find(&dir, "C.png", 5, &entry) == BARTAP_OK);
    CHECK(bt_dir_find(&dir, "nope", 4, &entry) == BARTAP_ERR_CORRUPT);
    put32(offsets[3], RECORD_SIGNATURE);

    /* the index is read after init, not the card: a miss reads nothing, a candidate reads its name
     * and a hash-only hit its record, both now zeros */
    CHECK(bt_dir_init(&dir, &source, buf, dir_size - 1u) == BARTAP_OK);
    CHECK(dir.index == buf);
    memset(archive + dir_start, 0, dir_size);
    CHECK(bt_dir_find(&dir, "nope", 4, &entry) == BARTAP_END);
    CHECK(bt_dir_find_hash(&dir, 0x12345678u, 3, &entry) == BARTAP_END);
    CHECK(bt_dir_find(&dir, "C.png", 5, &entry) == BARTAP_END);
    CHECK(bt_dir_find_hash(&dir, bt_name_hash(BT_NAME_HASH_INIT, "C.png", 5), 5, &entry) == BARTAP_ERR_CORRUPT);
}


/* Long names cross the chunks they are read in, so try every length around the edges. */
static void test_long_names(void)
{
    static char long_name[66000];
    static char copy[66000];
    bartap_source_t source;
    bartap_directory_t dir;
    bt_dir_cursor_t cursor;
    bartap_entry_t entry;
    uint32_t hash;
    uint16_t len;

    static const uint32_t lens[] = {1, 63, 64, 65, 127, 128, 129, 1000, 65535};
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        uint32_t n = lens[i];
        for (uint32_t j = 0; j < n; j++) {
            long_name[j] = (char)('a' + j % 26u);
        }
        begin_directory();
        uint32_t at = add_record(&(record_t){.name = long_name, .name_len = n});
        end_directory();
        open_directory(&dir, &source);

        bt_dir_begin(&dir, &cursor);
        CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_OK);
        CHECK(len == n && hash == bt_name_hash(BT_NAME_HASH_INIT, long_name, n));

        CHECK(bt_dir_name(&dir, at, copy, sizeof copy, &len) == BARTAP_OK);
        CHECK(len == n && memcmp(copy, long_name, n) == 0);

        entry.dir_offset = 0;
        CHECK(bt_dir_find(&dir, long_name, (uint16_t)n, &entry) == BARTAP_OK);
        CHECK(entry.dir_offset == at);

        /* Differing only in the last byte, in a case the hash cannot see: a real collision is
         * covered above, so this one only proves the compare reaches the end of a long name. */
        long_name[n - 1u] = (char)(long_name[n - 1u] == 'x' ? 'y' : 'x');
        CHECK(bt_dir_find(&dir, long_name, (uint16_t)n, &entry) == BARTAP_END);
    }
}

/* In none mode each read can fail. Failures are passed up, never taken for a missing entry. */
static void test_walk_io_failures(void)
{
    bartap_source_t source;
    bartap_directory_t dir;
    bt_dir_cursor_t cursor;
    bartap_entry_t entry;
    char name[8];
    uint32_t hash;
    uint16_t len;

    uint8_t extra[12];
    memset(extra, 0, sizeof extra);
    extra[0] = 0x01;
    extra[2] = 8;
    begin_directory();
    uint32_t at = add_record(&(record_t){.name = "abc", .uncompressed = UINT32_MAX, .extra = extra,
                                         .extra_len = sizeof extra});
    end_directory();

    CHECK(bartap_source_memory(&source, archive, archive_len) == BARTAP_OK);
    CHECK(bt_dir_init(&dir, &source, NULL, 0) == BARTAP_OK);
    source.read = failing_read;
    source.user = NULL;

    /* The record, its extra field's header, its value and its name are each one read. */
    for (int ok_reads = 0; ok_reads < 4; ok_reads++) {
        io_reads_left = ok_reads;
        bt_dir_begin(&dir, &cursor);
        CHECK(bt_dir_next(&dir, &cursor, &entry, &hash, &len) == BARTAP_ERR_IO);
        CHECK(cursor.offset == at);
        io_reads_left = ok_reads;
        CHECK(bt_dir_find(&dir, "abc", 3, &entry) == BARTAP_ERR_IO);
    }

    /* The name compare's read, after the walk's have passed. */
    io_reads_left = 4;
    CHECK(bt_dir_find(&dir, "abc", 3, &entry) == BARTAP_ERR_IO);

    for (int ok_reads = 0; ok_reads < 3; ok_reads++) {
        io_reads_left = ok_reads;
        CHECK(bt_dir_entry(&dir, at, &entry) == BARTAP_ERR_IO);
    }
    for (int ok_reads = 0; ok_reads < 4; ok_reads++) {
        io_reads_left = ok_reads;
        CHECK(bt_dir_name(&dir, at, name, sizeof name, &len) == BARTAP_ERR_IO);
    }

    /* With enough reads, all of it works. */
    io_reads_left = 100;
    CHECK(bt_dir_find(&dir, "abc", 3, &entry) == BARTAP_OK);
    CHECK(entry.dir_offset == at);
}

int main(void)
{
    for (dir_mode = 0; dir_mode < MODE_COUNT; dir_mode++) {
        test_walk();
        test_wrong_count();
        test_bad_records();
        test_unreadable_entries();
        test_zip64_records();
        test_untrusted_offsets();
        test_names();
        test_find();
        test_long_names();
    }
    test_walk_io_failures();

    test_comments();
    test_fakes_in_comments();
    test_padding();
    test_not_found();
    test_bad_values();
    test_split_archives();
    test_zip64();
    test_placeholder_without_zip64();
    test_modes();
    test_index();
    test_io_failures();
    return CHECK_RESULT();
}
