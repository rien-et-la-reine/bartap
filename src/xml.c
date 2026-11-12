/* Layer 2 (R3): rolling buffer, byte source and XML. Depends only on zip.c. */
#include <string.h>

#include "internal.h"

/* ---- Rolling buffer (R48) ---- */

// Byte offset o lives at ring[o & (size - 1)], so the ring holds the last size bytes delivered. Down here
// rather than with the checkpoints, so the byte source can feed it without reaching up a layer.

// Sets up a rolling buffer, empty and with no checkpoints. A NULL ring with a non-zero size is
// memory mode's virtual window: offsets are counted, no bytes are stored.
bartap_err_t bartap_rolling_init(bartap_rolling_t *rolling, uint8_t *ring, uint32_t size)
{
    if (size == 0 || (size & (size - 1u)) != 0) {
        // no window at all, or not a power of two. a mask keeps division off small cores
        return BARTAP_ERR_ARG;
    }
    rolling->ring = ring;
    rolling->size = size;
    rolling->delivered = 0;
    // the text layer owns these, but a fresh ring can't have any yet
    rolling->checkpoints[0].valid = 0;
    rolling->checkpoints[1].valid = 0;
    rolling->next_checkpoint = 0;
    return BARTAP_OK;
}

// Adds freshly delivered bytes. Only the last size bytes of a long append can survive, so only
// those are copied.
void bt_ring_append(bartap_rolling_t *rolling, const uint8_t *data, uint32_t len)
{
    uint32_t end = rolling->delivered + len;

    if (rolling->ring != NULL) {
        // skip what would be overwritten before the append finishes
        if (len > rolling->size) {
            data += len - rolling->size;
            len = rolling->size;
        }
        // copy up to the wrap, then the rest from the ring's start
        uint32_t pos = (end - len) & (rolling->size - 1u);
        uint32_t first = rolling->size - pos;
        if (first > len) {
            first = len;
        }
        memcpy(rolling->ring + pos, data, first);
        if (len > first) {
            memcpy(rolling->ring, data + first, len - first);
        }
    }
    // virtual window or real, the count moves the same
    rolling->delivered = end;
}

// Copies bytes back out by their offset in the source. len 0 just asks if offset is still held.
bartap_err_t bt_ring_read(const bartap_rolling_t *rolling, uint32_t offset, uint8_t *buf,
                          uint32_t len)
{
    if (offset > rolling->delivered || len > rolling->delivered - offset) {
        // asks for bytes not delivered yet, the caller's mistake
        return BARTAP_ERR_ARG;
    }
    if (rolling->ring == NULL || rolling->delivered - offset > rolling->size) {
        // virtual window stores nothing, or the bytes have been overwritten
        return BARTAP_END;
    }
    if (len == 0) {
        // buf may be NULL here, memcpy can't take that
        return BARTAP_OK;
    }
    // copy up to the wrap, then the rest from the ring's start
    uint32_t pos = offset & (rolling->size - 1u);
    uint32_t first = rolling->size - pos;
    if (first > len) {
        first = len;
    }
    memcpy(buf, rolling->ring + pos, first);
    memcpy(buf + first, rolling->ring, len - first);
    return BARTAP_OK;
}

/* ---- Byte source (R15) ---- */

// position is the next source offset to deliver. In stream mode it can sit behind the stream (a
// replay from the ring or the inflate window) or ahead of it (bytes still to skip after a seek); the
// stream itself only moves forward, and only bytes it delivers fresh go into the ring (R48).

// Forgets what the ring holds: it indexes one pass over one source.
static void bytes_reset_ring(bartap_bytes_t *bytes)
{
    if (bytes->rolling != NULL) {
        bytes->rolling->delivered = 0;
        // checkpoints point into what was just forgotten
        bytes->rolling->checkpoints[0].valid = 0;
        bytes->rolling->checkpoints[1].valid = 0;
        bytes->rolling->next_checkpoint = 0;
    }
}

// Reads an entry stream fresh from bt_stream_open, so the ring and the stream start together.
void bt_bytes_init_stream(bartap_bytes_t *bytes, bartap_stream_t *stream, bartap_rolling_t *rolling)
{
    bytes->stream = stream;
    bytes->memory = NULL;
    bytes->memory_size = 0;
    bytes->rolling = rolling;
    bytes->position = 0;
    bytes_reset_ring(bytes);
}

// Reads an already-inflated chapter from RAM (R11). The ring, if any, only counts offsets here.
void bt_bytes_init_memory(bartap_bytes_t *bytes, const uint8_t *data, uint32_t size,
                          bartap_rolling_t *rolling)
{
    bytes->stream = NULL;
    bytes->memory = data;
    bytes->memory_size = size;
    bytes->rolling = rolling;
    bytes->position = 0;
    bytes_reset_ring(bytes);
}

// Memory mode: a copy, and the ring's count catches up with anything new.
static bartap_err_t bytes_read_memory(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap,
                                      uint32_t *got)
{
    if (bytes->position >= bytes->memory_size) {
        // nothing left, done
        return BARTAP_END;
    }
    uint32_t len = bytes->memory_size - bytes->position;
    if (len > cap) {
        len = cap;
    }
    memcpy(buf, bytes->memory + bytes->position, len);
    bytes->position += len;
    *got = len;

    // feed the ring only bytes past what it has seen, so a re-read after a seek isn't counted twice
    bartap_rolling_t *rolling = bytes->rolling;
    if (rolling != NULL && bytes->position > rolling->delivered) {
        bt_ring_append(rolling, bytes->memory + rolling->delivered,
                       bytes->position - rolling->delivered);
    }
    return BARTAP_OK;
}

// Stream mode, behind the stream: the bytes come back from the ring or the inflate window, never the
// card, and aren't folded into the CRC or appended again (R48).
static bartap_err_t bytes_replay(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    uint32_t len = bytes->stream->delivered - bytes->position;
    if (len > cap) {
        len = cap;
    }
    // ring first, it usually reaches further back than the window
    bartap_err_t err = BARTAP_END;
    if (bytes->rolling != NULL) {
        err = bt_ring_read(bytes->rolling, bytes->position, buf, len);
    }
    if (err == BARTAP_END) {
        // not in the ring, try the inflate window
        err = bt_stream_reread(bytes->stream, bytes->position, buf, len);
    }
    if (err != BARTAP_OK) {
        // gone from both, which a seek should have ruled out. pass it up
        return err;
    }
    bytes->position += len;
    *got = len;
    return BARTAP_OK;
}

