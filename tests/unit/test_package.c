/* Package layer (R22-R25, R30, R41, R43, R44): href resolution, bartap_open on built EPUBs in every
 * directory mode and spine layout (which must agree, R24, R41), metadata, the spine, missing and
 * duplicate entries, packaging faults, the three-walk bound, and resources and verify reached
 * through a book. */
#include "check.h"
#include "epub_build.h"
#include "internal.h"

#define SPINE_MAX 400u

static epub_t epub;
static epub_str_t chapter_text;
static uint8_t window[BARTAP_WINDOW_SIZE];
static uint8_t dir_buf[1u << 20];
static bartap_open_work_t work;
static uint8_t index_buf[1u << 20];
static bartap_book_t book;
static bartap_book_t other;
static bartap_book_t third;
static bartap_spine_item_t spine[SPINE_MAX];
static bartap_spine_item_t other_spine[SPINE_MAX];
static bartap_spine_item_t third_spine[SPINE_MAX];
static bartap_resource_t res;
static bartap_verify_t verify;
static uint8_t out[1u << 18];

/* ---- A counting source, for the walk bound (R23) ---- */

static const uint8_t *source_bytes;
static uint32_t reads_at_dir_start;
static uint32_t dir_start;

static int counting_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)user;
    if (offset == dir_start) {
        reads_at_dir_start++;
    }
    memcpy(buf, source_bytes + offset, len);
    return 0;
}

static bartap_source_t source;

/* Opens the built archive with a directory buffer of buf_size bytes, NULL for none (R24). */
static bartap_err_t open_with(bartap_book_t *b, bartap_spine_item_t *s, uint16_t cap, uint8_t *buf,
                              uint32_t buf_size)
{
    CHECK(bartap_source_memory(&source, epub.bytes, epub.len) == BARTAP_OK);
    source.read = counting_read;
    source_bytes = epub.bytes;
    dir_start = epub.dir_offset;
    reads_at_dir_start = 0;
    return bartap_open(b, &source, s, cap, buf, buf_size, &work, window, NULL, NULL);
}

/* raw picks raw or none. */
static bartap_err_t open_as(bartap_book_t *b, bartap_spine_item_t *s, uint16_t cap, bool raw)
{
    return open_with(b, s, cap, raw ? dir_buf : NULL, raw ? sizeof dir_buf : 0);
}

static bool same_item(const bartap_spine_item_t *a, const bartap_spine_item_t *b)
{
    return a->path_hash == b->path_hash && a->dir_offset == b->dir_offset &&
           a->local_offset == b->local_offset && a->compressed_size == b->compressed_size &&
           a->uncompressed_size == b->uncompressed_size && a->crc32 == b->crc32 &&
           a->path_len == b->path_len && a->method == b->method && a->flags == b->flags;
}

/* Field by field: the struct has padding, which holds whatever was there before. */
static bool same_entry(const bartap_entry_t *x, const bartap_entry_t *y)
{
    return x->dir_offset == y->dir_offset && x->local_offset == y->local_offset &&
           x->compressed_size == y->compressed_size && x->uncompressed_size == y->uncompressed_size &&
           x->crc32 == y->crc32 && x->method == y->method;
}

/* Everything a caller can see of two books, and every item's entry, whatever their layouts. */
static void same_book(const bartap_book_t *a, const bartap_book_t *b)
{
    CHECK(a->warnings.bits == b->warnings.bits);
    CHECK(a->spine_count == b->spine_count && a->spine_required == b->spine_required);
    CHECK(a->nav_dir_offset == b->nav_dir_offset && a->ncx_dir_offset == b->ncx_dir_offset);
    CHECK(a->cover_dir_offset == b->cover_dir_offset && a->book_hash == b->book_hash);
    CHECK(a->title_len == b->title_len && memcmp(a->title, b->title, a->title_len) == 0);
    CHECK(a->creator_len == b->creator_len && memcmp(a->creator, b->creator, a->creator_len) == 0);
    CHECK(a->creator_count == b->creator_count);
    CHECK(a->identifier_len == b->identifier_len &&
          memcmp(a->identifier, b->identifier, a->identifier_len) == 0);
    for (uint16_t i = 0; i < a->spine_count && i < b->spine_count; i++) {
        uint32_t hash[2];
        uint16_t len[2];
        bt_spine_path(a, i, &hash[0], &len[0]);
        bt_spine_path(b, i, &hash[1], &len[1]);
        CHECK(hash[0] == hash[1] && len[0] == len[1]);
        CHECK((bt_spine_flags(a, i) & BT_SPINE_NONLINEAR) == (bt_spine_flags(b, i) & BT_SPINE_NONLINEAR));
        bartap_entry_t e[2];
        bartap_err_t err[2] = {bt_spine_entry(a, i, &e[0]), bt_spine_entry(b, i, &e[1])};
        CHECK(err[0] == err[1]);
        CHECK(err[0] != BARTAP_OK || same_entry(&e[0], &e[1]));
    }
}

/* Opens in every mode and checks they agree on everything; the raw-mode book is left in book. Also
 * hashed, with a buffer one byte short of the directory, and compact, with a third of the slots
 * (R24, R41). */
