/* XML tokenizer (R15, R21, R30, R33): tags and attributes, entities, UTF-8 with the Windows-1252
 * fallback, control characters, skipped markup, CDATA, BOMs, recovery from broken markup, limits,
 * the text API, seeking, stream mode and read failures. Expected token streams are written out in
 * xml_dump.h's format. Also built with a 1-byte read-ahead, which must change nothing (R33). */
#include "stream_fixture.h"
#include "xml_dump.h"

static bartap_xml_t xml;
static bartap_bytes_t bytes;
static char text[256];

/* Dumps input in memory mode and compares it with the expected dump. */
static void expect_at(int line, const char *input, size_t len, const char *expected)
{
    xml_dump_t d = {0};
    xml_dump_memory(&d, (const uint8_t *)input, (uint32_t)len, 64);
    if (strcmp(d.buf, expected) != 0) {
        printf("%s:%d: dump differs\n--- expected\n%s--- got\n%s", __FILE__, line, expected, d.buf);
        check_failures++;
    }
    xml_dump_free(&d);
}

#define EXPECT(input, expected) expect_at(__LINE__, input, sizeof input - 1u, expected)

static void open_memory(const char *input, size_t len, bartap_warnings_t *warnings)
{
    bt_bytes_init_memory(&bytes, (const uint8_t *)input, (uint32_t)len, NULL);
    bt_xml_init(&xml, &bytes, warnings, BARTAP_NO_SPINE);
}

static const bartap_xml_token_t *next_token(void)
{
    const bartap_xml_token_t *t = NULL;
    CHECK(bt_xml_next(&xml, &t) == BARTAP_OK);
    CHECK(t != NULL);
    return t;
}

/* ---- Tags and attributes ---- */

static void test_tags(void)
{
    EXPECT("<p>Hi</p>",
           "S 0-3 p\n"
           "T 3-5 \"Hi\"\n"
           "E 5-9 p\n"
           "END\n");
    EXPECT("a<br/>b",
           "T 0-1 \"a\"\n"
           "S 1-6 br /\n"
           "T 6-7 \"b\"\n"
           "END\n");
    /* R46: the local name, ASCII-folded. Attribute names keep their case. */
    EXPECT("<HTML:P Class=\"x\">",
           "S 0-18 p Class=\"x\"\n"
           "END\n");
    /* 32 bytes fit; 33 can't match a known element, so the name is empty. */
    EXPECT("<abcdefghijabcdefghijabcdefghij12><abcdefghijabcdefghijabcdefghij123>"
           "</x:abcdefghijabcdefghijabcdefghij12>",
           "S 0-34 abcdefghijabcdefghijabcdefghij12\n"
           "S 34-69 \n"
           "E 69-106 abcdefghijabcdefghijabcdefghij12\n"
           "END\n");
    EXPECT("", "END\n");
}

static void test_attributes(void)
{
    /* Quotes of both kinds, spaces around '=', entities decoded, and HTML's forms with warnings. */
    EXPECT("<p a=\"1\" b='2' c = \"3\" d=4 e f=\"&amp;&lt;&#65;&bogus;&\">",
           "S 0-56 p a=\"1\" b=\"2\" c=\"3\" d=\"4\" e=\"\" f=\"&<A&bogus;&\"\n"
           "  W malformed@23 spine=7\n"
           "  W malformed@27 spine=7\n"
           "  W entity@46 spine=7\n"
           "  W malformed@53 spine=7\n"
           "END\n");
    /* A name takes its first byte whatever it is, so nothing stalls. */
    EXPECT("<p ==x \"q\"='v'>",
           "S 0-15 p ==\"x\" \\\"q\\\"=\"v\"\n"
           "  W malformed@3 spine=7\n"
           "END\n");

    /* Lookup: by full name with its prefix, ASCII-folded, first match wins, only on a tag. */
    static const char doc[] = "<aside epub:type='footnote' ID=a id=b>x</aside>";
    const char *value;
    uint32_t value_len;
    open_memory(doc, sizeof doc - 1u, NULL);
    CHECK(next_token()->kind == BARTAP_XML_START_TAG);
    CHECK(bt_xml_attr(&xml, "epub:type", 9, &value, &value_len));
    CHECK(value_len == 8 && memcmp(value, "footnote", 8) == 0);
    CHECK(!bt_xml_attr(&xml, "type", 4, &value, &value_len));
    CHECK(bt_xml_attr(&xml, "id", 2, &value, &value_len));
    CHECK(value_len == 1 && value[0] == 'a');
    CHECK(!bt_xml_attr(&xml, "i", 1, &value, &value_len));
    CHECK(next_token()->kind == BARTAP_XML_TEXT);
    CHECK(!bt_xml_attr(&xml, "id", 2, &value, &value_len));
    CHECK(next_token()->kind == BARTAP_XML_END_TAG);
    CHECK(!bt_xml_attr(&xml, "id", 2, &value, &value_len));
}

