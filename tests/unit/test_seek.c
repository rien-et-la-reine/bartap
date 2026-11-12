/* Bookmarks and seek (R10, R12, R13, R19, R48, R49): the serialized bytes and every fallback level,
 * the P1 and P2 properties at every landing point in every mode, checkpoints sparing the card, quiet
 * warnings during replay, and resolving TOC ids to landings. Pass a stride to sample landing points. */
#include "check.h"
#include "text_fixture.h"

#define HEAD_LEN ((uint32_t)(sizeof TX_DOC_HEAD - 1u))

static epub_str_t doc;
static tx_dump_t dump;
static tx_dump_t fresh;
static tx_dump_t at_block;

/* ---- Bookmark bytes (R12, R19) ---- */

/* The name hash, written again here: FNV-1a over ASCII-folded bytes (R43). */
static uint32_t test_hash(const char *s)
{
    uint32_t h = 0x811C9DC5u;
    for (; *s != '\0'; s++) {
        uint8_t c = (uint8_t)*s;
        if (c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c + 32u);
        }
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_bookmark_bytes(void)
{
    epub_str_t second = {0};
    const char *chapters[2] = {tx_doc(&doc, "<p>one</p>"), tx_doc(&second, "<p>two</p>")};
    tx_build(chapters, 2, NULL, NULL, true);
    uint8_t b[BARTAP_BOOKMARK_SIZE];
    bartap_bookmark_t bm;

    CHECK(BARTAP_BOOKMARK_SIZE == 23u);
    CHECK(bartap_bookmark_write(&tx_book, 1, 0x01020304u, 0x0A0B0C0Du, b) == BARTAP_OK);
    /* frozen offsets, little-endian byte by byte */
    CHECK(b[0] == BARTAP_BOOKMARK_VERSION);
    CHECK(le32(b + 1) == test_hash("urn:test:book"));
    CHECK(b[5] == 1 && b[6] == 0);
    CHECK(b[7] == 0x04 && b[8] == 0x03 && b[9] == 0x02 && b[10] == 0x01);
    CHECK(b[11] == 0x0D && b[12] == 0x0C && b[13] == 0x0B && b[14] == 0x0A);
    CHECK(le32(b + 15) == epub_crc32((const uint8_t *)chapters[1], (uint32_t)strlen(chapters[1])));
    CHECK(le32(b + 19) == epub_crc32(b, 19));

    /* round trip */
    CHECK(bartap_bookmark_read(&tx_book, b, &bm) == BARTAP_OK);
    CHECK(bm.version == BARTAP_BOOKMARK_VERSION && bm.book_hash == tx_book.book_hash);
    CHECK(bm.spine_index == 1 && bm.block_offset == 0x01020304u && bm.char_offset == 0x0A0B0C0Du);
    CHECK(bm.chapter_crc32 == tx_spine[1].crc32);

    /* check 0: any changed byte fails the CRC, before anything else is looked at */
    for (uint32_t i = 0; i < BARTAP_BOOKMARK_SIZE; i++) {
        for (uint32_t bit = 0; bit < 8; bit++) {
            b[i] ^= (uint8_t)(1u << bit);
            CHECK(bartap_bookmark_read(&tx_book, b, &bm) == BARTAP_ERR_CORRUPT);
            b[i] ^= (uint8_t)(1u << bit);
        }
    }
    /* a spine index past the book, with a good CRC */
    b[5] = 2;
    uint32_t crc = epub_crc32(b, 19);
    b[19] = (uint8_t)crc;
    b[20] = (uint8_t)(crc >> 8);
    b[21] = (uint8_t)(crc >> 16);
    b[22] = (uint8_t)(crc >> 24);
    CHECK(bartap_bookmark_read(&tx_book, b, &bm) == BARTAP_ERR_CORRUPT);

    /* check 1: another book's */
    uint8_t mine[BARTAP_BOOKMARK_SIZE];
    CHECK(bartap_bookmark_write(&tx_book, 0, 5, 0, mine) == BARTAP_OK);
    tx_book.book_hash ^= 1u;
    CHECK(bartap_bookmark_read(&tx_book, mine, &bm) == BARTAP_ERR_WRONG_BOOK);
    tx_book.book_hash ^= 1u;
    CHECK(bartap_bookmark_read(&tx_book, mine, &bm) == BARTAP_OK);

    /* arguments */
    CHECK(bartap_bookmark_write(&tx_book, 2, 0, 0, b) == BARTAP_ERR_ARG);
    CHECK(bartap_bookmark_write(NULL, 0, 0, 0, b) == BARTAP_ERR_ARG);
    CHECK(bartap_bookmark_write(&tx_book, 0, 0, 0, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_bookmark_read(NULL, b, &bm) == BARTAP_ERR_ARG);
    CHECK(bartap_bookmark_read(&tx_book, NULL, &bm) == BARTAP_ERR_ARG);
    CHECK(bartap_bookmark_read(&tx_book, b, NULL) == BARTAP_ERR_ARG);
    str_free(&second);
}

/* A chapter with every kind of record, long enough for several checkpoints and evictions. */
static const char *rich_chapter(int repeats)
{
    epub_str_t body = {0};
    for (int i = 0; i < repeats; i++) {
        str_add(&body, "<p>para %d <i>it<b>bo</b></i>  caf&#233; <img src=\"../images/a.png\"/> &amp;"
                       "<![CDATA[x<y]]>\xe2\x82\xac end</p>\n", i);
        str_add(&body, "<div>loose %d<br/>x<p>in<div>restart</div>after</p></div>", i);
        if (i % 3 == 0) {
            str_add(&body, "<b><p>bold block %d</p><h2>head <i>x</i></h2></b><blockquote><ul><li><p>deep"
                           "</p></li></ul></blockquote>", i);
        }
        if (i % 7 == 0) {
            str_add(&body, "<ol start=\"%d\"><li>a</li><li>b<blockquote><p>q <u>u</u> v</p>tail"
                           "</blockquote></li></ol><pre>a\n b\n\n</pre><img src=\"../images/a.png\"/><hr/>", i);
        }
        if (i % 11 == 0) {
            str_add(&body, "<aside epub:type=\"footnote\"><p>note</p></aside><p/>x<br/>");
        }
    }
    const char *chapter = tx_doc(&doc, body.s);
    str_free(&body);
    return chapter;
}

#include "seek_prop.h"

/* Memory modes read the chapter just built. */
static void use_chapter(const char *chapter)
{
    sp_data = (const uint8_t *)chapter;
    sp_size = (uint32_t)strlen(chapter);
}

static int stride = 1;

static void test_properties(void)
{
    const char *chapter = rich_chapter(60);
    tx_build(&chapter, 1, NULL, NULL, true);
    use_chapter(chapter);
    straight_read();
    CHECK(point_n > 1000);

    static const uint32_t rings[] = {512, 64, 2048};
    static const uint32_t caps[] = {BARTAP_TEXT_MIN, 4096};
    int failures = 0;
    for (size_t ri = 0; ri < sizeof rings / sizeof rings[0]; ri++) {
        ring_size = rings[ri];
        /* every point at the default ring, a sample at the others */
        int step = ri == 0 ? stride : stride * 7;
        for (int i = 0; i < point_n; i += step) {
            const point_t *p = &points[i];
            bartap_bookmark_t bm = bookmark(p->block, p->chr);
            for (size_t ci = 0; ci < sizeof caps / sizeof caps[0]; ci++) {
                /* from a fresh open, part way through, and after the end */
                int warm = (i + (int)ci) % 3 == 0 ? 0 : (i * 13) % 900;
                if (i % 5 == 0) {
                    warm = 1000000;
                }
                for (int mode = 0; mode < MODE_COUNT; mode++) {
                    seek_read(mode, caps[ci], warm, &bm, &outs[mode]);
                    bool ok = mode == 0 ? p2_holds(p, &outs[0]) : same_raw(&outs[0], &outs[mode]);
                    if (!ok && failures++ < 10) {
                        printf("%s fails: ring %u cap %u warm %d mode %d point %d (block %u char %u)\n",
                               mode == 0 ? "P2" : "P1", ring_size, caps[ci], warm, mode, i, p->block,
                               p->chr);
                    }
                }
            }
        }
    }
    CHECK(failures == 0);
}

/* ---- Fallbacks (R12 checks 2 to 4, R29) ---- */

static void read_from_start(tx_dump_t *d)
{
    open_mode(MODE_MEMORY, 4096);
    tx_dump_begin(d, 0, TX_OFFSETS);
    CHECK(tx_dump_text(d, &tx_text, 4096) == BARTAP_END);
}

/* Seeks a fresh text object and dumps what follows. */
static bartap_err_t seek_dump(tx_dump_t *d, int mode, const bartap_bookmark_t *bm)
{
    open_mode(mode, 4096);
    bartap_record_t r;
    for (int i = 0; i < 50; i++) {
        (void)bartap_text_next(&tx_text, &r);
    }
    bartap_err_t err = bartap_text_seek(&tx_text, bm);
    tx_dump_begin(d, 0, TX_OFFSETS);
    if (err == BARTAP_OK) {
        CHECK(tx_dump_text(d, &tx_text, 4096) == BARTAP_END);
    }
    return err;
}

static void test_fallbacks(void)
{
    const char *chapter = rich_chapter(20);
    tx_build(&chapter, 1, NULL, NULL, true);
    use_chapter(chapter);
    straight_read();
    read_from_start(&fresh);

    /* a block holding a 3-byte character, with something after it */
    const point_t *target = NULL;
    for (int i = 0; i < point_n && target == NULL; i++) {
        int at = points[i].at;
        if (points[i].chr == 0 && i > 40 && at < straight_n && straight[at].kind == 'T') {
            target = &points[i];
        }
    }
    CHECK(target != NULL);
    if (target == NULL) {
        return;
    }
    for (int mode = 0; mode < MODE_COUNT; mode++) {
        bartap_bookmark_t bm = bookmark(target->block, 0);

        /* check 2: the chapter changed, its start */
        bm.chapter_crc32 ^= 1u;
        CHECK(seek_dump(&dump, mode, &bm) == BARTAP_OK && strcmp(dump.s, fresh.s) == 0);
        bm.chapter_crc32 ^= 1u;

        /* check 3: another version lands at the start of the block holding the offset */
        bartap_bookmark_t old = bm;
        old.version = (uint8_t)(BARTAP_BOOKMARK_VERSION + 1u);
        old.block_offset = target->block + 3u;
        old.char_offset = 999999u;
        CHECK(seek_dump(&at_block, mode, &bm) == BARTAP_OK);
        CHECK(seek_dump(&dump, mode, &old) == BARTAP_OK && strcmp(dump.s, at_block.s) == 0);
        /* an offset before any block: the start */
        old.block_offset = 0;
        CHECK(seek_dump(&dump, mode, &old) == BARTAP_OK && strcmp(dump.s, fresh.s) == 0);

        /* check 4, invalid landings: not a block start, past the block's text, inside a character */
        bartap_bookmark_t bad = bm;
        bad.block_offset = target->block + 1u;
        CHECK(seek_dump(&dump, mode, &bad) == BARTAP_OK && strcmp(dump.s, fresh.s) == 0);
        bad = bm;
        bad.char_offset = 100000u;
        CHECK(seek_dump(&dump, mode, &bad) == BARTAP_OK && strcmp(dump.s, fresh.s) == 0);
        bad.block_offset = (uint32_t)strlen(chapter) + 10u;
        bad.char_offset = 0;
        CHECK(seek_dump(&dump, mode, &bad) == BARTAP_OK && strcmp(dump.s, fresh.s) == 0);

        /* another item's bookmark is the caller's mistake */
        bad = bm;
        bad.spine_index = 1;
        CHECK(seek_dump(&dump, mode, &bad) == BARTAP_ERR_ARG);
    }

    /* inside a multi-byte character: find a block whose text has one */
    for (int i = 0; i < point_n; i++) {
        int at = points[i].at;
        if (at + 1 < straight_n && straight[at].kind == 'T' && (uint8_t)straight[at].a >= 0xE0u) {
            bartap_bookmark_t inside = bookmark(points[i].block, points[i].chr + 1u);
            CHECK(seek_dump(&dump, MODE_MEMORY, &inside) == BARTAP_OK && strcmp(dump.s, fresh.s) == 0);
            break;
        }
    }

    /* a text object that never opened refuses to seek, with its open's failure */
    bartap_bookmark_t bm = bookmark(target->block, 0);
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, 1, NULL, NULL, NULL) ==
          BARTAP_ERR_BUFFER_TOO_SMALL);
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_ERR_BUFFER_TOO_SMALL);
    CHECK(bartap_text_seek(NULL, &bm) == BARTAP_ERR_ARG);
    CHECK(bartap_text_seek(&tx_text, NULL) == BARTAP_ERR_ARG);
}