// Stream mode, level with or ahead of the stream: fresh bytes, which go into the ring. Bytes before
// position are skipped, using buf as scratch.
static bartap_err_t bytes_read_live(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    bartap_stream_t *stream = bytes->stream;

    for (;;) {
        // ahead of the stream, only take what's needed to catch up
        uint32_t want = cap;
        if (bytes->position > stream->delivered && bytes->position - stream->delivered < want) {
            want = bytes->position - stream->delivered;
        }
        uint32_t len;
        uint32_t start = stream->delivered;
        bartap_err_t err = bt_stream_read(stream, buf, want, &len);
        if (err != BARTAP_OK) {
            // the end or a failure, pass it up. an end while skipping means the seek was past it
            return err;
        }
        if (bytes->rolling != NULL) {
            bt_ring_append(bytes->rolling, buf, len);
        }
        if (start >= bytes->position) {
            // caught up, these are the bytes wanted
            bytes->position += len;
            *got = len;
            return BARTAP_OK;
        }
        // still skipping, read again. want kept this from overshooting position
    }
}

// Delivers the next bytes from position on; BARTAP_END when the source is finished.
bartap_err_t bt_bytes_read(bartap_bytes_t *bytes, uint8_t *buf, uint32_t cap, uint32_t *got)
{
    *got = 0;
    if (cap == 0) {
        return BARTAP_ERR_ARG;
    }
    if (bytes->stream == NULL) {
        return bytes_read_memory(bytes, buf, cap, got);
    }
    if (bytes->position < bytes->stream->delivered) {
        return bytes_replay(bytes, buf, cap, got);
    }
    return bytes_read_live(bytes, buf, cap, got);
}

// Moves to a source offset. Behind the stream, the bytes must still be in the ring or the inflate
// window, or the stream rewinds and the ring starts over (R48). Ahead, the next read skips forward.
bartap_err_t bt_bytes_seek(bartap_bytes_t *bytes, uint32_t offset)
{
    bartap_stream_t *stream = bytes->stream;

    if (stream == NULL) {
        if (offset > bytes->memory_size) {
            // past the chapter's end
            return BARTAP_ERR_ARG;
        }
        // memory mode, every offset is in reach
        bytes->position = offset;
        return BARTAP_OK;
    }
    if (offset > stream->entry.uncompressed_size) {
        // past the entry's end, per the directory (R22)
        return BARTAP_ERR_ARG;
    }
    if (offset < stream->delivered) {
        // behind the stream. len 0 only asks whether the offset is still held
        bool held = bytes->rolling != NULL &&
                    bt_ring_read(bytes->rolling, offset, NULL, 0) == BARTAP_OK;
        if (!held) {
            held = bt_stream_reread(stream, offset, NULL, 0) == BARTAP_OK;
        }
        if (!held) {
            // gone from both, start the entry over. the next read skips up to offset
            bartap_err_t err = bt_stream_rewind(stream);
            if (err != BARTAP_OK) {
                // stream never opened, pass its failure up
                return err;
            }
            bytes_reset_ring(bytes);
        }
    }
    bytes->position = offset;
    return BARTAP_OK;
}

/* ---- XML tokenizer (R15, R21, R30, R33) ---- */

// Tokens never depend on the read-ahead's size (R33): the scanner only ever looks one byte ahead, and
// whatever it has consumed but not yet delivered (a partial UTF-8 sequence, an entity that turned out
// to be literal, CDATA's closing brackets) is held in the tokenizer, not the read-ahead. So a mismatch
// never needs bytes back, and a 1-byte read-ahead gives the same tokens as any other.

enum { XML_START, XML_CONTENT, XML_TEXT, XML_TAG, XML_END, XML_FAILED };

#define XML_PEEK_END (-1)
#define XML_PEEK_FAIL (-2) // the failure is in status

static void xml_warn(bartap_xml_t *xml, bartap_warning_t code, uint32_t offset)
{
    // tested and fuzzed alone, the tokenizer may have nowhere to report
    if (xml->warnings != NULL) {
        bt_warn(xml->warnings, code, xml->spine_index, offset);
    }
}

// The next byte without consuming it, refilling the read-ahead when empty.
static int peek(bartap_xml_t *xml)
{
    if (xml->readahead_pos == xml->readahead_len) {
        uint32_t got;
        bartap_err_t err = bt_bytes_read(xml->bytes, xml->readahead, BARTAP_XML_READAHEAD, &got);
        if (err == BARTAP_END) {
            return XML_PEEK_END;
        }
        if (err != BARTAP_OK) {
            // failed read, sticky from here on
            xml->status = err;
            xml->state = XML_FAILED;
            return XML_PEEK_FAIL;
        }
        xml->readahead_pos = 0;
        xml->readahead_len = got;
    }
    return xml->readahead[xml->readahead_pos];
}

// Consumes the byte peek just returned.
static void advance(bartap_xml_t *xml)
{
    xml->readahead_pos++;
    xml->offset++;
}

static bool is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_name_start(int c)
{
    return is_alpha(c) || c == '_' || c == ':' || c >= 0x80;
}

// Where a tag or attribute name stops. Lenient: anything else, quotes included, is part of it.
static bool ends_name(int c)
{
    return c < 0 || is_space(c) || c == '/' || c == '>' || c == '<' || c == '=';
}

static bool is_entity_char(int c)
{
    return is_alpha(c) || (c >= '0' && c <= '9') || c == '#';
}

static char fold(int c)
{
    return (char)bt_fold((uint8_t)c);
}

static bartap_err_t skip_space(bartap_xml_t *xml)
{
    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (!is_space(c)) {
            // includes the source's end, which the caller's next peek sees
            return BARTAP_OK;
        }
        advance(xml);
    }
}

// Clears everything that belonged to the last token.
static void token_reset(bartap_xml_t *xml)
{
    xml->text_done = 0;
    xml->cdata = 0;
    xml->brackets = 0;
    xml->held_len = 0;
    xml->replay_len = 0;
    xml->replay_pos = 0;
    xml->attr_count = 0;
    xml->attr_used = 0;
    xml->token.self_closing = 0;
    xml->token.name_len = 0;
}

