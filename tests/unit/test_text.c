/* Text layer records (R4-R9, R16, R20, R21, R45-R47, R50): element classes, whitespace, styles, lists,
 * notes, recovery from broken markup, the depth limit, images, block offsets, every text-buffer size
 * from the minimum up, stream and memory mode, and the open and next API. */
#include "check.h"
#include "text_fixture.h"

static epub_str_t doc;
static tx_dump_t dump;
static tx_dump_t other;

#define HEAD_LEN ((uint32_t)(sizeof TX_DOC_HEAD - 1u))

/* A body snippet, read in memory and stream mode and through the minimum buffer: all three must give
 * want, with these warnings and no others. */
static void expect_at(int line, const char *body, uint32_t flags, const char *want, uint32_t warnings)
{
    const char *chapter = tx_doc(&doc, body);
    tx_build(&chapter, 1, NULL, NULL, true);
    const struct {
        bool stream;
        uint32_t cap;
    } runs[] = {{false, 4096}, {true, 4096}, {false, BARTAP_TEXT_MIN}};
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        const char *got = tx_read(&dump, 0, runs[i].stream, runs[i].cap, HEAD_LEN, flags);
        if (strcmp(got, want) != 0 || dump.warnings != warnings) {
            printf("line %d (%s, cap %u):\n  body %s\n  want %s  warnings %#x\n  got  %s  warnings %#x\n",
                   line, runs[i].stream ? "stream" : "memory", runs[i].cap, body, want, warnings, got,
                   dump.warnings);
            check_failures++;
        }
    }
}

#define EXPECT(body, want, warnings) expect_at(__LINE__, body, 0, want, warnings)
#define EXPECT_AT(body, want, warnings) expect_at(__LINE__, body, TX_OFFSETS, want, warnings)

/* A whole document, not wrapped, memory mode. */
static void expect_raw_at(int line, const char *document, const char *want, uint32_t warnings)
{
    tx_build(&document, 1, NULL, NULL, false);
    const char *got = tx_read(&dump, 0, false, 4096, 0, 0);
    if (strcmp(got, want) != 0 || dump.warnings != warnings) {
        printf("line %d:\n  doc  %s\n  want %s  warnings %#x\n  got  %s  warnings %#x\n", line, document,
               want, warnings, got, dump.warnings);
        check_failures++;
    }
}

#define EXPECT_RAW(document, want, warnings) expect_raw_at(__LINE__, document, want, warnings)

static void test_whitespace(void)
{
    /* R4: spaces collapse across style records, and a space keeps the style it was collapsed in */
    EXPECT("<p>a <i> b</i> c</p>", "p{0}\"a \"{1}\"b\"{0}\" c\"", 0);
    EXPECT("<p>un<i>like</i>ly</p>", "p{0}\"un\"{1}\"like\"{0}\"ly\"", 0);
    EXPECT("<p><u>a</u> b</p>", "p{0}{32}\"a\"{0}\" b\"", 0);
    EXPECT("<p><u>a</u> <i>b</i></p>", "p{0}{32}\"a\"{0}\" \"{1}\"b\"", 0);
    EXPECT("<p>a<i> </i>b</p>", "p{0}\"a\"{1}\" \"{0}\"b\"", 0);
    EXPECT("<p><i>a </i>b</p>", "p{0}{1}\"a \"{0}\"b\"", 0);
    EXPECT("<p>a<span> </span>b</p>", "p{0}\"a b\"", 0);
    /* no leading or trailing spaces in a block, none between blocks */
    EXPECT("<p>  </p><p>\n x \n</p>", "p{0} p{0}\"x\"", 0);
    EXPECT("<p>a</p>\n\n  <p>b</p>", "p{0}\"a\" p{0}\"b\"", 0);
    EXPECT("<p>a\t\r\n b</p>", "p{0}\"a b\"", 0);
    /* entities and CDATA are text like any other, and a non-breaking space doesn't collapse */
    EXPECT("<p>&#233;t&#233; &amp; <![CDATA[ c ]]></p>", "p{0}\"\xc3\xa9t\xc3\xa9 & c\"", 0);
    EXPECT("<p>a&nbsp; b</p>", "p{0}\"a\xc2\xa0 b\"", 0);
    EXPECT("<p>a&#32;&#32;b</p>", "p{0}\"a b\"", 0);
}