static bartap_err_t open_both(uint16_t cap)
{
    bartap_err_t none = open_as(&other, other_spine, cap, false);
    CHECK(other.directory.raw == NULL && other.directory.index == NULL);
    uint32_t short_size = other.directory.size > 1u ? other.directory.size - 1u : 0u;
    uint16_t compact_cap = (uint16_t)((other.spine_count + 2u) / 3u);

    /* hashed, full slots */
    bartap_err_t hashed = open_with(&third, third_spine, cap, index_buf, short_size);
    CHECK(hashed == none);
    if (hashed == BARTAP_OK) {
        CHECK(third.directory.raw == NULL && third.directory.index != NULL);
        same_book(&other, &third);
        for (uint16_t i = 0; i < other.spine_count; i++) {
            CHECK(same_item(&other_spine[i], &third_spine[i]));
        }
    }
    /* compact, with and without an index */
    for (int indexed = 0; indexed < 2; indexed++) {
        bartap_err_t compact = open_with(&third, third_spine, compact_cap, indexed ? index_buf : NULL,
                                         indexed ? short_size : 0);
        CHECK(compact == none);
        if (compact == BARTAP_OK) {
            CHECK(third.spine_compact == (other.spine_count > compact_cap));
            same_book(&other, &third);
        }
    }

    bartap_err_t raw = open_as(&book, spine, cap, true);
    CHECK(none == raw);
    if (none != BARTAP_OK || raw != BARTAP_OK) {
        CHECK(other.warnings.bits == book.warnings.bits);
        return raw;
    }
    CHECK(book.directory.raw != NULL);
    same_book(&other, &book);
    for (uint16_t i = 0; i < book.spine_count; i++) {
        CHECK(same_item(&other_spine[i], &spine[i]));
    }
    return raw;
}

static const epub_entry_t *entry_named(const char *name)
{
    for (uint32_t i = 0; i < epub.count; i++) {
        if (strlen(name) == epub.entries[i].name_len &&
            memcmp(name, epub.entries[i].name, epub.entries[i].name_len) == 0) {
            return &epub.entries[i];
        }
    }
    return NULL;
}

/* Reads a resource to its end, returning the final result. */
static bartap_err_t read_all(uint32_t *total)
{
    bartap_err_t err;
    uint32_t got;
    *total = 0;
    while ((err = bartap_resource_read(&res, out + *total, 4096, &got)) == BARTAP_OK) {
        *total += got;
    }
    return err;
}

/* ---- Paths (R43) ---- */

static void check_resolve(int line, const char *base, const char *href, uint32_t cap,
                          bartap_err_t want, const char *want_path)
{
    char path[512];
    uint16_t len = 0xFFFF;
    bartap_err_t err = bt_href_resolve(base, (uint32_t)strlen(base), href, (uint32_t)strlen(href),
                                       path, cap, &len);
    if (err != want || (want == BARTAP_OK &&
                        (len != strlen(want_path) || memcmp(path, want_path, len) != 0))) {
        printf("%s:%d: resolve(%s, %s) gave %s [%.*s]\n", __FILE__, line, base, href,
               bartap_strerror(err), err == BARTAP_OK ? (int)len : 0, path);
        check_failures++;
    }
}

#define RESOLVE(base, href, want_path) check_resolve(__LINE__, base, href, 255, BARTAP_OK, want_path)
#define REFUSE(base, href, cap, want) check_resolve(__LINE__, base, href, cap, want, "")

static void test_resolve(void)
{
    static const char opf[] = "OEBPS/content.opf";
    RESOLVE(opf, "text/ch1.xhtml", "OEBPS/text/ch1.xhtml");
    RESOLVE(opf, "../a/./b/../c.xhtml", "a/c.xhtml");
    RESOLVE(opf, "../../../x", "x"); /* never above the root (R29) */
    RESOLVE(opf, "/root.xhtml", "root.xhtml");
    RESOLVE(opf, "ch%201.xhtml#frag?q", "OEBPS/ch 1.xhtml");
    RESOLVE(opf, "ch.xhtml?q#f", "OEBPS/ch.xhtml");
    RESOLVE(opf, "%41%62", "OEBPS/Ab"); /* decoded once: */
    RESOLVE(opf, "%2541", "OEBPS/%41");
    RESOLVE(opf, "100%zz%4", "OEBPS/100%zz%4");
    RESOLVE(opf, "a%2", "OEBPS/a%2");
    RESOLVE(opf, "dir/a:b", "OEBPS/dir/a:b"); /* a colon after a '/' is no scheme */
    RESOLVE(opf, "#only", opf);
    RESOLVE(opf, "", opf);
    RESOLVE(opf, "a//b/", "OEBPS/a/b/");
    RESOLVE(opf, "a/..", "OEBPS/");
    RESOLVE(opf, "%2E%2E/x", "OEBPS/../x"); /* an encoded dot segment is a name, not a step */
    RESOLVE("noslash", "x.xhtml", "x.xhtml");
    RESOLVE("", "META-INF/container.xml", "META-INF/container.xml");
    RESOLVE("a/b/", "c", "a/b/c");

    REFUSE(opf, "a%2Fb", 255, BARTAP_ERR_CORRUPT);
    REFUSE(opf, "a%2fb", 255, BARTAP_ERR_CORRUPT);
    REFUSE(opf, "http://example.com/a", 255, BARTAP_ERR_UNSUPPORTED);
    REFUSE(opf, "data:image/png;base64,xx", 255, BARTAP_ERR_UNSUPPORTED);
    REFUSE(opf, "a:b/c", 255, BARTAP_ERR_UNSUPPORTED);
    REFUSE(opf, "mailto:x", 255, BARTAP_ERR_UNSUPPORTED);

    /* cap is exact: "OEBPS/abcd" is 10 bytes. */
    check_resolve(__LINE__, opf, "abcd", 10, BARTAP_OK, "OEBPS/abcd");
    REFUSE(opf, "abcd", 9, BARTAP_ERR_BUFFER_TOO_SMALL);
    REFUSE(opf, "abc/", 9, BARTAP_ERR_BUFFER_TOO_SMALL);
    REFUSE(opf, "x", 5, BARTAP_ERR_BUFFER_TOO_SMALL); /* the base's directory alone is too long */
    REFUSE(opf, "#f", 16, BARTAP_ERR_BUFFER_TOO_SMALL);
    check_resolve(__LINE__, opf, "#f", 17, BARTAP_OK, opf);
}