// Back to a clean state at a source offset, which the byte source is already at.
static void xml_reset(bartap_xml_t *xml, uint32_t offset)
{
    xml->offset = offset;
    xml->readahead_pos = 0;
    xml->readahead_len = 0;
    xml->status = BARTAP_OK;
    // only the source's very start can hold a BOM
    xml->state = offset == 0 ? XML_START : XML_CONTENT;
    xml->staged_len = 0;
    token_reset(xml);
}

void bt_xml_init(bartap_xml_t *xml, bartap_bytes_t *bytes, bartap_warnings_t *warnings,
                 uint16_t spine_index)
{
    xml->bytes = bytes;
    xml->warnings = warnings;
    xml->spine_index = spine_index;
    xml_reset(xml, 0);
}

/* ---- Characters (R21, R30) ---- */

static uint32_t put_utf8(uint32_t cp, char *out)
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

// Windows-1252's 0x80-0x9F. Its five undefined bytes become U+FFFD, so the fallback never makes a C1
// control (R21).
static const uint16_t cp1252_high[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
};

// One byte read as Windows-1252, which also covers Latin-1 (R30).
static uint32_t decode_1252(uint8_t b, char *out)
{
    if (b >= 0x80u && b <= 0x9Fu) {
        return put_utf8(cp1252_high[b - 0x80u], out);
    }
    // ASCII and 0xA0-0xFF are their own code points
    return put_utf8(b, out);
}

// Checks against Unicode's well-formed byte sequences, so overlongs, surrogates and anything past
// U+10FFFF all fall back, one lead byte at a time.
uint32_t bt_utf8_decode(const uint8_t *p, uint32_t len, char *out, uint32_t *out_len)
{
    if (len == 0) {
        return 0;
    }
    uint8_t b = p[0];
    uint32_t need;
    uint8_t lo = 0x80u; // second byte's range, narrower after some leads
    uint8_t hi = 0xBFu;

    if (b < 0x80u) {
        // ASCII, the common case
        out[0] = (char)b;
        *out_len = 1;
        return 1;
    }
    if (b >= 0xC2u && b <= 0xDFu) {
        need = 1;
    } else if (b >= 0xE0u && b <= 0xEFu) {
        need = 2;
        lo = b == 0xE0u ? 0xA0u : 0x80u; // no overlongs
        hi = b == 0xEDu ? 0x9Fu : 0xBFu; // no surrogates
    } else if (b >= 0xF0u && b <= 0xF4u) {
        need = 3;
        lo = b == 0xF0u ? 0x90u : 0x80u; // no overlongs
        hi = b == 0xF4u ? 0x8Fu : 0xBFu; // nothing past U+10FFFF
    } else {
        // a continuation byte, an overlong lead or past U+10FFFF, so not UTF-8 at all
        *out_len = decode_1252(b, out);
        return 1;
    }
    for (uint32_t i = 1; i <= need; i++) {
        if (i >= len) {
            // valid so far, the caller has more to give
            return 0;
        }
        uint8_t c = p[i];
        if (c < (i == 1 ? lo : 0x80u) || c > (i == 1 ? hi : 0xBFu)) {
            // broken sequence, only its lead byte falls back. the rest are tried afresh
            *out_len = decode_1252(b, out);
            return 1;
        }
    }
    memcpy(out, p, need + 1u);
    *out_len = need + 1u;
    return need + 1u;
}

// Named entities: XML's five, then every entity of the XHTML 1 DTDs (the HTML 4 set: Latin-1,
// symbols and Greek, and the special characters), which EPUB 2 content may use (R30). In code point
// order after the first five. Changing this table changes text output (R12).
typedef struct {
    char name[9]; // the longest, "thetasym", and its terminator
    uint16_t cp;
} xml_entity_t;