/* ---- Checkpoints spare the card (R48) ---- */

static void test_card_reads(void)
{
    const char *chapter = rich_chapter(600);
    tx_build(&chapter, 1, NULL, NULL, true);
    use_chapter(chapter);
    straight_read();

    ring_size = 8192;
    open_mode(MODE_STREAM_RING, 4096);
    bartap_record_t r;
    /* read on past the inflate window, which could serve the chapter's start without the card,
     * until the newer checkpoint is a good way behind but not yet replaced */
    const bartap_checkpoint_t *newer = &rolling.checkpoints[1];
    bool placed = false;
    for (int i = 0; i < 1000000 && !placed; i++) {
        if (bartap_text_next(&tx_text, &r) != BARTAP_OK) {
            printf("card reads: chapter of %u bytes ended before the checkpoints were placed\n", sp_size);
            break;
        }
        placed = newer->valid && newer->block_offset > 2u * BARTAP_WINDOW_SIZE &&
                 tx_text.parse.block_offset >= newer->block_offset + 1500u;
    }
    CHECK(placed);
    uint32_t live = tx_text.parse.block_offset;
    const point_t *back = NULL;
    for (int i = 0; i < point_n; i++) {
        if (points[i].chr == 0 && points[i].block >= newer->block_offset && points[i].block + 600u < live) {
            back = &points[i];
        }
    }
    CHECK(back != NULL);
    if (back == NULL) {
        return;
    }
    uint32_t before = tx_reads;
    bartap_bookmark_t bm = bookmark(back->block, 0);
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_OK);
    /* replayed from the ring: no card reads, no inflating */
    CHECK(tx_reads == before);

    /* before both checkpoints: the entry rewinds and the card is read again */
    bm = bookmark(points[1].block, 0);
    CHECK(points[1].block < rolling.checkpoints[0].block_offset || !rolling.checkpoints[0].valid);
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_OK);
    CHECK(tx_reads > before);
}