static void test_blocks(void)
{
    EXPECT("<h1>a</h1><h6>b</h6><dt>c</dt>", "h1{0}\"a\" h6{0}\"b\" dt{0}\"c\"", 0);
    EXPECT("<html:p xmlns:html=\"http://www.w3.org/1999/xhtml\">x</html:p><P>y</P>",
           "p{0}\"x\" p{0}\"y\"", 0);
    /* boundaries close an implicit paragraph so words never fuse (R46) */
    EXPECT("loose <b>text</b><p>p</p>more", "p{0}\"loose \"{2}\"text\" p{0}\"p\" p{0}\"more\"", 0);
    EXPECT("<div>a</div><div>b</div>", "p{0}\"a\" p{0}\"b\"", 0);
    EXPECT("<table><caption>c</caption><tr><th>h</th><td>a</td><td>b</td></tr></table>",
           "p{0}\"c\" p{0}\"h\" p{0}\"a\" p{0}\"b\"", 0);
    /* a boundary inside a block restarts its kind; only a block with content ends */
    EXPECT("<p>a<div>b</div>c</p>", "p{0}\"a\" p{0}\"b\" p{0}\"c\"", 0);
    EXPECT("<h2><div>A</div></h2>", "h2{0}\"A\"", 0);
    EXPECT("<h2>A<div>B</div></h2>", "h2{0}\"A\" h2{0}\"B\"", 0);
    EXPECT("<p><div></div>a</p>", "p{0}\"a\"", 0);
    /* a container inside a block: the outer kind resumes after it, never inside it */
    EXPECT("<p>a<blockquote>b</blockquote>c</p>", "p{0}\"a\" [quote p{0}\"b\" ]quote p{0}\"c\"", 0);
    /* hr takes no frame, so content after it is a block of its own */
    EXPECT("<hr/><hr></hr>x", "hr{0} hr{0} p{0}\"x\"", 0);
    /* self-closing elements open and close at once */
    EXPECT("<p/><blockquote/><i/>z", "p{0} [quote ]quote p{0}\"z\"", 0);
    /* transparent elements: content passes through, close tags ignored without a warning */
    EXPECT("<p>a<span class=\"x\">b</span><a href=\"#\">c</a><small>d</small></p>", "p{0}\"abcd\"", 0);
    EXPECT("<p>a</span>b</p>", "p{0}\"ab\"", 0);
}

static void test_skipped(void)
{
    EXPECT("<style>p{}</style><script>x<p>no</p></script><p>a</p>", "p{0}\"a\"", 0);
    EXPECT("<title>a<title>b</title>c</title><p>d</p>", "p{0}\"d\"", 0);
    EXPECT("<svg><desc>d</desc><title>t</title></svg>", "", 0);
    EXPECT("<p>a<title/>b</p>", "p{0}\"ab\"", 0);
}

static void test_styles(void)
{
    /* R7: every inline element's bit, complete style per record, lazily before the text it covers */
    EXPECT("<p><i>i</i><em>e</em><cite>c</cite><b>b</b><strong>s</strong><code>c</code><kbd>k</kbd>"
           "<samp>s</samp><tt>t</tt><sub>s</sub><sup>s</sup><u>u</u><ins>i</ins><s>s</s><del>d</del>"
           "<strike>s</strike></p>",
           "p{0}{1}\"iec\"{2}\"bs\"{4}\"ckst\"{8}\"s\"{16}\"s\"{32}\"ui\"{64}\"sds\"", 0);
    EXPECT("<p><b><i>x</i></b></p>", "p{0}{3}\"x\"", 0);
    /* R8: every block start restates the style, carried across a restart */
    EXPECT("<p><i>a<div>b</div>c</i></p>", "p{0}{1}\"a\" p{1}\"b\" p{1}\"c\"", 0);
    /* an empty inline element changes nothing */
    EXPECT("<p>a<i></i>b</p>", "p{0}\"ab\"", 0);
}