static const xml_entity_t entities[] = {
    {"amp", 0x26}, {"lt", 0x3C}, {"gt", 0x3E}, {"quot", 0x22}, {"apos", 0x27}, {"nbsp", 0xA0},
    {"iexcl", 0xA1}, {"cent", 0xA2}, {"pound", 0xA3}, {"curren", 0xA4}, {"yen", 0xA5},
    {"brvbar", 0xA6}, {"sect", 0xA7}, {"uml", 0xA8}, {"copy", 0xA9}, {"ordf", 0xAA},
    {"laquo", 0xAB}, {"not", 0xAC}, {"shy", 0xAD}, {"reg", 0xAE}, {"macr", 0xAF}, {"deg", 0xB0},
    {"plusmn", 0xB1}, {"sup2", 0xB2}, {"sup3", 0xB3}, {"acute", 0xB4}, {"micro", 0xB5},
    {"para", 0xB6}, {"middot", 0xB7}, {"cedil", 0xB8}, {"sup1", 0xB9}, {"ordm", 0xBA},
    {"raquo", 0xBB}, {"frac14", 0xBC}, {"frac12", 0xBD}, {"frac34", 0xBE}, {"iquest", 0xBF},
    {"Agrave", 0xC0}, {"Aacute", 0xC1}, {"Acirc", 0xC2}, {"Atilde", 0xC3}, {"Auml", 0xC4},
    {"Aring", 0xC5}, {"AElig", 0xC6}, {"Ccedil", 0xC7}, {"Egrave", 0xC8}, {"Eacute", 0xC9},
    {"Ecirc", 0xCA}, {"Euml", 0xCB}, {"Igrave", 0xCC}, {"Iacute", 0xCD}, {"Icirc", 0xCE},
    {"Iuml", 0xCF}, {"ETH", 0xD0}, {"Ntilde", 0xD1}, {"Ograve", 0xD2}, {"Oacute", 0xD3},
    {"Ocirc", 0xD4}, {"Otilde", 0xD5}, {"Ouml", 0xD6}, {"times", 0xD7}, {"Oslash", 0xD8},
    {"Ugrave", 0xD9}, {"Uacute", 0xDA}, {"Ucirc", 0xDB}, {"Uuml", 0xDC}, {"Yacute", 0xDD},
    {"THORN", 0xDE}, {"szlig", 0xDF}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"acirc", 0xE2},
    {"atilde", 0xE3}, {"auml", 0xE4}, {"aring", 0xE5}, {"aelig", 0xE6}, {"ccedil", 0xE7},
    {"egrave", 0xE8}, {"eacute", 0xE9}, {"ecirc", 0xEA}, {"euml", 0xEB}, {"igrave", 0xEC},
    {"iacute", 0xED}, {"icirc", 0xEE}, {"iuml", 0xEF}, {"eth", 0xF0}, {"ntilde", 0xF1},
    {"ograve", 0xF2}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"otilde", 0xF5}, {"ouml", 0xF6},
    {"divide", 0xF7}, {"oslash", 0xF8}, {"ugrave", 0xF9}, {"uacute", 0xFA}, {"ucirc", 0xFB},
    {"uuml", 0xFC}, {"yacute", 0xFD}, {"thorn", 0xFE}, {"yuml", 0xFF}, {"OElig", 0x152},
    {"oelig", 0x153}, {"Scaron", 0x160}, {"scaron", 0x161}, {"Yuml", 0x178}, {"fnof", 0x192},
    {"circ", 0x2C6}, {"tilde", 0x2DC}, {"Alpha", 0x391}, {"Beta", 0x392}, {"Gamma", 0x393},
    {"Delta", 0x394}, {"Epsilon", 0x395}, {"Zeta", 0x396}, {"Eta", 0x397}, {"Theta", 0x398},
    {"Iota", 0x399}, {"Kappa", 0x39A}, {"Lambda", 0x39B}, {"Mu", 0x39C}, {"Nu", 0x39D},
    {"Xi", 0x39E}, {"Omicron", 0x39F}, {"Pi", 0x3A0}, {"Rho", 0x3A1}, {"Sigma", 0x3A3},
    {"Tau", 0x3A4}, {"Upsilon", 0x3A5}, {"Phi", 0x3A6}, {"Chi", 0x3A7}, {"Psi", 0x3A8},
    {"Omega", 0x3A9}, {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4},
    {"epsilon", 0x3B5}, {"zeta", 0x3B6}, {"eta", 0x3B7}, {"theta", 0x3B8}, {"iota", 0x3B9},
    {"kappa", 0x3BA}, {"lambda", 0x3BB}, {"mu", 0x3BC}, {"nu", 0x3BD}, {"xi", 0x3BE},
    {"omicron", 0x3BF}, {"pi", 0x3C0}, {"rho", 0x3C1}, {"sigmaf", 0x3C2}, {"sigma", 0x3C3},
    {"tau", 0x3C4}, {"upsilon", 0x3C5}, {"phi", 0x3C6}, {"chi", 0x3C7}, {"psi", 0x3C8},
    {"omega", 0x3C9}, {"thetasym", 0x3D1}, {"upsih", 0x3D2}, {"piv", 0x3D6}, {"ensp", 0x2002},
    {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"lrm", 0x200E},
    {"rlm", 0x200F}, {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
    {"sbquo", 0x201A}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bdquo", 0x201E}, {"dagger", 0x2020},
    {"Dagger", 0x2021}, {"bull", 0x2022}, {"hellip", 0x2026}, {"permil", 0x2030}, {"prime", 0x2032},
    {"Prime", 0x2033}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A}, {"oline", 0x203E}, {"frasl", 0x2044},
    {"euro", 0x20AC}, {"image", 0x2111}, {"weierp", 0x2118}, {"real", 0x211C}, {"trade", 0x2122},
    {"alefsym", 0x2135}, {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
    {"harr", 0x2194}, {"crarr", 0x21B5}, {"lArr", 0x21D0}, {"uArr", 0x21D1}, {"rArr", 0x21D2},
    {"dArr", 0x21D3}, {"hArr", 0x21D4}, {"forall", 0x2200}, {"part", 0x2202}, {"exist", 0x2203},
    {"empty", 0x2205}, {"nabla", 0x2207}, {"isin", 0x2208}, {"notin", 0x2209}, {"ni", 0x220B},
    {"prod", 0x220F}, {"sum", 0x2211}, {"minus", 0x2212}, {"lowast", 0x2217}, {"radic", 0x221A},
    {"prop", 0x221D}, {"infin", 0x221E}, {"ang", 0x2220}, {"and", 0x2227}, {"or", 0x2228},
    {"cap", 0x2229}, {"cup", 0x222A}, {"int", 0x222B}, {"there4", 0x2234}, {"sim", 0x223C},
    {"cong", 0x2245}, {"asymp", 0x2248}, {"ne", 0x2260}, {"equiv", 0x2261}, {"le", 0x2264},
    {"ge", 0x2265}, {"sub", 0x2282}, {"sup", 0x2283}, {"nsub", 0x2284}, {"sube", 0x2286},
    {"supe", 0x2287}, {"oplus", 0x2295}, {"otimes", 0x2297}, {"perp", 0x22A5}, {"sdot", 0x22C5},
    {"lceil", 0x2308}, {"rceil", 0x2309}, {"lfloor", 0x230A}, {"rfloor", 0x230B}, {"lang", 0x2329},
    {"rang", 0x232A}, {"loz", 0x25CA}, {"spades", 0x2660}, {"clubs", 0x2663}, {"hearts", 0x2665},
    {"diams", 0x2666},
};

uint32_t bt_entity_decode(const char *name, uint32_t name_len, char *out)
{
    if (name_len >= 2u && name[0] == '#') {
        // character reference, decimal or hex
        bool hex = name[1] == 'x' || name[1] == 'X';
        uint32_t i = hex ? 2u : 1u;
        uint32_t cp = 0;
        if (i == name_len) {
            // "#x" with no digits, not a reference
            return 0;
        }
        for (; i < name_len; i++) {
            char c = name[i];
            uint32_t digit;
            if (c >= '0' && c <= '9') {
                digit = (uint32_t)(c - '0');
            } else if (hex && fold(c) >= 'a' && fold(c) <= 'f') {
                digit = (uint32_t)(fold(c) - 'a') + 10u;
            } else {
                // not a digit, not a reference
                return 0;
            }
            // stop growing once out of range, so long digit runs can't overflow
            if (cp <= 0x10FFFFu) {
                cp = cp * (hex ? 16u : 10u) + digit;
            }
        }
        // no character there, the replacement stands in
        if (cp == 0 || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
            cp = 0xFFFDu;
        }
        // 0x80-0x9F are Windows-1252's characters, as HTML reads them and as the fallback reads
        // the raw bytes (R30): &#150; is a dash. The five it leaves undefined stay C1 controls,
        // which R21's filter drops
        if (cp >= 0x80u && cp <= 0x9Fu && cp1252_high[cp - 0x80u] != 0xFFFDu) {
            cp = cp1252_high[cp - 0x80u];
        }
        return put_utf8(cp, out);
    }
    // named, case-sensitive as XML is
    for (size_t i = 0; i < sizeof entities / sizeof entities[0]; i++) {
        const xml_entity_t *e = &entities[i];
        if (name_len < sizeof e->name && memcmp(e->name, name, name_len) == 0 &&
            e->name[name_len] == '\0') {
            return put_utf8(e->cp, out);
        }
    }
    return 0;
}