/* ---- Quiet replay (R49) ---- */

static uint32_t warn_calls;

static void on_warn(void *user, bartap_warning_t code, uint16_t spine_index, uint32_t offset)
{
    (void)user;
    (void)code;
    (void)spine_index;
    (void)offset;
    warn_calls++;
}

static void test_quiet_replay(void)
{
    const char *chapter = tx_doc(&doc, "<p>one\x01</p><p>two</p><p>three\x02</p><p>four</p>");
    tx_build(&chapter, 1, NULL, NULL, true);
    use_chapter(chapter);
    uint32_t four = (uint32_t)(strstr(chapter, "<p>four") - chapter);
    bartap_bookmark_t bm = bookmark(four, 0);
    bartap_record_t r;

    /* everything read, then a seek back over both: the replay is silent */
    warn_calls = 0;
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, 64, NULL, on_warn, NULL) ==
          BARTAP_OK);
    while (bartap_text_next(&tx_text, &r) == BARTAP_OK) {
    }
    CHECK(warn_calls == 2);
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    while (bartap_text_next(&tx_text, &r) == BARTAP_OK) {
    }
    CHECK(warn_calls == 2);

    /* a seek forward replays new ground, and warns there */
    warn_calls = 0;
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, 64, NULL, on_warn, NULL) ==
          BARTAP_OK);
    for (int i = 0; i < 3; i++) {
        CHECK(bartap_text_next(&tx_text, &r) == BARTAP_OK);
    }
    CHECK(warn_calls == 1);
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    CHECK(warn_calls == 2);
    CHECK(bartap_text_warnings(&tx_text) == BARTAP_WARN_CONTROL_CHAR);
}