static void test_lists(void)
{
    EXPECT("<ol start=\"3\"><li>a</li><li value=\"10\"><p>b</p><p>c</p></li><li>d</li></ol>",
           "[ol [li3 p{0}\"a\" ]li [li10 p{0}\"b\" p{0}\"c\" ]li [li11 p{0}\"d\" ]li ]ol", 0);
    EXPECT("<ul><li>a</li><li>b</li></ul>", "[ul [li1 p{0}\"a\" ]li [li2 p{0}\"b\" ]li ]ul", 0);
    EXPECT("<ol reversed=\"\" start=\"2\"><li>a</li><li>b</li><li>c</li></ol>",
           "[ol [li2 p{0}\"a\" ]li [li1 p{0}\"b\" ]li [li0 p{0}\"c\" ]li ]ol", 0);
    EXPECT("<ol reversed=\"\"><li>a</li><li>b</li></ol>", "[ol [li1 p{0}\"a\" ]li [li2 p{0}\"b\" ]li ]ol",
           BARTAP_WARN_REVERSED_NO_START);
    EXPECT("<ol start=\"-5\"><li>a</li><li>b</li></ol>", "[ol [li-5 p{0}\"a\" ]li [li-4 p{0}\"b\" ]li ]ol", 0);
    EXPECT("<ol start=\" 7abc\"><li>a</li></ol><ol start=\"x\"><li>b</li></ol>",
           "[ol [li7 p{0}\"a\" ]li ]ol [ol [li1 p{0}\"b\" ]li ]ol", 0);
    /* clamped, then saturating */
    EXPECT("<ol start=\"99999999999\"><li>a</li><li>b</li></ol>",
           "[ol [li2147483647 p{0}\"a\" ]li [li2147483647 p{0}\"b\" ]li ]ol", BARTAP_WARN_ORDINAL_CLAMPED);
    EXPECT("<ol start=\"-2147483648\" reversed=\"\"><li>a</li><li>b</li></ol>",
           "[ol [li-2147483648 p{0}\"a\" ]li [li-2147483648 p{0}\"b\" ]li ]ol", 0);
    EXPECT("<ol start=\"-99999999999\"><li>a</li></ol>", "[ol [li-2147483648 p{0}\"a\" ]li ]ol",
           BARTAP_WARN_ORDINAL_CLAMPED);
    /* an item numbers from its nearest list, 0 outside any */
    EXPECT("<li>x</li>", "[li0 p{0}\"x\" ]li", 0);
    EXPECT("<ol><li>a<ul><li>b</li></ul></li><li>c</li></ol>",
           "[ol [li1 p{0}\"a\" [ul [li1 p{0}\"b\" ]li ]ul ]li [li2 p{0}\"c\" ]li ]ol", 0);
    EXPECT("<dl><dt>t</dt><dd>d</dd></dl>", "[dl dt{0}\"t\" [dd p{0}\"d\" ]dd ]dl", 0);
}

static void test_notes(void)
{
    EXPECT("<aside epub:type=\"footnote\"><p>n</p></aside><aside>x</aside>",
           "[note p{0}\"n\" ]note p{0}\"x\"", 0);
    EXPECT("<aside epub:type=\"noteref endnote\">e</aside><aside epub:type=\"rearnote\">r</aside>",
           "[note p{0}\"e\" ]note [note p{0}\"r\" ]note", 0);
    /* a token, not a substring */
    EXPECT("<aside epub:type=\"footnotes\">x</aside>", "p{0}\"x\"", 0);
}