/* ---- Books ---- */

/* The usual EPUB 3: an OPF in a folder, chapters in a subfolder, nav and cover image. */
static void build_epub3(void)
{
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "OEBPS/content.opf");
    epub_add_opf(&epub, "OEBPS/content.opf", "uid",
                 "<dc:identifier id=\"other\">urn:other</dc:identifier>"
                 "<dc:title>\n  Pride and   Prejudice \n</dc:title>"
                 "<dc:creator>Jane Austen</dc:creator><dc:creator>Someone Else</dc:creator>"
                 "<dc:language>en</dc:language><dc:identifier id=\"uid\">urn:isbn:123</dc:identifier>",
                 "<item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"/>"
                 "<item id=\"c1\" href=\"text/ch%201.xhtml\" media-type=\"application/xhtml+xml\"/>"
                 "<item id=\"c2\" href=\"../OEBPS/./text/ch2.xhtml#frag\" media-type=\"application/xhtml+xml\"/>"
                 "<item id=\"img\" href=\"img/cover.jpg\" properties=\"cover-image\" media-type=\"image/jpeg\"/>",
                 "<itemref idref=\"c1\"/><itemref idref=\"c2\" linear=\"no\"/><itemref idref=\"nav\"/>",
                 NULL);
    epub_add(&epub, "OEBPS/nav.xhtml", epub_chapter(&chapter_text, "nav"), EPUB_DEFLATED);
    static char big[100000];
    memset(big, 'x', sizeof big - 1u);
    epub_add(&epub, "OEBPS/text/ch 1.xhtml", epub_chapter(&chapter_text, big), EPUB_DEFLATED);
    epub_add(&epub, "OEBPS/text/ch2.xhtml", epub_chapter(&chapter_text, "two"), EPUB_STORED);
    epub_add(&epub, "OEBPS/img/cover.jpg", "JPEG bytes", EPUB_STORED);
    epub_finish(&epub);
}

static void test_epub3(void)
{
    bartap_metadata_t m;
    uint16_t count;
    uint32_t required;
    bool linear;
    uint32_t cover;
    uint32_t total;

    build_epub3();
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(bartap_book_warnings(&book) == 0);

    CHECK(bartap_book_metadata(&book, &m) == BARTAP_OK);
    CHECK(m.title_len == 19 && memcmp(m.title, "Pride and Prejudice", 19) == 0);
    CHECK(m.creator_len == 11 && memcmp(m.creator, "Jane Austen", 11) == 0);
    CHECK(m.creator_count == 2);
    CHECK(m.language_len == 2 && memcmp(m.language, "en", 2) == 0);
    CHECK(m.identifier_len == 12 && memcmp(m.identifier, "urn:isbn:123", 12) == 0);
    CHECK(book.book_hash == bt_name_hash(BT_NAME_HASH_INIT, "urn:isbn:123", 12));

    CHECK(bartap_book_spine_count(&book, &count, &required) == BARTAP_OK);
    CHECK(count == 3 && required == 3);
    static const char *const names[] = {"OEBPS/text/ch 1.xhtml", "OEBPS/text/ch2.xhtml",
                                        "OEBPS/nav.xhtml"};
    for (uint16_t i = 0; i < 3; i++) {
        const epub_entry_t *e = entry_named(names[i]);
        CHECK(spine[i].dir_offset == e->dir_offset);
        CHECK(spine[i].local_offset == e->local_offset);
        CHECK(spine[i].uncompressed_size == e->uncompressed_size);
        CHECK(spine[i].compressed_size == e->compressed_size);
        CHECK(spine[i].crc32 == e->crc);
        CHECK(spine[i].method == e->method);
        CHECK(spine[i].path_len == e->name_len);
        CHECK(spine[i].path_hash == bt_name_hash(BT_NAME_HASH_INIT, e->name, e->name_len));

        /* every chapter reads whole, its CRC checked (R31) */
        CHECK(bartap_resource_open_spine(&res, &book, i, window) == BARTAP_OK);
        CHECK(bartap_resource_size(&res) == e->uncompressed_size);
        CHECK(read_all(&total) == BARTAP_END);
        CHECK(total == e->uncompressed_size);
        CHECK(memcmp(out, epub.bytes + e->local_offset + 30u + e->name_len,
                     e->method == EPUB_STORED ? total : 0) == 0);
    }
    CHECK(bartap_book_spine_linear(&book, 0, &linear) == BARTAP_OK && linear);
    CHECK(bartap_book_spine_linear(&book, 1, &linear) == BARTAP_OK && !linear);
    CHECK(bartap_book_spine_linear(&book, 3, &linear) == BARTAP_ERR_ARG);

    CHECK(book.nav_dir_offset == entry_named("OEBPS/nav.xhtml")->dir_offset);
    CHECK(book.ncx_dir_offset == BARTAP_NO_DIR_OFFSET);
    CHECK(bartap_book_cover(&book, &cover) == BARTAP_OK);
    CHECK(cover == entry_named("OEBPS/img/cover.jpg")->dir_offset);
    CHECK(bartap_resource_open_dir(&res, &book, cover, window) == BARTAP_OK);
    CHECK(read_all(&total) == BARTAP_END && total == 10 && memcmp(out, "JPEG bytes", 10) == 0);

    /* R23: three walks, each starting at the directory's first record, plus the packaging check's
     * single read of that record. */
    CHECK(open_as(&book, spine, SPINE_MAX, false) == BARTAP_OK);
    CHECK(reads_at_dir_start == 4);

    /* Hashed: building the index is the only walk of the card. The lookups and the matching walk
     * read the index, so the first record is read once more, by the packaging check alone. */
    CHECK(open_with(&third, third_spine, SPINE_MAX, index_buf, book.directory.size - 1u) == BARTAP_OK);
    CHECK(third.directory.index != NULL);
    CHECK(reads_at_dir_start == 2);
}

