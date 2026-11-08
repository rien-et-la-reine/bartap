/* The TOC iterator (R13, R26): nav and NCX, depths, targets and fragments, labels as style and text
 * records, unresolvable targets, the NCX label re-read from the inflate window and by rewinding,
 * every buffer size, the API, and following an entry to its landing. */
#include "check.h"
#include "text_fixture.h"

static bartap_toc_t toc;
static tx_dump_t dump;
static tx_dump_t other;
static epub_str_t ch0;
static epub_str_t ch1;

#define NAV_HEAD "<?xml version=\"1.0\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" " \
                 "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>N</title></head><body>"
#define NCX_HEAD "<?xml version=\"1.0\"?><ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" " \
                 "version=\"2005-1\"><head></head><docTitle><text>T</text></docTitle>"

static const char nav_doc[] =
    NAV_HEAD
    "<nav epub:type=\"landmarks\"><ol><li><a href=\"text/c0.xhtml\">Skip</a></li></ol></nav>"
    "<nav epub:type=\"toc\" id=\"toc\"><h1>Contents</h1><ol>"
    "<li><a href=\"text/c0.xhtml\">One  <i>it</i>\n </a></li>"
    "<li><span>Part</span><ol><li><a href=\"text/c1.xhtml#s%C3%A9c\">Two</a></li>"
    "<li><a href=\"http://x.com/\">Web</a></li></ol></li>"
    "<li><a href=\"missing.xhtml\">Gone</a></li>"
    "<li><a href=\"text/C0.XHTML#top\">Case</a></li>"
    "</ol></nav>"
    "<nav epub:type=\"page-list\"><ol><li><a href=\"text/c0.xhtml\">1</a></li></ol></nav></body></html>";

static const char nav_want[] =
    "T0:0{0}\"One\"{1}\" it\" T0:-{0}\"Part\" T1:1#s\xc3\xa9" "c{0}\"Two\" T1:-{0}\"Web\" "
    "T0:-{0}\"Gone\" T0:0#top{0}\"Case\"";

static const char ncx_doc[] =
    NCX_HEAD "<navMap>"
    "<navPoint id=\"a\" playOrder=\"1\"><navLabel><text>One</text></navLabel><content src=\"text/c0.xhtml\"/>"
    "<navPoint id=\"b\"><navLabel><text>Sub &amp;  more</text></navLabel><content src=\"text/c1.xhtml#x\"/>"
    "</navPoint></navPoint>"
    "<navPoint id=\"c\"><content src=\"text/c1.xhtml\"/><navLabel><text>Late label</text></navLabel></navPoint>"
    "<navPoint id=\"d\"><navLabel><text>No content</text></navLabel></navPoint>"
    "<navPoint id=\"e\"><navLabel><text>Gone</text></navLabel><content src=\"nope.xhtml\"/></navPoint>"
    "</navMap><pageList><pageTarget><navLabel><text>1</text></navLabel><content src=\"text/c0.xhtml\"/>"
    "</pageTarget></pageList></ncx>";

static const char ncx_want[] =
    "T0:0{0}\"One\" T1:1#x{0}\"Sub & more\" T0:1{0}\"Late label\" T0:-{0}\"Gone\"";

static void build(const char *nav, const char *ncx)
{
    const char *chapters[2] = {
        tx_doc(&ch0, "<p id=\"top\">zero</p>"),
        tx_doc(&ch1, "<p>intro</p><h2 id=\"s\xc3\xa9" "c\">Sec</h2><p id=\"x\">x</p>"),
    };
    tx_build(chapters, 2, nav, ncx, true);
}

static uint32_t warn_calls;

static void on_warn(void *user, bartap_warning_t code, uint16_t spine_index, uint32_t offset)
{
    (void)user;
    (void)code;
    (void)offset;
    CHECK(spine_index == BARTAP_NO_SPINE);
    warn_calls++;
}

/* The whole TOC as a dump, at one buffer size. */
static const char *read_toc(tx_dump_t *d, uint32_t cap)
{
    bartap_record_t r;
    bartap_err_t err = bartap_toc_open(&toc, &tx_book, tx_window, tx_buf, cap, on_warn, NULL);
    tx_dump_begin(d, 0, 0);
    if (err != BARTAP_OK) {
        dump_put(d, "!open%d", err);
        return d->s;
    }
    while ((err = bartap_toc_next(&toc, &r)) == BARTAP_OK) {
        if (r.kind == BARTAP_REC_TEXT) {
            CHECK(r.text.len >= 1 && r.text.len <= cap);
        }
        tx_dump_record(d, &r);
    }
    tx_dump_end(d);
    if (err != BARTAP_END) {
        dump_put(d, " !err%d", err);
    }
    d->warnings = bartap_toc_warnings(&toc);
    return d->s;
}