// What R21 drops: C0 controls other than tab, LF and CR, then DEL, then C1 controls.
static bool is_control(const char *ch, uint32_t len)
{
    uint8_t b = (uint8_t)ch[0];
    if (len == 1) {
        return (b < 0x20u && b != '\t' && b != '\n' && b != '\r') || b == 0x7Fu;
    }
    return len == 2 && b == 0xC2u && (uint8_t)ch[1] <= 0x9Fu;
}

// Reads an entity after its '&', which was at amp. A known one goes to held as one character;
// anything else goes to replay, to pass through literally (R30).
static bartap_err_t read_entity(bartap_xml_t *xml, uint32_t amp)
{
    xml->held_len = 0;
    xml->replay[0] = '&';
    xml->replay_len = 1;
    xml->replay_pos = 0;
    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == ';') {
            advance(xml);
            uint32_t n = bt_entity_decode(xml->replay + 1, xml->replay_len - 1u, xml->held);
            if (n != 0) {
                // known, the literal bytes aren't needed
                xml->replay_len = 0;
                xml->held_len = (uint8_t)n;
                return BARTAP_OK;
            }
            // well formed but unknown, passed through ';' and all
            xml->replay[xml->replay_len++] = ';';
            xml_warn(xml, BARTAP_WARN_UNKNOWN_ENTITY, amp);
            return BARTAP_OK;
        }
        if (is_entity_char(c) && xml->replay_len <= BARTAP_XML_ENTITY_MAX) {
            advance(xml);
            xml->replay[xml->replay_len++] = (char)c;
            continue;
        }
        // no ';' in reach, such as a bare '&'. what was read passes through, the rest is untouched
        xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, amp);
        return BARTAP_OK;
    }
}

// Decodes the first staged character into held, consuming continuation bytes as it needs them. A
// sequence cut short falls back to Windows-1252 for its lead byte, like any other invalid UTF-8.
static bartap_err_t decode_staged(bartap_xml_t *xml)
{
    for (;;) {
        uint32_t len;
        uint32_t used = bt_utf8_decode(xml->staged, xml->staged_len, xml->held, &len);
        if (used == 0) {
            int c = peek(xml);
            if (c == XML_PEEK_FAIL) {
                return xml->status;
            }
            if (c >= 0x80 && c <= 0xBF && xml->staged_len < sizeof xml->staged) {
                // a continuation byte, take it and try again
                advance(xml);
                xml->staged[xml->staged_len++] = (uint8_t)c;
                continue;
            }
            // cut short by anything else, the source's end included
            len = decode_1252(xml->staged[0], xml->held);
            used = 1;
        }
        if (used == 1 && xml->staged[0] >= 0x80u) {
            // the staged bytes are the latest consumed, so the lead sits staged_len back
            xml_warn(xml, BARTAP_WARN_INVALID_UTF8, xml->offset - xml->staged_len);
        }
        xml->held_len = (uint8_t)len;
        xml->staged_len = (uint8_t)(xml->staged_len - used);
        memmove(xml->staged, xml->staged + used, xml->staged_len);
        return BARTAP_OK;
    }
}

// Loads the next character of the current text token into held: BARTAP_OK, BARTAP_END at the
// token's end, or a failure. Control characters are dropped here, with a warning (R21).
static bartap_err_t next_char(bartap_xml_t *xml)
{
    for (;;) {
        uint32_t at = xml->offset;
        bartap_err_t err;

        if (xml->replay_pos < xml->replay_len) {
            // literal bytes owed, from an entity or CDATA's brackets. all printable ASCII
            xml->held[0] = xml->replay[xml->replay_pos++];
            xml->held_len = 1;
            return BARTAP_OK;
        }
        xml->replay_pos = 0;
        xml->replay_len = 0;

        if (xml->staged_len == 0) {
            int c = peek(xml);
            if (c == XML_PEEK_FAIL) {
                return xml->status;
            }
            if (xml->cdata) {
                // raw text: no entities, '<' is literal, only "]]>" ends it
                if (c == XML_PEEK_END) {
                    if (xml->brackets != 0) {
                        // owe the held brackets first, the end comes round again
                        memset(xml->replay, ']', xml->brackets);
                        xml->replay_len = xml->brackets;
                        xml->brackets = 0;
                        continue;
                    }
                    // source ended inside the section, warn once
                    xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
                    xml->cdata = 0;
                    return BARTAP_END;
                }
                if (c == ']') {
                    advance(xml);
                    if (xml->brackets < 2u) {
                        // might be the end, hold it
                        xml->brackets++;
                        continue;
                    }
                    // a third: the oldest held one was text after all
                    xml->held[0] = ']';
                    xml->held_len = 1;
                    return BARTAP_OK;
                }
                if (c == '>' && xml->brackets == 2u) {
                    // "]]>", the section and the token end
                    advance(xml);
                    xml->brackets = 0;
                    xml->cdata = 0;
                    return BARTAP_END;
                }
                if (xml->brackets != 0) {
                    // held brackets were text, owe them. c is still unread
                    memset(xml->replay, ']', xml->brackets);
                    xml->replay_len = xml->brackets;
                    xml->brackets = 0;
                    continue;
                }
            } else {
                if (c == XML_PEEK_END || c == '<') {
                    // markup or the source's end, the token stops before it
                    return BARTAP_END;
                }
                if (c == '&') {
                    advance(xml);
                    err = read_entity(xml, at);
                    if (err != BARTAP_OK) {
                        return err;
                    }
                    if (xml->held_len == 0) {
                        // literal, it comes out of replay next time round
                        continue;
                    }
                    // a decoded character, still subject to the control filter
                    goto filter;
                }
            }
            advance(xml);
            xml->staged[0] = (uint8_t)c;
            xml->staged_len = 1;
        }
        err = decode_staged(xml);
        if (err != BARTAP_OK) {
            return err;
        }
    filter:
        if (!is_control(xml->held, xml->held_len)) {
            return BARTAP_OK;
        }
        // dropped from text (R21), carry on to the next character
        xml_warn(xml, BARTAP_WARN_CONTROL_CHAR, at);
        xml->held_len = 0;
    }
}