/* ---- TOC ids to landings (R13) ---- */

static void expect_resolve(const char *id, uint32_t block, uint32_t chr)
{
    bartap_bookmark_t bm;
    open_mode(MODE_MEMORY, 64);
    CHECK(bartap_toc_resolve(&tx_text, id, (uint32_t)strlen(id), &bm) == BARTAP_OK);
    if (bm.block_offset != block || bm.char_offset != chr) {
        printf("resolve %s: want %u+%u, got %u+%u\n", id, block, chr, bm.block_offset, bm.char_offset);
        check_failures++;
    }
    CHECK(bm.version == BARTAP_BOOKMARK_VERSION && bm.book_hash == tx_book.book_hash);
    CHECK(bm.spine_index == 0 && bm.chapter_crc32 == tx_spine[0].crc32);

    /* it has landed there already: the records are the ones a seek to the bookmark gives */
    tx_dump_begin(&dump, 0, TX_OFFSETS);
    CHECK(tx_dump_text(&dump, &tx_text, 64) == BARTAP_END);
    open_mode(MODE_MEMORY, 64);
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    tx_dump_begin(&fresh, 0, TX_OFFSETS);
    CHECK(tx_dump_text(&fresh, &tx_text, 64) == BARTAP_END);
    if (strcmp(dump.s, fresh.s) != 0) {
        printf("resolve %s lands on:\n%s\na seek to its bookmark on:\n%s\n", id, dump.s, fresh.s);
        check_failures++;
    }

    /* and seeking there from where it landed works too */
    bartap_record_t r;
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    bartap_err_t err;
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK && r.kind == BARTAP_REC_CONTAINER_START) {
    }
    if (block != 0) {
        CHECK(err == BARTAP_OK && r.kind == BARTAP_REC_BLOCK_START);
        CHECK(r.block.block_offset == block && r.block.char_offset == chr);
    }
}