/* Manifest ids are XML ids, case-sensitive: R43's folding is for file names only. */
static void test_id_case(void)
{
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "c.opf");
    epub_add_opf(&epub, "c.opf", "uid",
                 "<dc:identifier id=\"UID\">wrong</dc:identifier>"
                 "<dc:identifier id=\"uid\">right</dc:identifier>"
                 "<meta name=\"cover\" content=\"img\"/>",
                 "<item id=\"Ch\" href=\"upper.xhtml\" media-type=\"application/xhtml+xml\"/>"
                 "<item id=\"ch\" href=\"lower.xhtml\" media-type=\"application/xhtml+xml\"/>"
                 "<item id=\"IMG\" href=\"upper.jpg\" media-type=\"image/jpeg\"/>"
                 "<item id=\"img\" href=\"lower.jpg\" media-type=\"image/jpeg\"/>",
                 "<itemref idref=\"ch\"/><itemref idref=\"Ch\"/><itemref idref=\"CH\"/>", NULL);
    epub_add(&epub, "upper.xhtml", epub_chapter(&chapter_text, "upper"), EPUB_STORED);
    epub_add(&epub, "lower.xhtml", epub_chapter(&chapter_text, "lower"), EPUB_STORED);
    epub_add(&epub, "upper.jpg", "U", EPUB_STORED);
    epub_add(&epub, "lower.jpg", "L", EPUB_STORED);
    epub_finish(&epub);

    bartap_metadata_t m;
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    /* the third idref names no item at all */
    CHECK(bartap_book_warnings(&book) == BARTAP_WARN_SPINE_ITEM_MISSING);
    CHECK(book.spine_count == 3);
    CHECK(spine[0].dir_offset == entry_named("lower.xhtml")->dir_offset);
    CHECK(spine[1].dir_offset == entry_named("upper.xhtml")->dir_offset);
    CHECK(spine[2].dir_offset == BARTAP_NO_DIR_OFFSET);
    CHECK(book.cover_dir_offset == entry_named("lower.jpg")->dir_offset);
    CHECK(bartap_book_metadata(&book, &m) == BARTAP_OK);
    CHECK(m.identifier_len == 5 && memcmp(m.identifier, "right", 5) == 0);
}

/* EPUB 2: the NCX by the spine's toc attribute, the cover by <meta name="cover">. */
static void test_epub2(void)
{
    uint32_t cover;

    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "content.opf");
    epub_add_opf(&epub, "content.opf", "uid",
                 "<dc:title>T</dc:title><dc:identifier id=\"uid\">x</dc:identifier>"
                 "<meta name=\"cover\" content=\"cov\"/>",
                 "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
                 "<item id=\"other-ncx\" href=\"other.ncx\" media-type=\"application/x-dtbncx+xml\"/>"
                 "<item id=\"cov\" href=\"c.png\" media-type=\"image/png\"/>"
                 "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>",
                 "<itemref idref=\"a\"/>", "other-ncx");
    epub_add(&epub, "toc.ncx", "<ncx/>", EPUB_STORED);
    epub_add(&epub, "other.ncx", "<ncx/>", EPUB_STORED);
    epub_add(&epub, "c.png", "PNG", EPUB_STORED);
    epub_add(&epub, "a.xhtml", epub_chapter(&chapter_text, "a"), EPUB_STORED);
    epub_finish(&epub);

    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(bartap_book_warnings(&book) == 0);
    /* the toc attribute wins over the first item of the NCX media type */
    CHECK(book.ncx_dir_offset == entry_named("other.ncx")->dir_offset);
    CHECK(book.nav_dir_offset == BARTAP_NO_DIR_OFFSET);
    CHECK(bartap_book_cover(&book, &cover) == BARTAP_OK && cover == entry_named("c.png")->dir_offset);
    CHECK(book.spine_count == 1 && spine[0].dir_offset == entry_named("a.xhtml")->dir_offset);

    /* without one, the NCX media type finds it */
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "content.opf");
    epub_add_opf(&epub, "content.opf", "uid", "<dc:identifier id=\"uid\">x</dc:identifier>",
                 "<item id=\"n\" href=\"toc.ncx\" media-type=\"Application/X-DTBNCX+XML\"/>",
                 "", NULL);
    epub_add(&epub, "toc.ncx", "<ncx/>", EPUB_STORED);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.ncx_dir_offset == entry_named("toc.ncx")->dir_offset);
    CHECK(bartap_book_cover(&book, &cover) == BARTAP_END);
    CHECK(book.spine_count == 0 && book.spine_required == 0);
}