static void expect_toc(int line, const char *want, uint32_t warnings)
{
    for (uint32_t cap = BARTAP_TEXT_MIN; cap <= 4096; cap = cap < 16 ? cap + 1 : cap * 4) {
        const char *got = read_toc(&dump, cap);
        if (strcmp(got, want) != 0 || dump.warnings != warnings) {
            printf("line %d, cap %u:\n  want %s  warnings %#x\n  got  %s  warnings %#x\n", line, cap,
                   want, warnings, got, dump.warnings);
            check_failures++;
            return;
        }
    }
}

static void test_nav(void)
{
    build(nav_doc, NULL);
    expect_toc(__LINE__, nav_want, BARTAP_WARN_TOC_TARGET);
}

static void test_ncx(void)
{
    build(NULL, ncx_doc);
    expect_toc(__LINE__, ncx_want, BARTAP_WARN_TOC_TARGET);
}

static void test_preference(void)
{
    /* both: the nav, EPUB 3's */
    build(nav_doc, ncx_doc);
    expect_toc(__LINE__, nav_want, BARTAP_WARN_TOC_TARGET);

    /* neither: an empty TOC */
    build(NULL, NULL);
    bartap_record_t r;
    CHECK(bartap_toc_open(&toc, &tx_book, tx_window, tx_buf, 64, NULL, NULL) == BARTAP_END);
    CHECK(bartap_toc_next(&toc, &r) == BARTAP_END);

    /* a nav with no toc nav in it, and an NCX with no navMap */
    build(NAV_HEAD "<nav epub:type=\"landmarks\"><ol><li><a href=\"text/c0.xhtml\">L</a></li></ol></nav>"
                   "</body></html>",
          NULL);
    expect_toc(__LINE__, "", 0);
    build(NULL, NCX_HEAD "</ncx>");
    expect_toc(__LINE__, "", 0);
}

static void test_nav_shapes(void)
{
    /* nested navs inside the toc nav don't end it; a self-closing a is a labelless entry */
    build(NAV_HEAD "<nav epub:type=\"toc\"><nav><p>x</p></nav><ol><li><a href=\"text/c1.xhtml\"/></li>"
                   "<li><a href=\"text/c0.xhtml\">A</a><span>not an entry</span></li></ol></nav></body></html>",
          NULL);
    expect_toc(__LINE__, "T0:1 T0:0{0}\"A\"", 0);

    /* a label closed by its item, and an a with no href, which is a heading */
    build(NAV_HEAD "<nav epub:type=\"toc\"><ol><li><a href=\"text/c0.xhtml\">open<ol><li><a>Head</a></li>"
                   "</ol></li></ol></nav></body></html>",
          NULL);
    expect_toc(__LINE__, "T0:0{0}\"open\" T1:-{0}\"Head\"", 0);

    /* nested style in a label, a break as a space, a fragment with a bad escape kept as it is */
    build(NAV_HEAD "<nav epub:type=\"toc\"><ol><li><a href=\"text/c0.xhtml#a%zz\"><b>x <i>y</i></b><br/>z"
                   "</a></li></ol></nav></body></html>",
          NULL);
    expect_toc(__LINE__, "T0:0#a%zz{0}{2}\"x\"{3}\" y\"{0}\" z\"", 0);
}

