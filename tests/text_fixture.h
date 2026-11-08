/* Builds EPUBs around chapters for the text-layer tests, and dumps records as compact strings.
 * Test code only. */
#ifndef BARTAP_TEST_TEXT_FIXTURE_H
#define BARTAP_TEST_TEXT_FIXTURE_H

#include "epub_build.h"
#include "internal.h"

#ifndef TX_SPINE_MAX
#define TX_SPINE_MAX 16u
#endif
#define TX_CHAPTERS_MAX 8u

/* Paths in the built book. Chapters are OEBPS/text/c<i>.xhtml, so images are ../images/<name>. */
#define TX_OPF "OEBPS/content.opf"
#define TX_NAV "OEBPS/nav.xhtml"
#define TX_NCX "OEBPS/toc.ncx"

static epub_t tx_epub;
static bartap_source_t tx_source;
static uint8_t tx_window[BARTAP_WINDOW_SIZE];
static bartap_open_work_t tx_work;
static bartap_book_t tx_book;
static bartap_spine_item_t tx_spine[TX_SPINE_MAX];
static bartap_text_t tx_text;
static char tx_buf[1u << 16];
static const char *tx_chapters[TX_CHAPTERS_MAX];
static uint32_t tx_chapter_count;

/* The method tx_build gives the nav and NCX entries, deflated unless a test says otherwise. */
static uint16_t tx_toc_method = EPUB_DEFLATED;

/* Counts reads that reach the archive, for the checkpoint tests (R48). */
static uint32_t tx_reads;

static inline int tx_counting_read(void *user, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)user;
    tx_reads++;
    memcpy(buf, tx_epub.bytes + offset, len);
    return 0;
}

/* A chapter document around a body snippet. Offsets in dumps are relative to the snippet. */
#define TX_DOC_HEAD "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>t</title></head><body>"
#define TX_DOC_TAIL "</body></html>"

static inline const char *tx_doc(epub_str_t *s, const char *body)
{
    s->len = 0;
    str_add(s, "%s%s%s", TX_DOC_HEAD, body, TX_DOC_TAIL);
    return s->s;
}

/* Builds and opens a book: chapters are whole documents, nav and ncx are the TOC documents or NULL.
 * Chapter i is deflated when deflate is set (as stored deflate blocks: enough for the window path).
 * Images a.png and "b c.png" are always there. */
static inline void tx_build(const char *const *chapters, uint32_t count, const char *nav, const char *ncx,
                     bool deflate)
{
    epub_str_t manifest = {0};
    epub_str_t refs = {0};

    if (count > TX_CHAPTERS_MAX) {
        abort();
    }
    epub_begin(&tx_epub);
    epub_add_mimetype(&tx_epub);
    epub_add_container(&tx_epub, TX_OPF);
    for (uint32_t i = 0; i < count; i++) {
        char name[64];
        snprintf(name, sizeof name, "OEBPS/text/c%u.xhtml", i);
        epub_add(&tx_epub, name, chapters[i], deflate ? EPUB_DEFLATED : EPUB_STORED);
        str_add(&manifest, "<item id=\"c%u\" href=\"text/c%u.xhtml\" media-type=\"application/xhtml+xml\"/>", i, i);
        str_add(&refs, "<itemref idref=\"c%u\"/>", i);
        tx_chapters[i] = chapters[i];
    }
    tx_chapter_count = count;
    epub_add(&tx_epub, "OEBPS/images/a.png", "PNG-A", EPUB_STORED);
    epub_add(&tx_epub, "OEBPS/images/b c.png", "PNG-B", EPUB_STORED);
    if (nav != NULL) {
        epub_add(&tx_epub, TX_NAV, nav, tx_toc_method);
        str_add(&manifest, "<item id=\"nav\" href=\"nav.xhtml\" properties=\"nav\" media-type=\"application/xhtml+xml\"/>");
    }
    if (ncx != NULL) {
        epub_add(&tx_epub, TX_NCX, ncx, tx_toc_method);
        str_add(&manifest, "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>");
    }
    epub_add_opf(&tx_epub, TX_OPF, "uid", "<dc:identifier id=\"uid\">urn:test:book</dc:identifier><dc:title>T</dc:title>",
                 manifest.s != NULL ? manifest.s : "", refs.s != NULL ? refs.s : "",
                 ncx != NULL ? "ncx" : NULL);
    epub_finish(&tx_epub);
    str_free(&manifest);
    str_free(&refs);

    if (bartap_source_memory(&tx_source, tx_epub.bytes, tx_epub.len) != BARTAP_OK) {
        abort();
    }
    tx_source.read = tx_counting_read;
    if (bartap_open(&tx_book, &tx_source, tx_spine, TX_SPINE_MAX, NULL, 0, &tx_work, tx_window, NULL,
                    NULL) != BARTAP_OK) {
        printf("tx_build: the book didn't open\n");
        abort();
    }
}