static void test_resolve(void)
{
    const char *chapter = tx_doc(
        &doc, "<h2 id=\"top\">Title</h2><p>Some <a id=\"mid\">text</a> here.</p>"
              "<section id=\"sec\"><div><p>after</p></div></section><p>x<span id=\"sp\"></span></p>"
              "<p id=\"last\">end</p><div id=\"tail\"></div>");
    tx_build(&chapter, 1, NULL, NULL, true);
    use_chapter(chapter);
    uint32_t base = HEAD_LEN;
    uint32_t mid = (uint32_t)(strstr(chapter, "<p>Some") - chapter);
    uint32_t after = (uint32_t)(strstr(chapter, "<p>after") - chapter);
    uint32_t x = (uint32_t)(strstr(chapter, "<p>x") - chapter);
    uint32_t last = (uint32_t)(strstr(chapter, "<p id=\"last\"") - chapter);

    expect_resolve("top", base, 0);
    /* inside a block: after the text before it, the pending space not yet counted */
    expect_resolve("mid", mid, 4);
    /* between blocks: the next block */
    expect_resolve("sec", after, 0);
    expect_resolve("sp", x, 1);
    expect_resolve("last", last, 0);
    /* nothing after it: the end of the last block */
    expect_resolve("tail", last, 3);

    /* not there: the chapter start, with a warning, and a fresh read's records */
    bartap_bookmark_t bm;
    open_mode(MODE_MEMORY, 64);
    CHECK(bartap_toc_resolve(&tx_text, "nope", 4, &bm) == BARTAP_OK);
    CHECK(bm.block_offset == 0 && bm.char_offset == 0);
    CHECK(bartap_text_warnings(&tx_text) & BARTAP_WARN_BROKEN_LINK);
    tx_dump_begin(&dump, 0, TX_OFFSETS);
    CHECK(tx_dump_text(&dump, &tx_text, 64) == BARTAP_END);
    open_mode(MODE_MEMORY, 64);
    tx_dump_begin(&fresh, 0, TX_OFFSETS);
    CHECK(tx_dump_text(&fresh, &tx_text, 64) == BARTAP_END);
    CHECK(strcmp(dump.s, fresh.s) == 0);
    open_mode(MODE_MEMORY, 64);
    /* case matters for ids */
    CHECK(bartap_toc_resolve(&tx_text, "TOP", 3, &bm) == BARTAP_OK && bm.block_offset == 0);

    /* The landing is at the element. Its bookmark counts text only (R10), so a seek to it stops
     * right after the text before the element: a break or image in between comes out again. An
     * element that is itself a break, an image or a block start lands on that record, in every
     * mode. */
    const char *marks = tx_doc(
        &doc, "<ul><li><p>ab<br/><b id=\"past\">cd</b><br id=\"br\"/>ef</p></li></ul>"
              "<div id=\"gap\"></div><br id=\"lone\"/>gh<p>x<i id=\"empty\"/>y</p><p id=\"self\"/><hr id=\"rule\"/>z");
    tx_build(&marks, 1, NULL, NULL, true);
    use_chapter(marks);
    static const struct {
        const char *id;
        const char *first; /* the kinds of the first records after the context's containers */
    } want[] = {
        {"past", "BST"},  /* block start, style, then the bold text: no break before it */
        {"br", "BSL"},    /* the break itself */
        {"gap", "BSL"},   /* between blocks: the block the lone break opens */
        {"lone", "BSL"},
        {"empty", "BST"},
        {"self", "BSB"},  /* an empty block, then the hr's */
        {"rule", "BSB"},
    };
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        for (int mode = 0; mode < MODE_COUNT; mode++) {
            bartap_record_t r;
            char kinds[4] = "";
            ring_size = 512;
            open_mode(mode, 64);
            CHECK(bartap_toc_resolve(&tx_text, want[k].id, (uint32_t)strlen(want[k].id), &bm) == BARTAP_OK);
            CHECK((bartap_text_warnings(&tx_text) & BARTAP_WARN_BROKEN_LINK) == 0);
            bartap_err_t err;
            while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK && r.kind == BARTAP_REC_CONTAINER_START) {
            }
            for (int n = 0; n < 3 && err == BARTAP_OK; n++) {
                kinds[n] = r.kind == BARTAP_REC_BLOCK_START ? 'B' : r.kind == BARTAP_REC_STYLE ? 'S'
                         : r.kind == BARTAP_REC_TEXT ? 'T' : r.kind == BARTAP_REC_LINE_BREAK ? 'L' : '?';
                if (n == 0) {
                    CHECK(r.kind == BARTAP_REC_BLOCK_START && r.block.block_offset == bm.block_offset &&
                          r.block.char_offset == bm.char_offset);
                }
                err = bartap_text_next(&tx_text, &r);
            }
            if (strcmp(kinds, want[k].first) != 0) {
                printf("resolve %s mode %d: want %s, got %s\n", want[k].id, mode, want[k].first, kinds);
                check_failures++;
            }
            while (err == BARTAP_OK) {
                err = bartap_text_next(&tx_text, &r);
            }
            CHECK(err == BARTAP_END);
        }
    }

    /* an id inside head isn't a place in the text */
    const char *raw = "<html><head><title id=\"hd\">t</title></head><body><p>a</p></body></html>";
    tx_build(&raw, 1, NULL, NULL, false);
    use_chapter(raw);
    open_mode(MODE_MEMORY, 64);
    CHECK(bartap_toc_resolve(&tx_text, "hd", 2, &bm) == BARTAP_OK && bm.block_offset == 0);
    CHECK(bartap_text_warnings(&tx_text) & BARTAP_WARN_BROKEN_LINK);

    CHECK(bartap_toc_resolve(&tx_text, NULL, 2, &bm) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_resolve(&tx_text, "a", 0, &bm) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_resolve(NULL, "a", 1, &bm) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_resolve(&tx_text, "a", 1, NULL) == BARTAP_ERR_ARG);
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        stride = atoi(argv[1]) > 0 ? atoi(argv[1]) : 1;
    }
    test_bookmark_bytes();
    test_properties();
    test_fallbacks();
    test_card_reads();
    test_quiet_replay();
    test_resolve();
    str_free(&doc);
    epub_free(&tx_epub);
    return CHECK_RESULT();
}