/* R33: storage and count limits drop whole attributes, and later ones that fit are still kept. */
static void test_attribute_limits(void)
{
    static char doc[2048];
    bartap_warnings_t w = {0};
    const char *value;
    uint32_t value_len;
    size_t len = 0;

    len += (size_t)sprintf(doc + len, "<p a=\"");
    memset(doc + len, 'x', 1000);
    len += 1000;
    len += (size_t)sprintf(doc + len, "\" b=\"");
    memset(doc + len, 'y', 100);
    len += 100;
    len += (size_t)sprintf(doc + len, "\" c=\"z\">");

    open_memory(doc, len, &w);
    next_token();
    CHECK(w.bits == BARTAP_WARN_ATTR_TRUNCATED);
    CHECK(xml.attr_count == 2);
    CHECK(bt_xml_attr(&xml, "a", 1, &value, &value_len) && value_len == 1000);
    CHECK(!bt_xml_attr(&xml, "b", 1, &value, &value_len));
    CHECK(bt_xml_attr(&xml, "c", 1, &value, &value_len) && value_len == 1 && value[0] == 'z');
    CHECK(xml.attr_used == 1u + 1000u + 1u + 1u);

    /* Exactly full is fine. */
    len = (size_t)sprintf(doc, "<p a=\"");
    memset(doc + len, 'x', BARTAP_ATTR_MAX - 1u);
    len += BARTAP_ATTR_MAX - 1u;
    len += (size_t)sprintf(doc + len, "\">");
    w.bits = 0;
    open_memory(doc, len, &w);
    next_token();
    CHECK(w.bits == 0 && xml.attr_count == 1 && xml.attr_used == BARTAP_ATTR_MAX);

    /* The seventeenth attribute is dropped. */
    len = (size_t)sprintf(doc, "<p");
    for (int i = 0; i < 17; i++) {
        len += (size_t)sprintf(doc + len, " a%d=\"%d\"", i, i);
    }
    len += (size_t)sprintf(doc + len, ">");
    w.bits = 0;
    open_memory(doc, len, &w);
    next_token();
    CHECK(w.bits == BARTAP_WARN_ATTR_TRUNCATED);
    CHECK(xml.attr_count == BARTAP_XML_ATTRS_MAX);
    CHECK(bt_xml_attr(&xml, "a15", 3, &value, &value_len));
    CHECK(!bt_xml_attr(&xml, "a16", 3, &value, &value_len));
}

/* ---- Characters (R21, R30) ---- */