/* The directory offset of a built entry, by name. */
static inline uint32_t tx_dir_offset(const char *name)
{
    for (uint32_t i = 0; i < tx_epub.count; i++) {
        if (tx_epub.entries[i].name_len == strlen(name) &&
            memcmp(tx_epub.entries[i].name, name, tx_epub.entries[i].name_len) == 0) {
            return tx_epub.entries[i].dir_offset;
        }
    }
    return BARTAP_NO_DIR_OFFSET;
}

/* ---- Dumps ---- */

/* Records as a string: structural records separated by spaces, text merged across records.
 *   block      p h1..h6 pre hr dt, then @offset with TX_OFFSETS, then +char_offset if not 0
 *   container  [quote [ol [ul [li<ordinal> [dl [dd [note, and ]kind for its end
 *   style      {bits}
 *   text       "..." (consecutive text records merged)
 *   image      <name> by the entry's last path segment, or <?dir_offset> if no entry has it
 *   break      |
 *   toc entry  T<depth>:<spine or ->, then #fragment */
#define TX_OFFSETS 1u

typedef struct {
    char s[1u << 18];
    size_t n;
    bool in_text;
    uint32_t base;  /* subtracted from block offsets */
    uint32_t flags;
    uint32_t records;
    uint32_t warnings;
} tx_dump_t;

static inline void dump_put(tx_dump_t *d, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (d->n < sizeof d->s) {
        int n = vsnprintf(d->s + d->n, sizeof d->s - d->n, fmt, ap);
        if (n > 0) {
            d->n += (size_t)n;
            if (d->n > sizeof d->s - 1u) {
                d->n = sizeof d->s - 1u;
            }
        }
    }
    va_end(ap);
}

static inline void dump_close_text(tx_dump_t *d)
{
    if (d->in_text) {
        dump_put(d, "\"");
        d->in_text = false;
    }
}

static inline void dump_sep(tx_dump_t *d)
{
    dump_close_text(d);
    if (d->n != 0) {
        dump_put(d, " ");
    }
}

static inline const char *tx_container_name(int kind)
{
    static const char *const names[] = {"?", "quote", "ol", "ul", "li", "dl", "dd", "note"};
    return kind >= 1 && kind <= 7 ? names[kind] : "?";
}