/* A minimal book around a given OPF metadata block. */
static void build_with_metadata(const char *unique_id, const char *metadata)
{
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "p.opf");
    epub_add_opf(&epub, "p.opf", unique_id, metadata,
                 "<item id=\"a\" href=\"a.xhtml\" media-type=\"application/xhtml+xml\"/>",
                 "<itemref idref=\"a\"/>", NULL);
    epub_add(&epub, "a.xhtml", epub_chapter(&chapter_text, "a"), EPUB_STORED);
    epub_finish(&epub);
}

/* R25. */
static void test_metadata(void)
{
    epub_str_t meta = {0};

    /* Cut at a character boundary: 100 two-byte characters leave 63 of them, 126 bytes. */
    str_add(&meta, "<dc:identifier id=\"u\">i</dc:identifier><dc:title>");
    for (int i = 0; i < 100; i++) {
        str_add(&meta, "\xC3\xA9");
    }
    str_add(&meta, "</dc:title><dc:creator>A<span>B</span>  C</dc:creator>");
    for (int i = 0; i < 300; i++) {
        str_add(&meta, "<dc:creator>x</dc:creator>");
    }
    build_with_metadata("u", meta.s);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.title_len == 126);
    for (uint32_t i = 0; i < 126; i += 2) {
        CHECK((uint8_t)book.title[i] == 0xC3 && (uint8_t)book.title[i + 1u] == 0xA9);
    }
    CHECK(bartap_book_warnings(&book) == BARTAP_WARN_METADATA_TRUNCATED);
    /* markup inside a field is read through; the count saturates (R25) */
    CHECK(book.creator_len == 4 && memcmp(book.creator, "AB C", 4) == 0);
    CHECK(book.creator_count == 255);

    /* Exactly the limit is no truncation. */
    meta.len = 0;
    str_add(&meta, "<dc:identifier id=\"u\">i</dc:identifier><dc:title>");
    for (uint32_t i = 0; i < BARTAP_META_MAX; i++) {
        str_add(&meta, "t");
    }
    str_add(&meta, "</dc:title>");
    build_with_metadata("u", meta.s);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.title_len == BARTAP_META_MAX && bartap_book_warnings(&book) == 0);

    /* No identifier the unique-identifier names: the first non-empty one stands in, with a
     * warning; an empty one is passed over. */
    build_with_metadata("missing", "<dc:identifier> </dc:identifier><dc:identifier>first</dc:identifier>"
                                   "<dc:identifier>second</dc:identifier>");
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.identifier_len == 5 && memcmp(book.identifier, "first", 5) == 0);
    CHECK(bartap_book_warnings(&book) == BARTAP_WARN_NO_UNIQUE_ID);
    CHECK(book.book_hash == bt_name_hash(BT_NAME_HASH_INIT, "first", 5));

    /* The unique one replaces a stand-in read before it, and a stand-in's cut isn't reported. */
    meta.len = 0;
    str_add(&meta, "<dc:identifier>");
    for (int i = 0; i < 200; i++) {
        str_add(&meta, "s");
    }
    str_add(&meta, "</dc:identifier><dc:identifier id=\"u\">the-one</dc:identifier>");
    build_with_metadata("u", meta.s);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.identifier_len == 7 && memcmp(book.identifier, "the-one", 7) == 0);
    CHECK(bartap_book_warnings(&book) == 0);

    /* None at all: an empty identifier, hashed as such. Self-closing and later titles are skipped. */
    build_with_metadata("u", "<dc:title/><dc:title>Real</dc:title><dc:title>Later</dc:title>");
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.identifier_len == 0);
    CHECK(book.book_hash == BT_NAME_HASH_INIT);
    CHECK(book.title_len == 4 && memcmp(book.title, "Real", 4) == 0);
    CHECK(bartap_book_warnings(&book) == BARTAP_WARN_NO_UNIQUE_ID);
    str_free(&meta);
}

/* R41: more items than slots are held compact, up to three a slot; past that the first ones are
 * kept, and the full count is always reported. */