static void test_ncx_reread(void)
{
    /* a label longer than the inflate window: the entry rewinds for it, still correct (R26) */
    epub_str_t ncx = {0};
    epub_str_t want = {0};
    str_add(&ncx, "%s<navMap><navPoint><navLabel><text>", NCX_HEAD);
    str_add(&want, "T0:0{0}\"");
    for (int i = 0; i < 40000; i++) {
        str_add(&ncx, "%c", 'a' + i % 26);
        str_add(&want, "%c", 'a' + i % 26);
    }
    str_add(&ncx, "</text></navLabel><content src=\"text/c0.xhtml\"/></navPoint>"
                  "<navPoint><navLabel><text>B</text></navLabel><content src=\"text/c1.xhtml\"/></navPoint>"
                  "</navMap></ncx>");
    str_add(&want, "\" T0:1{0}\"B\"");
    build(NULL, ncx.s);
    expect_toc(__LINE__, want.s, 0);

    /* a warning in a label is raised once, not again on the re-read */
    build(NULL, NCX_HEAD "<navMap><navPoint><navLabel><text>ctl\x01</text></navLabel>"
                         "<content src=\"text/c0.xhtml\"/></navPoint></navMap></ncx>");
    warn_calls = 0;
    CHECK(strcmp(read_toc(&dump, 64), "T0:0{0}\"ctl\"") == 0);
    CHECK(warn_calls == 1 && dump.warnings == BARTAP_WARN_CONTROL_CHAR);

    /* content with no src: still an entry, with a warning */
    build(NULL, NCX_HEAD "<navMap><navPoint><navLabel><text>L</text></navLabel><content/></navPoint>"
                         "</navMap></ncx>");
    expect_toc(__LINE__, "T0:-{0}\"L\"", BARTAP_WARN_TOC_TARGET);
    str_free(&ncx);
    str_free(&want);
}

static void test_api(void)
{
    bartap_record_t r;
    build(nav_doc, NULL);
    CHECK(bartap_toc_open(&toc, &tx_book, tx_window, tx_buf, BARTAP_TEXT_MIN - 1u, NULL, NULL) ==
          BARTAP_ERR_BUFFER_TOO_SMALL);
    CHECK(bartap_toc_next(&toc, &r) == BARTAP_ERR_BUFFER_TOO_SMALL);
    CHECK(bartap_toc_open(&toc, NULL, tx_window, tx_buf, 64, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_next(&toc, &r) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_open(&toc, &tx_book, NULL, tx_buf, 64, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_open(&toc, &tx_book, tx_window, NULL, 64, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_open(NULL, &tx_book, tx_window, tx_buf, 64, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_next(NULL, &r) == BARTAP_ERR_ARG);
    CHECK(bartap_toc_warnings(NULL) == 0);

    /* END again after the end */
    read_toc(&dump, 64);
    CHECK(bartap_toc_next(&toc, &r) == BARTAP_END);
}

/* An entry's fragment, followed: resolve in its chapter, seek, and land on the element (R13). */
static void test_follow(void)
{
    build(nav_doc, NULL);
    bartap_record_t r;
    char fragment[64];
    uint32_t fragment_len = 0;
    uint16_t spine = BARTAP_NO_SPINE;
    CHECK(bartap_toc_open(&toc, &tx_book, tx_window, tx_buf, 64, NULL, NULL) == BARTAP_OK);
    while (bartap_toc_next(&toc, &r) == BARTAP_OK) {
        if (r.kind == BARTAP_REC_TOC_ENTRY && r.toc.spine_index == 1 && r.toc.fragment != NULL) {
            /* valid until the next call: copy it */
            fragment_len = r.toc.fragment_len;
            memcpy(fragment, r.toc.fragment, fragment_len);
            spine = r.toc.spine_index;
            break;
        }
    }
    CHECK(spine == 1 && fragment_len == 4 && memcmp(fragment, "s\xc3\xa9" "c", 4) == 0);

    bartap_bookmark_t bm;
    CHECK(bartap_text_open(&tx_text, &tx_book, spine, tx_window, tx_buf, 64, NULL, NULL, NULL) ==
          BARTAP_OK);
    CHECK(bartap_toc_resolve(&tx_text, fragment, fragment_len, &bm) == BARTAP_OK);
    CHECK(bm.block_offset == (uint32_t)(strstr(tx_chapters[1], "<h2") - tx_chapters[1]));
    CHECK(bm.char_offset == 0);
    CHECK(bartap_text_seek(&tx_text, &bm) == BARTAP_OK);
    tx_dump_begin(&other, (uint32_t)(sizeof TX_DOC_HEAD - 1u), 0);
    CHECK(tx_dump_text(&other, &tx_text, 64) == BARTAP_END);
    CHECK(strcmp(other.s, "h2{0}\"Sec\" p{0}\"x\"") == 0);
}

int main(void)
{
    test_nav();
    test_ncx();
    test_preference();
    test_nav_shapes();
    test_ncx_reread();
    test_api();
    test_follow();
    str_free(&ch0);
    str_free(&ch1);
    epub_free(&tx_epub);
    return CHECK_RESULT();
}