static void test_entities(void)
{
    EXPECT("&amp;&lt;&gt;&quot;&apos;&nbsp;&eacute;&mdash;&AMP;",
           "T 0-51 \"&<>\\\"'\\xC2\\xA0\\xC3\\xA9\\xE2\\x80\\x94&AMP;\"\n"
           "  W entity@46 spine=7\n"
           "END\n");
    /* No character at 0, a surrogate or past U+10FFFF: the replacement character. */
    EXPECT("&#65;&#x41;&#X41;&#0;&#xD800;&#x110000;&#99999999999999;&#;&#x;&#1a;",
           "T 0-68 \"AAA\\xEF\\xBF\\xBD\\xEF\\xBF\\xBD\\xEF\\xBF\\xBD\\xEF\\xBF\\xBD&#;&#x;&#1a;\"\n"
           "  W entity@56 spine=7\n"
           "  W entity@59 spine=7\n"
           "  W entity@63 spine=7\n"
           "END\n");
    /* No ';' in reach: literal, and the byte that stopped it is untouched. */
    EXPECT("AT&T & co &zz",
           "T 0-13 \"AT&T & co &zz\"\n"
           "  W malformed@2 spine=7\n"
           "  W malformed@5 spine=7\n"
           "  W malformed@10 spine=7\n"
           "END\n");
    /* BARTAP_XML_ENTITY_MAX: a 32-byte name is looked up; a 33rd byte means no ';' in reach. */
    EXPECT("&abcdefghijabcdefghijabcdefghij12;&abcdefghijabcdefghijabcdefghij123;",
           "T 0-69 \"&abcdefghijabcdefghijabcdefghij12;&abcdefghijabcdefghijabcdefghij123;\"\n"
           "  W entity@0 spine=7\n"
           "  W malformed@34 spine=7\n"
           "END\n");

    char out[BARTAP_TEXT_MIN];
    CHECK(bt_entity_decode("#x10FFFF", 8, out) == 4 && memcmp(out, "\xF4\x8F\xBF\xBF", 4) == 0);
    CHECK(bt_entity_decode("#1114111", 8, out) == 4 && memcmp(out, "\xF4\x8F\xBF\xBF", 4) == 0);
    CHECK(bt_entity_decode("hellip", 6, out) == 3 && memcmp(out, "\xE2\x80\xA6", 3) == 0);
    CHECK(bt_entity_decode("yuml", 4, out) == 2 && memcmp(out, "\xC3\xBF", 2) == 0);
    CHECK(bt_entity_decode("Yuml", 4, out) == 2 && memcmp(out, "\xC5\xB8", 2) == 0);
    CHECK(bt_entity_decode("amp", 2, out) == 0);
    CHECK(bt_entity_decode("ampx", 4, out) == 0);
    CHECK(bt_entity_decode("", 0, out) == 0);
    CHECK(bt_entity_decode("#", 1, out) == 0);
    CHECK(bt_entity_decode("#xG", 3, out) == 0);
}

static void test_utf8_in_text(void)
{
    EXPECT("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80",
           "T 0-9 \"\\xC3\\xA9\\xE2\\x82\\xAC\\xF0\\x9F\\x98\\x80\"\n"
           "END\n");
    /* Overlong, overlong, surrogate, past U+10FFFF, lone continuation, cut short by markup: each
     * falls back one lead byte at a time. */
    EXPECT("\xC0\x80|\xE0\x80\x80|\xED\xA0\x80|\xF4\x90\x80\x80|\x80|\xC3<b>",
           "T 0-19 \"\\xC3\\x80\\xE2\\x82\\xAC|\\xC3\\xA0\\xE2\\x82\\xAC\\xE2\\x82\\xAC|\\xC3\\xAD\\xC2\\xA0"
           "\\xE2\\x82\\xAC|\\xC3\\xB4\\xEF\\xBF\\xBD\\xE2\\x82\\xAC\\xE2\\x82\\xAC|\\xE2\\x82\\xAC|\\xC3\\x83\"\n"
           "  W utf8@0 spine=7\n"
           "  W utf8@1 spine=7\n"
           "  W utf8@3 spine=7\n"
           "  W utf8@4 spine=7\n"
           "  W utf8@5 spine=7\n"
           "  W utf8@7 spine=7\n"
           "  W utf8@8 spine=7\n"
           "  W utf8@9 spine=7\n"
           "  W utf8@11 spine=7\n"
           "  W utf8@12 spine=7\n"
           "  W utf8@13 spine=7\n"
           "  W utf8@14 spine=7\n"
           "  W utf8@16 spine=7\n"
           "  W utf8@18 spine=7\n"
           "S 19-22 b\n"
           "END\n");
    /* Windows-1252: 0x80-0x9F are punctuation, never C1 controls; undefined bytes are U+FFFD. */
    EXPECT("\x80\x81\x9F\xE9\xFF",
           "T 0-5 \"\\xE2\\x82\\xAC\\xEF\\xBF\\xBD\\xC5\\xB8\\xC3\\xA9\\xC3\\xBF\"\n"
           "  W utf8@0 spine=7\n"
           "  W utf8@1 spine=7\n"
           "  W utf8@2 spine=7\n"
           "  W utf8@3 spine=7\n"
           "  W utf8@4 spine=7\n"
           "END\n");
    /* Cut short by the source's end. */
    EXPECT("a\xE2\x82",
           "T 0-3 \"a\\xC3\\xA2\\xE2\\x80\\x9A\"\n"
           "  W utf8@1 spine=7\n"
           "  W utf8@2 spine=7\n"
           "END\n");
}