static void test_spine_truncation(void)
{
    uint16_t count;
    uint32_t required;
    bartap_entry_t entry;

    build_epub3();
    CHECK(open_both(2) == BARTAP_OK);
    CHECK(bartap_book_spine_count(&book, &count, &required) == BARTAP_OK);
    CHECK(count == 3 && required == 3 && book.spine_compact);
    CHECK(bartap_book_warnings(&book) == 0);
    CHECK(bt_spine_entry(&book, 2, &entry) == BARTAP_OK);
    CHECK(entry.dir_offset == entry_named("OEBPS/nav.xhtml")->dir_offset);
    /* exactly enough slots stays full */
    CHECK(open_both(3) == BARTAP_OK);
    CHECK(book.spine_count == 3 && !book.spine_compact);

    /* one slot holds three compact items, which is all of them */
    CHECK(open_as(&book, spine, 1, false) == BARTAP_OK);
    CHECK(book.spine_count == 3 && book.spine_compact && bartap_book_warnings(&book) == 0);
    for (uint16_t i = 0; i < 3; i++) {
        uint32_t total;
        CHECK(bartap_resource_open_spine(&res, &book, i, window) == BARTAP_OK);
        CHECK(read_all(&total) == BARTAP_END);
        CHECK(bartap_verify_init(&verify, &book, i, window) == BARTAP_OK);
    }
    CHECK(bartap_resource_open_spine(&res, &book, 3, window) == BARTAP_ERR_ARG);

    /* No array at all is valid. */
    CHECK(bartap_open(&book, &source, NULL, 0, NULL, 0, &work, window, NULL, NULL) == BARTAP_OK);
    CHECK(book.spine_count == 0 && book.spine_required == 3);
    CHECK(bartap_resource_open_spine(&res, &book, 0, window) == BARTAP_ERR_ARG);

    /* Many items. */
    epub_str_t manifest = {0};
    epub_str_t refs = {0};
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "p.opf");
    for (int i = 0; i < 300; i++) {
        char name[32];
        snprintf(name, sizeof name, "c%d.xhtml", i);
        str_add(&manifest, "<item id=\"i%d\" href=\"%s\"/>", i, name);
        str_add(&refs, "<itemref idref=\"i%d\"/>", 299 - i);
        epub_add(&epub, name, epub_chapter(&chapter_text, name), EPUB_STORED);
    }
    epub_add_opf(&epub, "p.opf", "u", "<dc:identifier id=\"u\">x</dc:identifier>", manifest.s,
                 refs.s, NULL);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.spine_count == 300 && bartap_book_warnings(&book) == 0);
    CHECK(spine[0].dir_offset == entry_named("c299.xhtml")->dir_offset);
    CHECK(spine[299].dir_offset == entry_named("c0.xhtml")->dir_offset);

    /* compact: 100 slots hold all 300, 50 slots the first 150, with the truncation warning */
    CHECK(open_both(100) == BARTAP_OK);
    CHECK(book.spine_count == 300 && book.spine_compact && bartap_book_warnings(&book) == 0);
    CHECK(bt_spine_entry(&book, 0, &entry) == BARTAP_OK);
    CHECK(entry.dir_offset == entry_named("c299.xhtml")->dir_offset);
    CHECK(bt_spine_entry(&book, 299, &entry) == BARTAP_OK);
    CHECK(entry.dir_offset == entry_named("c0.xhtml")->dir_offset);
    CHECK(open_both(50) == BARTAP_OK);
    CHECK(bartap_book_spine_count(&book, &count, &required) == BARTAP_OK);
    CHECK(count == 150 && required == 300 && book.spine_compact);
    CHECK(bartap_book_warnings(&book) == BARTAP_WARN_SPINE_TRUNCATED);
    CHECK(bt_spine_entry(&book, 149, &entry) == BARTAP_OK);
    CHECK(entry.dir_offset == entry_named("c150.xhtml")->dir_offset);
    str_free(&manifest);
    str_free(&refs);
}

/* Missing items warn with their index and fail only when opened (R23); the rest work. */
static void test_missing_items(void)
{
    static char long_href[400];
    epub_str_t manifest = {0};
    uint32_t total;

    memset(long_href, 'h', 300);
    str_add(&manifest,
            "<item id=\"a\" href=\"a.xhtml\"/><item id=\"gone\" href=\"gone.xhtml\"/>"
            "<item id=\"web\" href=\"http://example.com/x.xhtml\"/><item id=\"long\" href=\"%s\"/>"
            "<item id=\"slash\" href=\"a%%2Fb.xhtml\"/><item id=\"a\" href=\"gone.xhtml\"/>"
            "<item href=\"no-id.xhtml\"/><item id=\"no-href\"/>",
            long_href);
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "p.opf");
    epub_add_opf(&epub, "p.opf", "u", "<dc:identifier id=\"u\">x</dc:identifier>", manifest.s,
                 "<itemref idref=\"a\"/><itemref idref=\"nope\"/><itemref idref=\"gone\"/>"
                 "<itemref idref=\"web\"/><itemref/><itemref idref=\"long\"/>"
                 "<itemref idref=\"slash\"/><itemref idref=\"no-href\"/><itemref idref=\"a\"/>",
                 NULL);
    epub_add(&epub, "a.xhtml", epub_chapter(&chapter_text, "a"), EPUB_STORED);
    epub_finish(&epub);
    str_free(&manifest);

    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(bartap_book_warnings(&book) == (BARTAP_WARN_SPINE_ITEM_MISSING | BARTAP_WARN_PATH_TOO_LONG));
    CHECK(book.spine_count == 9);
    for (uint16_t i = 0; i < 9; i++) {
        /* the first item with an id wins, so both "a" refs reach a.xhtml */
        bool present = i == 0 || i == 8;
        CHECK((spine[i].dir_offset != BARTAP_NO_DIR_OFFSET) == present);
        CHECK(bartap_resource_open_spine(&res, &book, i, window) ==
              (present ? BARTAP_OK : BARTAP_ERR_CORRUPT));
        if (present) {
            CHECK(read_all(&total) == BARTAP_END);
        }
        CHECK(bartap_verify_init(&verify, &book, i, window) ==
              (present ? BARTAP_OK : BARTAP_ERR_CORRUPT));
    }
}