static void test_recovery(void)
{
    /* rule 1: a block inside a live block pops it; the surplus close is rule 3 */
    EXPECT("<p>a<p>b</p></p>", "p{0}\"a\" p{0}\"b\"", BARTAP_WARN_CLOSE_UNMATCHED);
    /* inline frames above the popped block stay, so style carries */
    EXPECT("<p><i>a<p>b</i></p>", "p{0}{1}\"a\" p{1}\"b\"",
           BARTAP_WARN_CLOSE_UNWOUND | BARTAP_WARN_CLOSE_UNMATCHED);
    /* past a boundary the outer frame stays, and its close matches normally */
    EXPECT("<p>a<div>b<p>c</p></div>d</p>", "p{0}\"a\" p{0}\"b\" p{0}\"c\" p{0}\"d\"", 0);
    EXPECT("<p>x<hr/>y</p>", "p{0}\"x\" hr{0} p{0}\"y\"", BARTAP_WARN_CLOSE_UNMATCHED);
    /* rule 2: unwinding closes every container above, balanced */
    EXPECT("<blockquote><li><p>a</blockquote>", "[quote [li0 p{0}\"a\" ]li ]quote", BARTAP_WARN_CLOSE_UNWOUND);
    /* rule 3 */
    EXPECT("</p>x</i>", "p{0}\"x\"", BARTAP_WARN_CLOSE_UNMATCHED);
    /* hr, br and img never take a frame, so their close tags are fine */
    EXPECT("<p>a<br></br>b<img src=\"../images/a.png\"></img></p>", "p{0}\"a\"|\"b\"<a.png>", 0);
}

static void test_end_of_item(void)
{
    /* rule 4: everything closes, with a warning for an unclosed container or explicit block */
    EXPECT_RAW("<p>a", "p{0}\"a\"", BARTAP_WARN_UNCLOSED_AT_END);
    EXPECT_RAW("<blockquote><p>a", "[quote p{0}\"a\" ]quote", BARTAP_WARN_UNCLOSED_AT_END);
    EXPECT_RAW("<i>a", "p{1}\"a\"", 0);
    EXPECT_RAW("<div>a", "p{0}\"a\"", 0);
    EXPECT_RAW("", "", 0);
}

static void test_depth(void)
{
    /* rule 5: body is frame 0, so 31 containers fit. the rest, their p included, are ignored, their
     * close tags with them, and their text stays where it is */
    epub_str_t body = {0};
    epub_str_t want = {0};
    for (int i = 0; i < 33; i++) {
        str_add(&body, "<blockquote>");
    }
    str_add(&body, "<p>a</p>");
    for (int i = 0; i < 33; i++) {
        str_add(&body, "</blockquote>");
    }
    for (int i = 0; i < 31; i++) {
        str_add(&want, "%s[quote", i == 0 ? "" : " ");
    }
    str_add(&want, " p{0}\"a\"");
    for (int i = 0; i < 31; i++) {
        str_add(&want, " ]quote");
    }
    EXPECT(body.s, want.s, BARTAP_WARN_DEPTH_EXCEEDED);

    /* an ignored boundary still ends the live block, open and close */
    body.len = 0;
    for (int i = 0; i < 31; i++) {
        str_add(&body, "<i>");
    }
    str_add(&body, "a<div>b</div>c");
    for (int i = 0; i < 31; i++) {
        str_add(&body, "</i>");
    }
    EXPECT(body.s, "p{1}\"a\" p{1}\"b\" p{1}\"c\"", BARTAP_WARN_DEPTH_EXCEEDED);
    str_free(&body);
    str_free(&want);
}

static void test_breaks_and_pre(void)
{
    /* no space at the start of a line after a break */
    EXPECT("<p>a<br/>\n  b<br></br>c</p>", "p{0}\"a\"|\"b\"|\"c\"", 0);
    /* a break on its own gets a block */
    EXPECT("<div><br/></div>", "p{0}|", 0);
    /* pre keeps spacing; LF, CR and CRLF are one break each, a tab one space */
    EXPECT("<pre>  a\n\n b\r\nc\td\re</pre>", "pre{0}\"  a\"||\" b\"|\"c d\"|\"e\"", 0);
    EXPECT("<pre>x<div>y\nz</div></pre>", "pre{0}\"x\" pre{0}\"y\"|\"z\"", 0);
    EXPECT("<pre><b>a</b> b</pre>", "pre{0}{2}\"a\"{0}\" b\"", 0);
}