static inline void tx_dump_record(tx_dump_t *d, const bartap_record_t *r)
{
    d->records++;
    switch (r->kind) {
    case BARTAP_REC_BLOCK_START: {
        static const char *const kinds[] = {"?", "p", "h", "pre", "hr", "dt"};
        dump_sep(d);
        int k = (int)r->block.kind;
        dump_put(d, "%s", k >= 1 && k <= 5 ? kinds[k] : "?");
        if (r->block.kind == BARTAP_BLOCK_HEADING) {
            dump_put(d, "%u", r->block.level);
        }
        if (d->flags & TX_OFFSETS) {
            dump_put(d, "@%u", r->block.block_offset - d->base);
        }
        if (r->block.char_offset != 0) {
            dump_put(d, "+%u", r->block.char_offset);
        }
        break;
    }
    case BARTAP_REC_CONTAINER_START:
        dump_sep(d);
        dump_put(d, "[%s", tx_container_name((int)r->container.kind));
        if (r->container.kind == BARTAP_CONTAINER_LI) {
            dump_put(d, "%d", r->container.ordinal);
        }
        break;
    case BARTAP_REC_CONTAINER_END:
        dump_sep(d);
        dump_put(d, "]%s", tx_container_name((int)r->container.kind));
        break;
    case BARTAP_REC_STYLE:
        dump_close_text(d);
        dump_put(d, "{%u}", r->style.bits);
        break;
    case BARTAP_REC_TEXT:
        if (!d->in_text) {
            dump_put(d, "\"");
            d->in_text = true;
        }
        dump_put(d, "%.*s", (int)r->text.len, r->text.ptr);
        break;
    case BARTAP_REC_IMAGE: {
        dump_close_text(d);
        bool named = false;
        for (uint32_t i = 0; i < tx_epub.count && !named; i++) {
            const epub_entry_t *e = &tx_epub.entries[i];
            if (e->dir_offset == r->image.dir_offset) {
                uint32_t start = 0;
                for (uint32_t k = 0; k < e->name_len; k++) {
                    if (e->name[k] == '/') {
                        start = k + 1u;
                    }
                }
                dump_put(d, "<%.*s>", (int)(e->name_len - start), e->name + start);
                named = true;
            }
        }
        if (!named) {
            dump_put(d, "<?%u>", r->image.dir_offset);
        }
        break;
    }
    case BARTAP_REC_LINE_BREAK:
        dump_close_text(d);
        dump_put(d, "|");
        break;
    case BARTAP_REC_TOC_ENTRY:
        dump_sep(d);
        dump_put(d, "T%u:", r->toc.depth);
        if (r->toc.spine_index == BARTAP_NO_SPINE) {
            dump_put(d, "-");
        } else {
            dump_put(d, "%u", r->toc.spine_index);
        }
        if (r->toc.fragment != NULL) {
            dump_put(d, "#%.*s", (int)r->toc.fragment_len, r->toc.fragment);
        }
        break;
    default:
        dump_sep(d);
        dump_put(d, "?%d", (int)r->kind);
    }
}

static inline void tx_dump_begin(tx_dump_t *d, uint32_t base, uint32_t flags)
{
    d->n = 0;
    d->s[0] = '\0';
    d->in_text = false;
    d->base = base;
    d->flags = flags;
    d->records = 0;
}

static inline const char *tx_dump_end(tx_dump_t *d)
{
    dump_close_text(d);
    return d->s;
}

/* Reads the open text object to its end into d. Returns the final result: END, or the failure. */
static inline bartap_err_t tx_dump_text(tx_dump_t *d, bartap_text_t *text, uint32_t cap)
{
    bartap_record_t r;
    bartap_err_t err;
    uint32_t guard = 0;
    while ((err = bartap_text_next(text, &r)) == BARTAP_OK && guard++ < 10000000u) {
        if (r.kind == BARTAP_REC_TEXT && (r.text.len == 0 || r.text.len > cap)) {
            dump_put(d, "!badlen%u", r.text.len);
        }
        tx_dump_record(d, &r);
    }
    tx_dump_end(d);
    d->warnings = bartap_text_warnings(text);
    return err;
}

/* Opens chapter i, memory mode or stream mode, and dumps it. */
static inline const char *tx_read(tx_dump_t *d, uint16_t chapter, bool stream, uint32_t cap, uint32_t base,
                           uint32_t flags)
{
    bartap_err_t err;
    if (stream) {
        err = bartap_text_open(&tx_text, &tx_book, chapter, tx_window, tx_buf, cap, NULL, NULL, NULL);
    } else {
        const char *doc = tx_chapters[chapter];
        err = bartap_text_open_memory(&tx_text, &tx_book, chapter, (const uint8_t *)doc,
                                      (uint32_t)strlen(doc), tx_buf, cap, NULL, NULL, NULL);
    }
    tx_dump_begin(d, base, flags);
    if (err != BARTAP_OK) {
        dump_put(d, "!open%d", err);
        return d->s;
    }
    err = tx_dump_text(d, &tx_text, cap);
    if (err != BARTAP_END) {
        dump_put(d, " !err%d", err);
    }
    return d->s;
}

#endif /* BARTAP_TEST_TEXT_FIXTURE_H */