/* R44: names differing only in case are the same name, and the first in the directory wins. */
static void test_duplicates(void)
{
    uint32_t warnings = 0;

    build_epub3();
    epub.len = epub.dir_offset; /* reopen the archive to add one more entry */
    epub_add(&epub, "oebps/TEXT/CH2.xhtml", epub_chapter(&chapter_text, "dup"), EPUB_STORED);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    warnings = bartap_book_warnings(&book);
    CHECK(warnings == BARTAP_WARN_DUPLICATE_NAME);
    CHECK(spine[1].dir_offset == entry_named("OEBPS/text/ch2.xhtml")->dir_offset);

    /* The same for the nav and cover. */
    build_epub3();
    epub.len = epub.dir_offset;
    epub_add(&epub, "OEBPS/IMG/COVER.JPG", "second", EPUB_STORED);
    epub_add(&epub, "oebps/nav.XHTML", "second", EPUB_STORED);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(bartap_book_warnings(&book) == BARTAP_WARN_DUPLICATE_NAME);
    CHECK(book.cover_dir_offset == entry_named("OEBPS/img/cover.jpg")->dir_offset);
    CHECK(book.nav_dir_offset == entry_named("OEBPS/nav.xhtml")->dir_offset);
}

/* R30: packaging faults warn; only what makes the book unreadable is an error. */
static void build_packaging(int variant)
{
    epub_begin(&epub);
    if (variant == 0) {
        epub_add_mimetype(&epub);
    } else if (variant == 2) {
        epub_add(&epub, "mimetype", "application/epub+zip", EPUB_DEFLATED);
    }
    epub_add_container(&epub, "p.opf");
    epub_add_opf(&epub, "p.opf", "u", "<dc:identifier id=\"u\">x</dc:identifier>", "", "", NULL);
    if (variant == 3) {
        epub_add_mimetype(&epub);
    }
    epub_finish(&epub);
}

static void test_packaging(void)
{
    for (int variant = 0; variant < 4; variant++) {
        build_packaging(variant);
        CHECK(open_both(SPINE_MAX) == BARTAP_OK);
        CHECK(bartap_book_warnings(&book) == (variant == 0 ? 0u : (uint32_t)BARTAP_WARN_PACKAGING));
    }
}

static void test_unreadable(void)
{
    static char long_path[300];

    /* no container */
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_opf(&epub, "p.opf", "u", "", "", "", NULL);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_CORRUPT);

    /* no OPF where the container says */
    epub_begin(&epub);
    epub_add_mimetype(&epub);
    epub_add_container(&epub, "missing.opf");
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_CORRUPT);

    /* no rootfile, or none with a path */
    epub_begin(&epub);
    epub_add(&epub, "META-INF/container.xml",
             "<container><rootfiles><rootfile media-type=\"application/oebps-package+xml\"/>"
             "</rootfiles></container>",
             EPUB_STORED);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_CORRUPT);

    /* an OPF path refused by the resolver */
    epub_begin(&epub);
    epub_add_container(&epub, "a%2Fb.opf");
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_CORRUPT);

    /* an OPF path longer than Bartap keeps: valid, but unsupported, with a warning */
    memset(long_path, 'p', 260);
    epub_begin(&epub);
    epub_add_container(&epub, long_path);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_UNSUPPORTED);
    CHECK(other.warnings.bits == (BARTAP_WARN_PATH_TOO_LONG | BARTAP_WARN_PACKAGING));

    /* a container under a method Bartap can't read */
    epub_begin(&epub);
    epub_add(&epub, "META-INF/container.xml", "<container/>", 12);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_UNSUPPORTED);

    /* an OPF in UTF-16 (R30) */
    epub_begin(&epub);
    epub_add_container(&epub, "p.opf");
    epub_add_raw(&epub, "p.opf", 5, "\xFF\xFE<\0p\0>\0", 8, EPUB_STORED);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_UNSUPPORTED);

    /* an OPF whose data fails its CRC */
    epub_begin(&epub);
    epub_add_container(&epub, "p.opf");
    epub_add(&epub, "p.opf", "<package/>", EPUB_STORED)->crc ^= 1u;
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_CORRUPT);

    /* not a zip at all */
    epub_begin(&epub);
    epub_put(&epub, "not a zip", 9);
    CHECK(open_both(SPINE_MAX) == BARTAP_ERR_CORRUPT);
}

/* The OPF of the media type wins over an earlier rootfile of another; else the first one. */
static void test_rootfiles(void)
{
    epub_begin(&epub);
    epub_add(&epub, "META-INF/container.xml",
             "<container><rootfiles><rootfile full-path=\"other.pdf\" media-type=\"application/pdf\"/>"
             "<rootfile full-path=\"p.opf\" media-type=\"application/oebps-package+xml\"/>"
             "<rootfile full-path=\"later.opf\" media-type=\"application/oebps-package+xml\"/>"
             "</rootfiles></container>",
             EPUB_STORED);
    epub_add_opf(&epub, "p.opf", "u", "<dc:title>p</dc:title>", "", "", NULL);
    epub_add_opf(&epub, "later.opf", "u", "<dc:title>later</dc:title>", "", "", NULL);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.title_len == 1 && book.title[0] == 'p');

    epub_begin(&epub);
    epub_add(&epub, "META-INF/container.xml",
             "<container><rootfiles><rootfile full-path=\"first.opf\"/>"
             "<rootfile full-path=\"second.opf\"/></rootfiles></container>",
             EPUB_STORED);
    epub_add_opf(&epub, "first.opf", "u", "<dc:title>first</dc:title>", "", "", NULL);
    epub_finish(&epub);
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.title_len == 5 && memcmp(book.title, "first", 5) == 0);
}

/* ---- Resources and verify through a book (R31, R38) ---- */