static void test_images(void)
{
    /* inline where they sit, a break opportunity both sides (R5, R6) */
    EXPECT("<p>a <img src=\"../images/a.png\"/> b</p>", "p{0}\"a\"<a.png>\"b\"", 0);
    EXPECT("<p><img src=\"../images/b%20c.png\"/></p>", "p{0}<b c.png>", 0);
    EXPECT("<p><img src=\"/OEBPS/images/a.png\"/></p>", "p{0}<a.png>", 0);
    EXPECT("<p><img src=\"../IMAGES/A.PNG\"/></p>", "p{0}<a.png>", 0);
    EXPECT("<img src=\"../images/a.png\"></img>t", "p{0}<a.png>\"t\"", 0);
    EXPECT("<svg><image xlink:href=\"../images/a.png\"/><image href=\"../images/b%20c.png\"/></svg>",
           "p{0}<a.png><b c.png>", 0);
    /* unresolved: no record, still content (R9) */
    EXPECT("<p><img src=\"nope.png\"/></p>", "p{0}", BARTAP_WARN_IMAGE_UNRESOLVED);
    EXPECT("<p><img src=\"data:image/png;base64,AA\"/></p>", "p{0}", BARTAP_WARN_IMAGE_UNRESOLVED);
    EXPECT("<p><img src=\"http://example.com/a.png\"/></p>", "p{0}", BARTAP_WARN_IMAGE_UNRESOLVED);
    EXPECT("<p><img/>a</p>", "p{0}\"a\"", BARTAP_WARN_IMAGE_UNRESOLVED);
    EXPECT("<img src=\"nope.png\"/><div>x</div>", "p{0} p{0}\"x\"", BARTAP_WARN_IMAGE_UNRESOLVED);
}

static void test_offsets(void)
{
    /* R16: an explicit block at its <, a block with no tag at the token that opened it */
    EXPECT_AT("<p>a</p><h3>b</h3>", "p@0{0}\"a\" h3@8{0}\"b\"", 0);
    EXPECT_AT("<p>a<div>b</div>c</p>", "p@0{0}\"a\" p@9{0}\"b\" p@16{0}\"c\"", 0);
    EXPECT_AT("<div>\n  hello</div>", "p@5{0}\"hello\"", 0);
    EXPECT_AT("<div><img src=\"../images/a.png\"/></div>", "p@5{0}<a.png>", 0);
    EXPECT_AT("<div><br/></div>", "p@5{0}|", 0);
    EXPECT_AT("<div><![CDATA[x]]></div>", "p@5{0}\"x\"", 0);
    EXPECT_AT("<div>&amp;</div>", "p@5{0}\"&\"", 0);
}

/* ---- Every buffer size, both modes (R6, R50) ---- */

static const char *const pieces[] = {
    "<p>a <i> b</i> c caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80x\xf0\x9f\x98\x80 \xf0\x9f\x98\x80</p>",
    "<ol start=\"3\"><li>a<blockquote><p>q <u>u</u> v</p>tail</blockquote></li></ol>",
    "<pre>  a\n\n b\r\nc\td</pre><div>loose<br/>x<p>in<div>restart</div>after</p></div>",
    "<p>a <img src=\"../images/a.png\"/> b &amp;<![CDATA[x<y]]> end</p><hr/>",
    "<aside epub:type=\"footnote\"><p>n</p></aside><table><tr><td>a</td><td>b</td></tr></table>",
    "<p><b>bold <i>both</i></b> plain <sub>2</sub>   <sup>3</sup></p>",
};