// Runs the current text token to its end, delivered or not. Warnings fire either way, so they don't
// depend on whether the caller read the text.
static bartap_err_t skip_text(bartap_xml_t *xml)
{
    while (!xml->text_done) {
        xml->held_len = 0;
        bartap_err_t err = next_char(xml);
        if (err == BARTAP_END) {
            xml->text_done = 1;
            xml->token.end_offset = xml->offset;
        } else if (err != BARTAP_OK) {
            return err;
        }
    }
    xml->held_len = 0;
    return BARTAP_OK;
}

bartap_err_t bt_xml_text(bartap_xml_t *xml, char *buf, uint32_t cap, uint32_t *got)
{
    *got = 0;
    if (xml->state == XML_FAILED) {
        return xml->status;
    }
    if (xml->state != XML_TEXT) {
        // the current token isn't text
        return BARTAP_ERR_ARG;
    }
    while (!xml->text_done) {
        if (xml->held_len == 0) {
            bartap_err_t err = next_char(xml);
            if (err == BARTAP_END) {
                // only now is the token's end known
                xml->text_done = 1;
                xml->token.end_offset = xml->offset;
                break;
            }
            if (err != BARTAP_OK) {
                return err;
            }
        }
        if (xml->held_len > cap - *got) {
            // whole characters only, this one waits for the next call
            break;
        }
        memcpy(buf + *got, xml->held, xml->held_len);
        *got += xml->held_len;
        xml->held_len = 0;
    }
    if (*got == 0 && xml->held_len != 0) {
        // not even one character fits (R50)
        return BARTAP_ERR_BUFFER_TOO_SMALL;
    }
    return BARTAP_OK;
}

/* ---- Tags ---- */

// Reads a tag name, keeping the ASCII-folded local name (R46). A local name longer than
// BARTAP_XML_NAME_MAX can't match a known element, so it's kept as empty.
static bartap_err_t read_name(bartap_xml_t *xml)
{
    bartap_xml_token_t *tok = &xml->token;
    uint32_t len = 0;

    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (ends_name(c)) {
            break;
        }
        advance(xml);
        if (c == ':') {
            // what came before was a prefix, the local name starts over
            len = 0;
            continue;
        }
        if (len < BARTAP_XML_NAME_MAX) {
            tok->name[len] = fold(c);
        }
        len++;
    }
    tok->name_len = len <= BARTAP_XML_NAME_MAX ? (uint8_t)len : 0;
    return BARTAP_OK;
}

// Appends to attribute storage, or marks the attribute dropped once it doesn't fit (R33).
static void attr_put(bartap_xml_t *xml, const char *p, uint32_t len, bool *dropped)
{
    if (*dropped) {
        return;
    }
    if (len > BARTAP_ATTR_MAX - xml->attr_used) {
        *dropped = true;
        return;
    }
    memcpy(xml->attr_storage + xml->attr_used, p, len);
    xml->attr_used += len;
}

// An entity inside an attribute value, decoded the same way as in text.
static bartap_err_t attr_entity(bartap_xml_t *xml, uint32_t amp, bool *dropped)
{
    bartap_err_t err = read_entity(xml, amp);
    if (err != BARTAP_OK) {
        return err;
    }
    if (xml->held_len != 0) {
        attr_put(xml, xml->held, xml->held_len, dropped);
    } else {
        // literal, stored as read
        attr_put(xml, xml->replay, xml->replay_len, dropped);
    }
    xml->held_len = 0;
    xml->replay_len = 0;
    return BARTAP_OK;
}

// Reads an attribute value after its '='. BARTAP_END if the source ends inside quotes.
static bartap_err_t read_value(bartap_xml_t *xml, uint32_t at, bool *dropped)
{
    int c = peek(xml);
    if (c == XML_PEEK_FAIL) {
        return xml->status;
    }
    if (c == '"' || c == '\'') {
        int quote = c;
        advance(xml);
        for (;;) {
            uint32_t pos = xml->offset;
            c = peek(xml);
            if (c == XML_PEEK_FAIL) {
                return xml->status;
            }
            if (c == XML_PEEK_END) {
                // source ended inside the quotes, the tag is lost
                xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
                return BARTAP_END;
            }
            advance(xml);
            if (c == quote) {
                return BARTAP_OK;
            }
            if (c == '&') {
                bartap_err_t err = attr_entity(xml, pos, dropped);
                if (err != BARTAP_OK) {
                    return err;
                }
                continue;
            }
            char ch = (char)c;
            attr_put(xml, &ch, 1, dropped);
        }
    }
    // unquoted, as HTML allows. runs to whitespace or the tag's end
    xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
    for (;;) {
        uint32_t pos = xml->offset;
        c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END || is_space(c) || c == '>' || c == '<') {
            return BARTAP_OK;
        }
        advance(xml);
        if (c == '&') {
            bartap_err_t err = attr_entity(xml, pos, dropped);
            if (err != BARTAP_OK) {
                return err;
            }
            continue;
        }
        char ch = (char)c;
        attr_put(xml, &ch, 1, dropped);
    }
}