static void test_control_chars(void)
{
    /* R21: dropped whether raw, UTF-8 or a reference. Tab, LF and CR stay. */
    EXPECT("a\x01"
           "b\tc\nd\re\x7F"
           "f\xC2\x85g&#x85;h&#1;i&#9;j",
           "T 0-31 \"ab\\x09c\\x0Ad\\x0Defghi\\x09j\"\n"
           "  W control@1 spine=7\n"
           "  W control@9 spine=7\n"
           "  W control@11 spine=7\n"
           "  W control@14 spine=7\n"
           "  W control@21 spine=7\n"
           "END\n");
}

/* Reference decoder, independent of the library's: whether p[0..n) is one well-formed character. */
static bool ref_valid(const uint8_t *p, uint32_t n)
{
    static const uint8_t lead_mask[5] = {0, 0x80, 0xE0, 0xF0, 0xF8};
    static const uint8_t lead_bits[5] = {0, 0x00, 0xC0, 0xE0, 0xF0};
    static const uint32_t min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
    if ((p[0] & lead_mask[n]) != lead_bits[n]) {
        return false;
    }
    uint32_t cp = p[0] & (uint8_t)~lead_mask[n];
    for (uint32_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    return cp >= min_cp[n] && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

/* Windows-1252 per its published table, written out again here rather than shared. */
static uint32_t ref_1252(uint8_t b)
{
    static const uint32_t high[32] = {
        0x20AC, 0, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160,
        0x2039, 0x152, 0, 0x17D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
        0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0, 0x17E, 0x178};
    if (b >= 0x80 && b <= 0x9F) {
        return high[b - 0x80] != 0 ? high[b - 0x80] : 0xFFFD;
    }
    return b;
}

static uint32_t ref_cp(const char *out, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)out;
    static const uint8_t mask[5] = {0, 0x7F, 0x1F, 0x0F, 0x07};
    uint32_t cp = p[0] & mask[len];
    for (uint32_t i = 1; i < len; i++) {
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    return cp;
}

/* Checks one complete input of 4 bytes against the reference. */
static void check_decode(const uint8_t *p)
{
    char out[BARTAP_TEXT_MIN];
    uint32_t out_len = 0;
    uint32_t used = bt_utf8_decode(p, 4, out, &out_len);
    uint32_t want = 0;
    for (uint32_t n = 1; n <= 4 && want == 0; n++) {
        if (ref_valid(p, n)) {
            want = n;
        }
    }
    if (want != 0) {
        CHECK(used == want && out_len == want && memcmp(out, p, want) == 0);
    } else {
        CHECK(used == 1 && ref_cp(out, out_len) == ref_1252(p[0]));
    }
}

/* Exhaustive over every lead and second byte, with third and fourth bytes covering each class. */
static void test_utf8_decode(void)
{
    static const uint8_t tails[] = {0x00, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xFF};
    uint8_t p[4];
    char out[BARTAP_TEXT_MIN];
    uint32_t out_len;

    for (uint32_t b0 = 0; b0 < 256; b0++) {
        for (uint32_t b1 = 0; b1 < 256; b1++) {
            for (size_t t2 = 0; t2 < sizeof tails; t2++) {
                for (size_t t3 = 0; t3 < sizeof tails; t3++) {
                    p[0] = (uint8_t)b0;
                    p[1] = (uint8_t)b1;
                    p[2] = tails[t2];
                    p[3] = tails[t3];
                    check_decode(p);
                }
            }
        }
    }

    /* 0 means "valid so far, cut short": only then, for every prefix of every two-byte start. */
    CHECK(bt_utf8_decode(p, 0, out, &out_len) == 0);
    for (uint32_t b0 = 0xC0; b0 < 256; b0++) {
        p[0] = (uint8_t)b0;
        uint32_t need = b0 >= 0xF0 ? 4 : b0 >= 0xE0 ? 3 : 2;
        bool completable = false;
        for (uint32_t c = 0x80; c <= 0xBF && !completable; c++) {
            uint8_t q[4] = {p[0], (uint8_t)c, 0x80, 0x80};
            completable = need <= 4 && ref_valid(q, need);
        }
        CHECK((bt_utf8_decode(p, 1, out, &out_len) == 0) == completable);
        for (uint32_t b1 = 0; b1 < 256 && need >= 3; b1++) {
            p[1] = (uint8_t)b1;
            uint8_t q[4] = {p[0], p[1], 0x80, 0x80};
            CHECK((bt_utf8_decode(p, 2, out, &out_len) == 0) == ref_valid(q, need));
        }
    }
}

/* ---- Skipped markup, CDATA, BOMs ---- */

static void test_skipped(void)
{
    /* "--" inside is tolerated, and "<!-->" isn't an empty comment: it runs to the next "-->". */
    EXPECT("a<!-- x -- y -->b<!---->c<!-->d-->e",
           "T 0-1 \"a\"\n"
           "T 16-17 \"b\"\n"
           "T 24-25 \"c\"\n"
           "T 34-35 \"e\"\n"
           "END\n");
    EXPECT("a<?xml version='1.0'?>b<!DOCTYPE html [<!ENTITY x \"]>\">]>c",
           "T 0-1 \"a\"\n"
           "T 22-23 \"b\"\n"
           "T 57-58 \"c\"\n"
           "END\n");
    /* Warnings from skipped markup sit between the tokens around it. */
    EXPECT("a<!-x>b<![FOO[c]>d",
           "T 0-1 \"a\"\n"
           "  W malformed@1 spine=7\n"
           "T 6-7 \"b\"\n"
           "  W malformed@7 spine=7\n"
           "T 17-18 \"d\"\n"
           "END\n");
}

static void test_cdata(void)
{
    EXPECT("<![CDATA[<i>&amp;</i>]]>x<![CDATA[a]]]>b<![CDATA[]]><![CDATA[]]x]]>",
           "T 0-24 \"<i>&amp;</i>\"\n"
           "T 24-25 \"x\"\n"
           "T 25-39 \"a]\"\n"
           "T 39-40 \"b\"\n"
           "T 40-52 \"\"\n"
           "T 52-67 \"]]x\"\n"
           "END\n");
    /* Unterminated: held brackets are text after all. */
    EXPECT("<![CDATA[ab]]",
           "T 0-13 \"ab]]\"\n"
           "  W malformed@13 spine=7\n"
           "END\n");
    EXPECT("<![CDATA[ab]",
           "T 0-12 \"ab]\"\n"
           "  W malformed@12 spine=7\n"
           "END\n");
    /* Control characters are still dropped inside CDATA. */
    EXPECT("<![CDATA[a\x01]]>",
           "T 0-14 \"a\"\n"
           "  W control@10 spine=7\n"
           "END\n");
}

static void test_bom(void)
{
    EXPECT("\xEF\xBB\xBF<p>",
           "S 3-6 p\n"
           "END\n");
    /* A partial BOM is text, decoded like any other bytes. */
    EXPECT("\xEF\xBB<p>",
           "T 0-2 \"\\xC3\\xAF\\xC2\\xBB\"\n"
           "  W utf8@0 spine=7\n"
           "  W utf8@1 spine=7\n"
           "S 2-5 p\n"
           "END\n");
    EXPECT("\xEF\xBBx",
           "T 0-3 \"\\xC3\\xAF\\xC2\\xBBx\"\n"
           "  W utf8@0 spine=7\n"
           "  W utf8@1 spine=7\n"
           "END\n");
    EXPECT("\xFEx",
           "T 0-2 \"\\xC3\\xBEx\"\n"
           "  W utf8@0 spine=7\n"
           "END\n");
    /* A valid UTF-8 character starting with the BOM's lead byte. */
    EXPECT("\xEF\xBC\x81",
           "T 0-3 \"\\xEF\\xBC\\x81\"\n"
           "END\n");
    /* R30: UTF-16 can't be read, in either byte order, and it sticks. */
    EXPECT("\xFF\xFE<\x00p\x00>", "ERR BARTAP_ERR_UNSUPPORTED\n");
    EXPECT("\xFE\xFF\x00<", "ERR BARTAP_ERR_UNSUPPORTED\n");
    open_memory("\xFF\xFE", 2, NULL);
    const bartap_xml_token_t *t;
    CHECK(bt_xml_next(&xml, &t) == BARTAP_ERR_UNSUPPORTED);
    CHECK(bt_xml_next(&xml, &t) == BARTAP_ERR_UNSUPPORTED);
    uint32_t got;
    CHECK(bt_xml_text(&xml, text, sizeof text, &got) == BARTAP_ERR_UNSUPPORTED);
}

/* ---- Recovery (R30) ---- */

static void test_recovery(void)
{
    EXPECT("a < b <",
           "T 0-2 \"a \"\n"
           "T 2-6 \"< b \"\n"
           "  W malformed@2 spine=7\n"
           "T 6-7 \"<\"\n"
           "  W malformed@6 spine=7\n"
           "END\n");
    EXPECT("</ p>x</p junk>y</p >",
           "  W malformed@0 spine=7\n"
           "T 5-6 \"x\"\n"
           "E 6-15 p\n"
           "  W malformed@6 spine=7\n"
           "T 15-16 \"y\"\n"
           "E 16-21 p\n"
           "END\n");
    /* A tag left open ends at the next '<', so it can't swallow what follows. */
    EXPECT("<p <b>x",
           "S 0-3 p\n"
           "  W malformed@3 spine=7\n"
           "S 3-6 b\n"
           "T 6-7 \"x\"\n"
           "END\n");
    EXPECT("<p / a=1/>",
           "S 0-10 p a=\"1/\"\n"
           "  W malformed@3 spine=7\n"
           "  W malformed@5 spine=7\n"
           "END\n");
    /* The source ending inside markup drops it. */
    EXPECT("x<p a=\"unterminated",
           "T 0-1 \"x\"\n"
           "  W malformed@4 spine=7\n"
           "END\n");
    EXPECT("x<p a=1",
           "T 0-1 \"x\"\n"
           "  W malformed@4 spine=7\n"
           "  W malformed@1 spine=7\n"
           "END\n");
    EXPECT("x</p",
           "T 0-1 \"x\"\n"
           "  W malformed@1 spine=7\n"
           "END\n");
    EXPECT("x<!-- open",
           "T 0-1 \"x\"\n"
           "  W malformed@1 spine=7\n"
           "END\n");
    EXPECT("x<?pi",
           "T 0-1 \"x\"\n"
           "  W malformed@1 spine=7\n"
           "END\n");
    EXPECT("x<!DOCTYPE [",
           "T 0-1 \"x\"\n"
           "  W malformed@1 spine=7\n"
           "END\n");
}

/* ---- The text API ---- */

static void test_text_api(void)
{
    static const char doc[] = "ab\xE2\x82\xAC<p>";
    const bartap_xml_token_t *t;
    uint32_t got;

    open_memory(doc, sizeof doc - 1u, NULL);
    t = next_token();
    CHECK(t->kind == BARTAP_XML_TEXT && t->offset == 0);
    CHECK(t->end_offset == 0); /* not known yet */

    /* Whole characters only: "ab" leaves no room for the 3-byte one. */
    CHECK(bt_xml_text(&xml, text, 4, &got) == BARTAP_OK && got == 2 && memcmp(text, "ab", 2) == 0);
    CHECK(bt_xml_text(&xml, text, 2, &got) == BARTAP_ERR_BUFFER_TOO_SMALL && got == 0);
    CHECK(bt_xml_text(&xml, text, 3, &got) == BARTAP_OK && got == 3);
    CHECK(memcmp(text, "\xE2\x82\xAC", 3) == 0);
    CHECK(bt_xml_text(&xml, text, 3, &got) == BARTAP_OK && got == 0);
    CHECK(t->end_offset == 5);
    CHECK(bt_xml_text(&xml, text, 3, &got) == BARTAP_OK && got == 0); /* the end repeats */

    /* Only a text token has text. */
    t = next_token();
    CHECK(t->kind == BARTAP_XML_START_TAG);
    CHECK(bt_xml_text(&xml, text, sizeof text, &got) == BARTAP_ERR_ARG);
    CHECK(bt_xml_next(&xml, &t) == BARTAP_END && t == NULL);
    CHECK(bt_xml_next(&xml, &t) == BARTAP_END);
}

/* Text the caller skips is still run through, so warnings don't depend on whether it was read. */
static void test_unread_text(void)
{
    static const char doc[] = "a\x01&bogus;b<p>c\xFF<i>";
    bartap_warnings_t w = {0};
    uint32_t got;

    open_memory(doc, sizeof doc - 1u, &w);
    CHECK(next_token()->kind == BARTAP_XML_TEXT);
    const bartap_xml_token_t *t = next_token();
    CHECK(t->kind == BARTAP_XML_START_TAG && t->offset == 10);
    CHECK(w.bits == (BARTAP_WARN_CONTROL_CHAR | BARTAP_WARN_UNKNOWN_ENTITY));

    /* Partly read, with a character held back for want of room. */
    t = next_token();
    CHECK(t->kind == BARTAP_XML_TEXT);
    CHECK(bt_xml_text(&xml, text, 1, &got) == BARTAP_OK && got == 1 && text[0] == 'c');
    CHECK(bt_xml_text(&xml, text, 1, &got) == BARTAP_ERR_BUFFER_TOO_SMALL);
    t = next_token();
    CHECK(t->kind == BARTAP_XML_START_TAG && t->name_len == 1 && t->name[0] == 'i');
    CHECK((w.bits & BARTAP_WARN_INVALID_UTF8) != 0);
}

/* ---- Seeking (R48) ---- */

/* From every token's offset, the rest matches a straight read. */
static void check_seeks(const char *doc, size_t len)
{
    xml_dump_t straight = {0};
    xml_dump_memory(&straight, (const uint8_t *)doc, (uint32_t)len, 64);

    for (size_t k = 0; k < straight.count; k++) {
        xml_dump_t d = {0};
        bartap_warnings_t w = dump_warnings(&d);
        const bartap_xml_token_t *t;
        bt_bytes_init_memory(&bytes, (const uint8_t *)doc, (uint32_t)len, NULL);
        bt_xml_init(&xml, &bytes, &w, 7);
        /* half-read a token first, so the seek has state to throw away */
        if (bt_xml_next(&xml, &t) == BARTAP_OK && t->kind == BARTAP_XML_TEXT) {
            uint32_t got;
            (void)bt_xml_text(&xml, text, 4, &got);
        }
        d.pending_len = 0;
        CHECK(bt_xml_seek(&xml, straight.offsets[k]) == BARTAP_OK);
        xml_dump_run(&d, &xml, 64);
        CHECK(d.len == straight.len - straight.starts[k]);
        CHECK(d.len == straight.len - straight.starts[k] &&
              memcmp(d.buf, straight.buf + straight.starts[k], d.len) == 0);
        xml_dump_free(&d);
    }
    xml_dump_free(&straight);
}

static void test_seek(void)
{
    static const char doc1[] = "\xEF\xBB\xBF<p a='1'>x&amp;y</p><![CDATA[z]]>a < b<br/>\xE9";
    static const char doc2[] = "\xEF\xBB<p>";
    static const char doc3[] = "<!-- c -->t<?pi?><x:Y q=\"&lt;\"/>";
    check_seeks(doc1, sizeof doc1 - 1u);
    check_seeks(doc2, sizeof doc2 - 1u);
    check_seeks(doc3, sizeof doc3 - 1u);

    /* Past the source is the byte source's refusal, and nothing moves. */
    open_memory(doc3, sizeof doc3 - 1u, NULL);
    CHECK(next_token()->kind == BARTAP_XML_TEXT);
    CHECK(bt_xml_seek(&xml, sizeof doc3) == BARTAP_ERR_ARG);
    CHECK(next_token()->kind == BARTAP_XML_START_TAG);
}

/* ---- Stream mode and read failures (R11, R15, R37) ---- */

static uint8_t window[BARTAP_WINDOW_SIZE];
static bartap_stream_t stream;
static char big[60000];
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

static size_t make_big(void)
{
    size_t len = 0;
    while (len + 40 < sizeof big) {
        len += (size_t)sprintf(big + len, "<p class=\"c\">caf\xC3\xA9 &amp; more</p>\n");
    }
    return len;
}

static void test_stream_mode(void)
{
    size_t len = make_big();
    xml_dump_t memory = {0};
    xml_dump_t streamed = {0};
    fixture_t f;

    xml_dump_memory(&memory, (const uint8_t *)big, (uint32_t)len, 64);
    fixture_build(&f, (const uint8_t *)big, (uint32_t)len, 0, (uint32_t)len,
                  bt_crc32(0, (const uint8_t *)big, len));
    CHECK(bt_stream_open(&stream, &f.source, &f.entry, window) == BARTAP_OK);
    bt_bytes_init_stream(&bytes, &stream, NULL);
    bartap_warnings_t w = dump_warnings(&streamed);
    bt_xml_init(&xml, &bytes, &w, 7);
    xml_dump_run(&streamed, &xml, 64);
    CHECK(streamed.len == memory.len && memcmp(streamed.buf, memory.buf, memory.len) == 0);
    xml_dump_free(&memory);
    xml_dump_free(&streamed);

    /* A read failure mid-way sticks, for tokens and text alike. */
    for (int ok_reads = 1; ok_reads < 6; ok_reads++) {
        bartap_source_t failing = f.source;
        const bartap_xml_token_t *t;
        bartap_err_t err;
        uint32_t got;

        failing.read = failing_read;
        io_bytes = f.bytes;
        io_reads_left = ok_reads;
        CHECK(bt_stream_open(&stream, &failing, &f.entry, window) == BARTAP_OK);
        bt_bytes_init_stream(&bytes, &stream, NULL);
        bt_xml_init(&xml, &bytes, NULL, BARTAP_NO_SPINE);
        do {
            err = bt_xml_next(&xml, &t);
            while (err == BARTAP_OK && t->kind == BARTAP_XML_TEXT) {
                err = bt_xml_text(&xml, text, sizeof text, &got);
                if (err != BARTAP_OK || got == 0) {
                    break;
                }
            }
        } while (err == BARTAP_OK);
        CHECK(err == BARTAP_ERR_IO);
        CHECK(bt_xml_next(&xml, &t) == BARTAP_ERR_IO);
        CHECK(bt_xml_text(&xml, text, sizeof text, &got) == BARTAP_ERR_IO);
    }
    fixture_free(&f);
}

/* No warnings object is fine; the spine index reaches the callback. */
static void test_warning_plumbing(void)
{
    static const char doc[] = "&bogus;\x01<p a>";
    const bartap_xml_token_t *t;
    uint32_t got;

    open_memory(doc, sizeof doc - 1u, NULL);
    CHECK(next_token()->kind == BARTAP_XML_TEXT);
    CHECK(bt_xml_text(&xml, text, sizeof text, &got) == BARTAP_OK && got == 7);
    CHECK(next_token()->kind == BARTAP_XML_START_TAG);
    CHECK(bt_xml_next(&xml, &t) == BARTAP_END);
}

int main(void)
{
    test_tags();
    test_attributes();
    test_attribute_limits();
    test_entities();
    test_utf8_in_text();
    test_control_chars();
    test_utf8_decode();
    test_skipped();
    test_cdata();
    test_bom();
    test_recovery();
    test_text_api();
    test_unread_text();
    test_seek();
    test_stream_mode();
    test_warning_plumbing();
    return CHECK_RESULT();
}