/* Merged dump at one size, checking every text record holds whole characters within the buffer. */
static const char *read_checked(tx_dump_t *d, bool stream, uint32_t cap)
{
    bartap_err_t err;
    if (stream) {
        err = bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, cap, NULL, NULL, NULL);
    } else {
        err = bartap_text_open_memory(&tx_text, &tx_book, 0, (const uint8_t *)tx_chapters[0],
                                      (uint32_t)strlen(tx_chapters[0]), tx_buf, cap, NULL, NULL, NULL);
    }
    CHECK(err == BARTAP_OK);
    tx_dump_begin(d, HEAD_LEN, TX_OFFSETS);
    bartap_record_t r;
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
        if (r.kind == BARTAP_REC_TEXT) {
            const uint8_t *p = (const uint8_t *)r.text.ptr;
            CHECK(r.text.len >= 1 && r.text.len <= cap);
            /* starts on a character, ends after a whole one */
            CHECK((p[0] & 0xC0u) != 0x80u);
            uint32_t i = 0;
            while (i < r.text.len) {
                uint32_t len = p[i] < 0x80u ? 1u : p[i] < 0xE0u ? 2u : p[i] < 0xF0u ? 3u : 4u;
                i += len;
            }
            CHECK(i == r.text.len);
        }
        if (r.kind == BARTAP_REC_BLOCK_START) {
            /* a straight read never starts a block mid-way (R16) */
            CHECK(r.block.char_offset == 0 && r.block.spine_index == 0);
        }
        tx_dump_record(d, &r);
    }
    CHECK(err == BARTAP_END);
    return tx_dump_end(d);
}

static void test_buffer_sizes(void)
{
    epub_str_t body = {0};
    for (size_t i = 0; i < sizeof pieces / sizeof pieces[0]; i++) {
        str_add(&body, "%s", pieces[i]);
    }
    const char *chapter = tx_doc(&doc, body.s);
    tx_build(&chapter, 1, NULL, NULL, true);
    read_checked(&other, false, 4096);
    for (uint32_t cap = BARTAP_TEXT_MIN; cap <= 70; cap++) {
        CHECK(strcmp(read_checked(&dump, false, cap), other.s) == 0);
        CHECK(strcmp(read_checked(&dump, true, cap), other.s) == 0);
    }
    CHECK(strcmp(read_checked(&dump, true, 4096), other.s) == 0);
    str_free(&body);
}

/* ---- API ---- */

static uint32_t warn_calls;
static uint16_t warn_spine;

static void on_warn(void *user, bartap_warning_t code, uint16_t spine_index, uint32_t offset)
{
    (void)user;
    (void)code;
    (void)offset;
    warn_calls++;
    warn_spine = spine_index;
}

static void test_api(void)
{
    bartap_record_t r;
    const char *chapters[2];
    epub_str_t second = {0};
    chapters[0] = tx_doc(&doc, "<p>one</p><p>bad\x01</p>");
    chapters[1] = tx_doc(&second, "<p>two</p>");
    tx_build(chapters, 2, NULL, NULL, true);

    /* errors from open stick, next repeats them */
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, BARTAP_TEXT_MIN - 1u, NULL, NULL,
                           NULL) == BARTAP_ERR_BUFFER_TOO_SMALL);
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_ERR_BUFFER_TOO_SMALL);
    CHECK(bartap_text_open(&tx_text, &tx_book, 2, tx_window, tx_buf, 64, NULL, NULL, NULL) ==
          BARTAP_ERR_ARG);
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, NULL, tx_buf, 64, NULL, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_text_open(&tx_text, NULL, 0, tx_window, tx_buf, 64, NULL, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, NULL, 64, NULL, NULL, NULL) ==
          BARTAP_ERR_ARG);
    CHECK(bartap_text_open(NULL, &tx_book, 0, tx_window, tx_buf, 64, NULL, NULL, NULL) == BARTAP_ERR_ARG);
    CHECK(bartap_text_open_memory(&tx_text, &tx_book, 0, NULL, 5, tx_buf, 64, NULL, NULL, NULL) ==
          BARTAP_ERR_ARG);
    CHECK(bartap_text_open_memory(&tx_text, &tx_book, 0, NULL, 0, tx_buf, 64, NULL, NULL, NULL) ==
          BARTAP_OK);
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_END);

    /* a ring that stores nothing is memory mode's, not stream mode's (R48) */
    bartap_rolling_t virtual_ring;
    CHECK(bartap_rolling_init(&virtual_ring, NULL, 256) == BARTAP_OK);
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, 64, &virtual_ring, NULL, NULL) ==
          BARTAP_ERR_ARG);
    CHECK(bartap_text_next(NULL, &r) == BARTAP_ERR_ARG);
    CHECK(bartap_text_warnings(NULL) == 0);

    /* the second item reads as itself, with its spine index on blocks and warnings */
    CHECK(strcmp(tx_read(&dump, 1, true, 64, HEAD_LEN, 0), "p{0}\"two\"") == 0);
    warn_calls = 0;
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, 64, NULL, on_warn, NULL) ==
          BARTAP_OK);
    tx_dump_begin(&dump, HEAD_LEN, 0);
    CHECK(tx_dump_text(&dump, &tx_text, 64) == BARTAP_END);
    CHECK(strcmp(dump.s, "p{0}\"one\" p{0}\"bad\"") == 0);
    CHECK(warn_calls == 1 && warn_spine == 0 && dump.warnings == BARTAP_WARN_CONTROL_CHAR);
    /* END again, and again */
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_END);
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_END);
    str_free(&second);
}