// Reads one attribute into storage. One that doesn't fit, or past BARTAP_XML_ATTRS_MAX, is still
// read past but dropped whole, with a warning: a truncated href or id would be wrong, not short (R33).
static bartap_err_t read_attribute(bartap_xml_t *xml)
{
    uint32_t at = xml->offset;
    uint32_t name_start = xml->attr_used;
    bool dropped = xml->attr_count >= BARTAP_XML_ATTRS_MAX;
    bartap_err_t err;

    // the caller saw a byte that isn't space or the tag's end. take it whatever it is, so a stray
    // '=' or quote can't stall the loop
    for (bool first = true;; first = false) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (!first && ends_name(c)) {
            break;
        }
        advance(xml);
        char ch = (char)c;
        attr_put(xml, &ch, 1, &dropped);
    }
    uint32_t name_len = xml->attr_used - name_start;

    err = skip_space(xml);
    if (err != BARTAP_OK) {
        return err;
    }
    uint32_t value_start = xml->attr_used;
    int c = peek(xml);
    if (c == XML_PEEK_FAIL) {
        return xml->status;
    }
    if (c == '=') {
        advance(xml);
        err = skip_space(xml);
        if (err == BARTAP_OK) {
            err = read_value(xml, at, &dropped);
        }
        if (err != BARTAP_OK) {
            return err;
        }
    } else {
        // no value, as in HTML's <input disabled>. kept, empty, with a warning
        xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
    }

    if (dropped) {
        // give back whatever part of it was stored
        xml_warn(xml, BARTAP_WARN_ATTR_TRUNCATED, at);
        xml->attr_used = name_start;
        return BARTAP_OK;
    }
    bartap_xml_attr_t *a = &xml->attrs[xml->attr_count++];
    a->name_offset = (uint16_t)name_start;
    a->name_len = (uint16_t)name_len;
    a->value_offset = (uint16_t)value_start;
    a->value_len = (uint16_t)(xml->attr_used - value_start);
    return BARTAP_OK;
}

// Reads a start tag after its '<'. BARTAP_END if the source ends inside it: the tag is dropped.
static bartap_err_t read_start_tag(bartap_xml_t *xml)
{
    bartap_xml_token_t *tok = &xml->token;
    bartap_err_t err = read_name(xml);

    tok->kind = BARTAP_XML_START_TAG;
    while (err == BARTAP_OK) {
        err = skip_space(xml);
        if (err != BARTAP_OK) {
            break;
        }
        uint32_t at = xml->offset;
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END) {
            // source ended inside the tag
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, tok->offset);
            return BARTAP_END;
        }
        if (c == '>') {
            advance(xml);
            break;
        }
        if (c == '<') {
            // a tag left open. end it here, so the next one isn't swallowed
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
            break;
        }
        if (c == '/') {
            advance(xml);
            c = peek(xml);
            if (c == XML_PEEK_FAIL) {
                return xml->status;
            }
            if (c == '>') {
                advance(xml);
                tok->self_closing = 1;
                break;
            }
            // a stray '/', skipped
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
            continue;
        }
        err = read_attribute(xml);
    }
    tok->end_offset = xml->offset;
    return err;
}

// Reads an end tag after its "</". Anything between the name and '>' is ignored, with a warning.
static bartap_err_t read_end_tag(bartap_xml_t *xml)
{
    bartap_xml_token_t *tok = &xml->token;
    bool junk = false;

    tok->kind = BARTAP_XML_END_TAG;
    bartap_err_t err = read_name(xml);
    if (err != BARTAP_OK) {
        return err;
    }
    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END) {
            // source ended inside the tag
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, tok->offset);
            return BARTAP_END;
        }
        if (c == '>') {
            advance(xml);
            break;
        }
        if (c == '<') {
            // left open, end it here as with a start tag
            junk = true;
            break;
        }
        advance(xml);
        junk = junk || !is_space(c);
    }
    if (junk) {
        xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, tok->offset);
    }
    tok->end_offset = xml->offset;
    return BARTAP_OK;
}

/* ---- Skipped markup ---- */

// Skips a comment after its "<!--", to "-->". BARTAP_END if the source ends first.
static bartap_err_t skip_comment(bartap_xml_t *xml, uint32_t at)
{
    uint32_t dashes = 0;
    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END) {
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
            return BARTAP_END;
        }
        advance(xml);
        if (c == '>' && dashes >= 2u) {
            return BARTAP_OK;
        }
        dashes = c == '-' ? dashes + 1u : 0;
    }
}

// Skips a processing instruction, such as <?xml ...?>, after its "<?", to "?>".
static bartap_err_t skip_pi(bartap_xml_t *xml, uint32_t at)
{
    bool question = false;
    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END) {
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
            return BARTAP_END;
        }
        advance(xml);
        if (c == '>' && question) {
            return BARTAP_OK;
        }
        question = c == '?';
    }
}

// Skips a declaration such as a DOCTYPE to its '>', past an internal subset in brackets, quoted
// strings and the comments inside it: a quote or bracket in those isn't the declaration's own.
static bartap_err_t skip_decl(bartap_xml_t *xml, uint32_t at)
{
    static const char comment_open[] = "<!--";
    int quote = 0;
    uint32_t depth = 0;
    uint32_t opening = 0; // how much of "<!--" the latest bytes spell
    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END) {
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
            return BARTAP_END;
        }
        advance(xml);
        if (quote != 0) {
            // inside a quoted string, only its closing quote matters
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        opening = c == comment_open[opening] ? opening + 1u : (c == '<' ? 1u : 0u);
        if (opening == sizeof comment_open - 1u) {
            // a comment, whatever it holds. already warned if the source ends inside it
            bartap_err_t err = skip_comment(xml, at);
            if (err != BARTAP_OK) {
                return err;
            }
            opening = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[') {
            depth++;
        } else if (c == ']' && depth != 0) {
            depth--;
        } else if (c == '>' && depth == 0) {
            return BARTAP_OK;
        }
    }
}

// Skips markup that is nothing recognised, to the first '>', as HTML skips a bogus comment. Quotes
// and brackets mean nothing here: "</ don't>" ends at its '>'.
static bartap_err_t skip_bogus(bartap_xml_t *xml, uint32_t at)
{
    for (;;) {
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END) {
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
            return BARTAP_END;
        }
        advance(xml);
        if (c == '>') {
            return BARTAP_OK;
        }
    }
}

// After "<!": a comment, a CDATA section (*cdata set, its text follows) or a declaration to skip.
// Matched a byte at a time, so a mismatch never needs bytes back.
static bartap_err_t read_bang(bartap_xml_t *xml, uint32_t at, bool *cdata)
{
    static const char cdata_open[] = "CDATA[";

    *cdata = false;
    int c = peek(xml);
    if (c == XML_PEEK_FAIL) {
        return xml->status;
    }
    if (c == '-') {
        advance(xml);
        c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == '-') {
            advance(xml);
            return skip_comment(xml, at);
        }
        // "<!-" without its second dash, nothing recognised
        xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
        return skip_bogus(xml, at);
    }
    if (c == '[') {
        advance(xml);
        for (uint32_t i = 0; i < sizeof cdata_open - 1u; i++) {
            c = peek(xml);
            if (c == XML_PEEK_FAIL) {
                return xml->status;
            }
            if (c != cdata_open[i]) {
                // "<![" but not CDATA, nothing recognised
                xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
                return skip_bogus(xml, at);
            }
            advance(xml);
        }
        *cdata = true;
        return BARTAP_OK;
    }
    // a DOCTYPE or other declaration, nothing in it is kept
    return skip_decl(xml, at);
}