static void test_resources(void)
{
    uint32_t total;
    static char long_name[70000];

    build_epub3();
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);

    /* by name, under R43's folding */
    CHECK(bartap_resource_open(&res, &book, "oebps/IMG/cover.JPG", 19, window) == BARTAP_OK);
    CHECK(read_all(&total) == BARTAP_END && total == 10);
    CHECK(bartap_resource_open(&res, &book, "OEBPS/none", 10, window) == BARTAP_END);
    CHECK(bartap_resource_size(&res) == 0);
    CHECK(read_all(&total) == BARTAP_END && total == 0); /* a failed open only repeats its result */
    memset(long_name, 'n', sizeof long_name);
    CHECK(bartap_resource_open(&res, &book, long_name, sizeof long_name, window) == BARTAP_END);

    /* an untrusted directory offset is checked (R29) */
    CHECK(bartap_resource_open_dir(&res, &book, 3, window) != BARTAP_OK);
    CHECK(bartap_resource_open_dir(&res, &book, UINT32_MAX, window) != BARTAP_OK);
    CHECK(bartap_resource_open_spine(&res, &book, 3, window) == BARTAP_ERR_ARG);

    /* verify: bounded steps, then OK once the entry checks out, and that repeats */
    CHECK(bartap_verify_init(&verify, &book, 0, window) == BARTAP_OK);
    CHECK(bartap_verify_step(&verify, 0) == BARTAP_ERR_ARG);
    int steps = 0;
    bartap_err_t err;
    while ((err = bartap_verify_step(&verify, 7000)) == BARTAP_MORE) {
        steps++;
    }
    CHECK(err == BARTAP_OK);
    CHECK(steps == (int)(spine[0].uncompressed_size / 7000u));
    CHECK(bartap_verify_step(&verify, 1) == BARTAP_OK);
    CHECK(bartap_verify_init(&verify, &book, 9, window) == BARTAP_ERR_ARG);
    CHECK(bartap_verify_init_dir(&verify, &book, book.cover_dir_offset, window) == BARTAP_OK);
    CHECK(bartap_verify_step(&verify, 1u << 20) == BARTAP_OK);
    CHECK(bartap_verify_init_dir(&verify, &book, 5, window) != BARTAP_OK);
    CHECK(bartap_verify_step(&verify, 10) != BARTAP_OK);

    /* a chapter whose data doesn't match its CRC: open is fine, the end isn't */
    build_epub3();
    for (uint32_t i = 0; i < epub.count; i++) {
        if (strcmp(epub.entries[i].name, "OEBPS/text/ch2.xhtml") == 0) {
            epub.bytes[epub.entries[i].local_offset + 30u + epub.entries[i].name_len] ^= 1u;
        }
    }
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(bartap_resource_open_spine(&res, &book, 1, window) == BARTAP_OK);
    CHECK(read_all(&total) == BARTAP_ERR_CORRUPT);
    CHECK(bartap_verify_init(&verify, &book, 1, window) == BARTAP_OK);
    CHECK(bartap_verify_step(&verify, 1u << 20) == BARTAP_ERR_CORRUPT);
    CHECK(bartap_verify_step(&verify, 1u << 20) == BARTAP_ERR_CORRUPT);
}

/* ---- The API's edges ---- */

static int warned;
static uint32_t warned_code;

static void on_warn(void *user, bartap_warning_t code, uint16_t spine_index, uint32_t offset)
{
    (void)spine_index;
    (void)offset;
    CHECK(user == &warned);
    warned++;
    warned_code = (uint32_t)code;
}

static void test_api(void)
{
    build_epub3();
    CHECK(bartap_source_memory(&source, epub.bytes, epub.len) == BARTAP_OK);
    CHECK(bartap_open(NULL, &source, spine, 1, NULL, 0, &work, window, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_open(&book, NULL, spine, 1, NULL, 0, &work, window, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_open(&book, &source, NULL, 1, NULL, 0, &work, window, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_open(&book, &source, spine, 1, NULL, 0, NULL, window, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_open(&book, &source, spine, 1, NULL, 0, &work, NULL, NULL, NULL) == BARTAP_ERR_ARG);

    /* A small directory buffer is ignored, never used partly (R24). */
    CHECK(bartap_open(&book, &source, spine, 3, dir_buf, 10, &work, window, NULL, NULL) == BARTAP_OK);
    CHECK(book.directory.raw == NULL);

    /* The callback hears each warning (R32). */
    build_packaging(1);
    CHECK(bartap_source_memory(&source, epub.bytes, epub.len) == BARTAP_OK);
    warned = 0;
    CHECK(bartap_open(&book, &source, spine, 3, NULL, 0, &work, window, on_warn, &warned) ==
          BARTAP_OK);
    CHECK(bartap_book_warnings(&book) == BARTAP_WARN_PACKAGING);
    CHECK(warned == 1 && warned_code == BARTAP_WARN_PACKAGING);

    /* The work struct is reusable: nothing from the last book leaks into the next. */
    build_epub3();
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    build_with_metadata("u", "<dc:title>Plain</dc:title>");
    CHECK(open_both(SPINE_MAX) == BARTAP_OK);
    CHECK(book.nav_dir_offset == BARTAP_NO_DIR_OFFSET);
    CHECK(book.cover_dir_offset == BARTAP_NO_DIR_OFFSET);
    CHECK(book.title_len == 5 && book.creator_count == 0);
}

int main(void)
{
    test_resolve();
    test_epub3();
    test_id_case();
    test_epub2();
    test_metadata();
    test_spine_truncation();
    test_missing_items();
    test_duplicates();
    test_packaging();
    test_unreadable();
    test_rootfiles();
    test_resources();
    test_api();
    epub_free(&epub);
    str_free(&chapter_text);
    return CHECK_RESULT();
}