static void test_missing_and_damaged(void)
{
    /* an item with no entry fails when opened, not at bartap_open */
    const char *chapter = tx_doc(&doc, "<p>x</p>");
    epub_str_t manifest = {0};
    epub_begin(&tx_epub);
    epub_add_mimetype(&tx_epub);
    epub_add_container(&tx_epub, TX_OPF);
    epub_add(&tx_epub, "OEBPS/text/c0.xhtml", chapter, EPUB_STORED);
    epub_add_opf(&tx_epub, TX_OPF, "uid", "<dc:identifier id=\"uid\">u</dc:identifier>",
                 "<item id=\"c0\" href=\"text/c0.xhtml\"/><item id=\"m\" href=\"text/missing.xhtml\"/>",
                 "<itemref idref=\"c0\"/><itemref idref=\"m\"/>", NULL);
    epub_finish(&tx_epub);
    CHECK(bartap_source_memory(&tx_source, tx_epub.bytes, tx_epub.len) == BARTAP_OK);
    CHECK(bartap_open(&tx_book, &tx_source, tx_spine, TX_SPINE_MAX, NULL, 0, &tx_work, tx_window, NULL,
                      NULL) == BARTAP_OK);
    CHECK(tx_book.spine_count == 2);
    bartap_record_t r;
    CHECK(bartap_text_open(&tx_text, &tx_book, 1, tx_window, tx_buf, 64, NULL, NULL, NULL) ==
          BARTAP_ERR_CORRUPT);
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_ERR_CORRUPT);
    str_free(&manifest);

    /* damaged bytes: stream mode fails, never ENDs, and the failure sticks (R31) */
    epub_str_t body = {0};
    for (int i = 0; i < 400; i++) {
        str_add(&body, "<p>paragraph %d</p>", i);
    }
    chapter = tx_doc(&doc, body.s);
    tx_build(&chapter, 1, NULL, NULL, true);
    const epub_entry_t *e = &tx_epub.entries[2];
    tx_epub.bytes[e->local_offset + 30u + e->name_len + 5u + 3000u] ^= 0x01u;
    CHECK(bartap_text_open(&tx_text, &tx_book, 0, tx_window, tx_buf, 64, NULL, NULL, NULL) == BARTAP_OK);
    bartap_err_t err;
    while ((err = bartap_text_next(&tx_text, &r)) == BARTAP_OK) {
    }
    CHECK(err == BARTAP_ERR_CORRUPT);
    CHECK(bartap_text_next(&tx_text, &r) == BARTAP_ERR_CORRUPT);
    str_free(&body);
}

int main(void)
{
    test_whitespace();
    test_blocks();
    test_skipped();
    test_styles();
    test_lists();
    test_notes();
    test_recovery();
    test_end_of_item();
    test_depth();
    test_breaks_and_pre();
    test_images();
    test_offsets();
    test_buffer_sizes();
    test_api();
    test_missing_and_damaged();
    str_free(&doc);
    epub_free(&tx_epub);
    return CHECK_RESULT();
}