// At the source's start: skips a UTF-8 BOM and refuses UTF-16 (R30). Bytes that turn out not to be a
// BOM are staged, to be decoded as the first text.
static bartap_err_t read_bom(bartap_xml_t *xml)
{
    static const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};

    xml->state = XML_CONTENT;
    int c = peek(xml);
    if (c == XML_PEEK_FAIL) {
        return xml->status;
    }
    if (c == 0xFE || c == 0xFF) {
        advance(xml);
        int d = peek(xml);
        if (d == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (d == (c == 0xFE ? 0xFF : 0xFE)) {
            // UTF-16, either byte order. this item can't be read
            xml->status = BARTAP_ERR_UNSUPPORTED;
            xml->state = XML_FAILED;
            return xml->status;
        }
        // just a byte, it's text
        xml->staged[0] = (uint8_t)c;
        xml->staged_len = 1;
        return BARTAP_OK;
    }
    for (uint32_t i = 0; i < sizeof bom && c == bom[i]; i++) {
        advance(xml);
        xml->staged[xml->staged_len++] = (uint8_t)c;
        c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
    }
    if (xml->staged_len == sizeof bom) {
        // a whole BOM, dropped
        xml->staged_len = 0;
    }
    return BARTAP_OK;
}

// Makes the current token a text token starting at at. Its characters come from bt_xml_text.
static bartap_err_t begin_text(bartap_xml_t *xml, uint32_t at, const bartap_xml_token_t **token)
{
    xml->token.kind = BARTAP_XML_TEXT;
    xml->token.offset = at;
    xml->token.end_offset = at; // not known until the text has been read
    xml->state = XML_TEXT;
    *token = &xml->token;
    return BARTAP_OK;
}

bartap_err_t bt_xml_next(bartap_xml_t *xml, const bartap_xml_token_t **token)
{
    bartap_err_t err;

    *token = NULL;
    if (xml->state == XML_FAILED) {
        return xml->status;
    }
    if (xml->state == XML_END) {
        return BARTAP_END;
    }
    if (xml->state == XML_TEXT) {
        // the caller moved on mid-text, run it out
        err = skip_text(xml);
        if (err != BARTAP_OK) {
            return err;
        }
    }
    if (xml->state == XML_START) {
        err = read_bom(xml);
        if (err != BARTAP_OK) {
            return err;
        }
    }
    xml->state = XML_CONTENT;

    for (;;) {
        // staged bytes were consumed already, the token starts at the first of them
        uint32_t at = xml->offset - xml->staged_len;
        bool cdata;

        token_reset(xml);
        xml->token.offset = at;
        if (xml->staged_len != 0) {
            // left by read_bom, text
            return begin_text(xml, at, token);
        }
        int c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }
        if (c == XML_PEEK_END) {
            xml->state = XML_END;
            return BARTAP_END;
        }
        if (c != '<') {
            return begin_text(xml, at, token);
        }
        advance(xml);
        c = peek(xml);
        if (c == XML_PEEK_FAIL) {
            return xml->status;
        }

        if (c == '/') {
            advance(xml);
            c = peek(xml);
            if (c == XML_PEEK_FAIL) {
                return xml->status;
            }
            if (is_name_start(c)) {
                err = read_end_tag(xml);
            } else {
                // "</" without a name, skipped as HTML does
                xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
                err = skip_bogus(xml, at);
                if (err == BARTAP_OK) {
                    continue;
                }
            }
        } else if (c == '!') {
            advance(xml);
            err = read_bang(xml, at, &cdata);
            if (err == BARTAP_OK && cdata) {
                // CDATA is text, read raw
                xml->cdata = 1;
                return begin_text(xml, at, token);
            }
            if (err == BARTAP_OK) {
                continue;
            }
        } else if (c == '?') {
            advance(xml);
            err = skip_pi(xml, at);
            if (err == BARTAP_OK) {
                continue;
            }
        } else if (is_name_start(c)) {
            err = read_start_tag(xml);
        } else {
            // a '<' that starts no markup, such as "a < b". it's text
            xml_warn(xml, BARTAP_WARN_MALFORMED_MARKUP, at);
            xml->replay[0] = '<';
            xml->replay_len = 1;
            return begin_text(xml, at, token);
        }

        if (err == BARTAP_END) {
            // the source ended inside markup, already warned. nothing more to give
            xml->state = XML_END;
            return BARTAP_END;
        }
        if (err != BARTAP_OK) {
            return err;
        }
        xml->state = XML_TAG;
        *token = &xml->token;
        return BARTAP_OK;
    }
}

bool bt_xml_attr(const bartap_xml_t *xml, const char *name, uint32_t name_len, const char **value,
                 uint32_t *value_len)
{
    if (xml->state != XML_TAG) {
        // only a tag has attributes. an end tag's count is 0
        return false;
    }
    // first match wins. names compare ASCII-folded, as HTML's do
    for (uint32_t i = 0; i < xml->attr_count; i++) {
        const bartap_xml_attr_t *a = &xml->attrs[i];
        const char *stored = xml->attr_storage + a->name_offset;
        uint32_t j = 0;
        if (a->name_len != name_len) {
            continue;
        }
        while (j < name_len && fold(stored[j]) == fold(name[j])) {
            j++;
        }
        if (j == name_len) {
            *value = xml->attr_storage + a->value_offset;
            *value_len = a->value_len;
            return true;
        }
    }
    return false;
}

// Resets the read-ahead and resumes at a source offset (R48). The caller picks an offset between
// tokens, such as a checkpoint's resume offset.
bartap_err_t bt_xml_seek(bartap_xml_t *xml, uint32_t offset)
{
    bartap_err_t err = bt_bytes_seek(xml->bytes, offset);
    if (err != BARTAP_OK) {
        // the source didn't move, so neither does the tokenizer
        return err;
    }
    xml_reset(xml, offset);
    return BARTAP_OK;
}
