/* Canonical dump of a token stream (R15): readable, so unit tests can spell out what they expect, and
 * exact, so builds and modes can be diffed (R33). Test code, not library code.
 *
 *   S 3-10 p / a="v" b="w"   start tag: offsets, folded local name, self-closing mark, attributes
 *   E 20-24 p                end tag
 *   T 10-20 "text"           text token, read whole; bytes outside printable ASCII as \xNN
 *     W malformed@12         a warning raised while producing the token above it
 *   END / ERR <name>         how bt_xml_next finished
 *
 * A warning goes after the token it was raised for, never inside a text chunk, so the dump does not
 * depend on the text buffer's size. One about skipped markup before a token (offset below the
 * token's) goes before the token's line instead: a seek to the token skips that markup, and its
 * warning with it. */
#ifndef BARTAP_TEST_XML_DUMP_H
#define BARTAP_TEST_XML_DUMP_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    size_t *starts;   /* where each token's line begins, for comparing suffixes after a seek */
    uint32_t *offsets; /* each token's source offset */
    size_t count;
    size_t starts_cap;
    struct {
        bartap_warning_t code;
        uint32_t offset;
        uint16_t spine_index;
    } *pending; /* warnings raised since the last one written */
    size_t pending_len;
    size_t pending_cap;
} xml_dump_t;

static inline void dump_put(xml_dump_t *d, const char *p, size_t n)
{
    if (d->len + n + 1 > d->cap) {
        size_t cap = d->cap != 0 ? d->cap : 1024;
        while (d->len + n + 1 > cap) {
            cap *= 2;
        }
        char *grown = realloc(d->buf, cap);
        if (grown == NULL) {
            abort();
        }
        d->buf = grown;
        d->cap = cap;
    }
    memcpy(d->buf + d->len, p, n);
    d->len += n;
    d->buf[d->len] = '\0';
}

static inline void dump_printf(xml_dump_t *d, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dump_put(d, line, n < 0 ? 0 : (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

static inline void dump_escaped(xml_dump_t *d, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\') {
            char esc[2] = {'\\', (char)c};
            dump_put(d, esc, 2);
        } else if (c >= 0x20 && c < 0x7F) {
            dump_put(d, (const char *)&c, 1);
        } else {
            dump_printf(d, "\\x%02X", c);
        }
    }
}

static inline const char *dump_warning_name(bartap_warning_t code)
{
    switch (code) {
    case BARTAP_WARN_MALFORMED_MARKUP: return "malformed";
    case BARTAP_WARN_INVALID_UTF8: return "utf8";
    case BARTAP_WARN_CONTROL_CHAR: return "control";
    case BARTAP_WARN_UNKNOWN_ENTITY: return "entity";
    case BARTAP_WARN_ATTR_TRUNCATED: return "attr";
    default: return "other";
    }
}

static inline void dump_on_warn(void *user, bartap_warning_t code, uint16_t spine_index,
                                uint32_t offset)
{
    xml_dump_t *d = user;
    if (d->pending_len == d->pending_cap) {
        d->pending_cap = d->pending_cap != 0 ? d->pending_cap * 2 : 16;
        d->pending = realloc(d->pending, d->pending_cap * sizeof *d->pending);
        if (d->pending == NULL) {
            abort();
        }
    }
    d->pending[d->pending_len].code = code;
    d->pending[d->pending_len].offset = offset;
    d->pending[d->pending_len].spine_index = spine_index;
    d->pending_len++;
}

/* Writes the pending warnings below offset, or all of them for UINT32_MAX, keeping the rest. */
static inline void dump_flush_warnings(xml_dump_t *d, uint32_t below)
{
    size_t kept = 0;
    for (size_t i = 0; i < d->pending_len; i++) {
        if (below == UINT32_MAX || d->pending[i].offset < below) {
            dump_printf(d, "  W %s@%u spine=%u\n", dump_warning_name(d->pending[i].code),
                        d->pending[i].offset, d->pending[i].spine_index);
        } else {
            d->pending[kept++] = d->pending[i];
        }
    }
    d->pending_len = kept;
}

static inline void dump_token_start(xml_dump_t *d, uint32_t offset)
{
    if (d->count == d->starts_cap) {
        d->starts_cap = d->starts_cap != 0 ? d->starts_cap * 2 : 64;
        d->starts = realloc(d->starts, d->starts_cap * sizeof *d->starts);
        d->offsets = realloc(d->offsets, d->starts_cap * sizeof *d->offsets);
        if (d->starts == NULL || d->offsets == NULL) {
            abort();
        }
    }
    d->starts[d->count] = d->len;
    d->offsets[d->count] = offset;
    d->count++;
}

/* Dumps every token xml gives, reading text cap bytes at a time. The xml must have been initialised
 * with d's warnings, from dump_warnings. Returns how bt_xml_next finished. */
static inline bartap_err_t xml_dump_run(xml_dump_t *d, bartap_xml_t *xml, uint32_t cap)
{
    char *text = malloc(cap != 0 ? cap : 1);
    bartap_err_t err;

    if (text == NULL) {
        abort();
    }
    for (;;) {
        const bartap_xml_token_t *t;
        err = bt_xml_next(xml, &t);
        if (err != BARTAP_OK) {
            dump_flush_warnings(d, UINT32_MAX);
            dump_printf(d, err == BARTAP_END ? "END\n" : "ERR %s\n", bartap_strerror(err));
            break;
        }
        dump_flush_warnings(d, t->offset);
        dump_token_start(d, t->offset);
        if (t->kind == BARTAP_XML_TEXT) {
            xml_dump_t body = {0};
            uint32_t got;
            for (;;) {
                err = bt_xml_text(xml, text, cap, &got);
                if (err != BARTAP_OK || got == 0) {
                    break;
                }
                dump_escaped(&body, text, got);
            }
            dump_printf(d, "T %u-%u \"", t->offset, t->end_offset);
            dump_put(d, body.buf != NULL ? body.buf : "", body.len);
            dump_put(d, "\"", 1);
            if (err != BARTAP_OK) {
                dump_printf(d, " ERR %s", bartap_strerror(err));
            }
            dump_put(d, "\n", 1);
            free(body.buf);
        } else {
            dump_printf(d, "%s %u-%u %.*s%s", t->kind == BARTAP_XML_START_TAG ? "S" : "E", t->offset,
                        t->end_offset, (int)t->name_len, t->name, t->self_closing ? " /" : "");
            for (uint32_t i = 0; i < xml->attr_count; i++) {
                const bartap_xml_attr_t *a = &xml->attrs[i];
                dump_put(d, " ", 1);
                dump_escaped(d, xml->attr_storage + a->name_offset, a->name_len);
                dump_put(d, "=\"", 2);
                dump_escaped(d, xml->attr_storage + a->value_offset, a->value_len);
                dump_put(d, "\"", 1);
            }
            dump_put(d, "\n", 1);
        }
        dump_flush_warnings(d, UINT32_MAX);
    }
    free(text);
    return err;
}

static inline bartap_warnings_t dump_warnings(xml_dump_t *d)
{
    bartap_warnings_t w = {0, dump_on_warn, d};
    return w;
}

/* Memory mode: the usual way to dump a document. */
static inline void xml_dump_memory(xml_dump_t *d, const uint8_t *data, uint32_t size, uint32_t cap)
{
    static bartap_xml_t xml; /* about 1.4 KiB, kept off the stack */
    bartap_bytes_t bytes;
    bartap_warnings_t w = dump_warnings(d);

    bt_bytes_init_memory(&bytes, data, size, NULL);
    bt_xml_init(&xml, &bytes, &w, 7);
    xml_dump_run(d, &xml, cap);
}

static inline void xml_dump_free(xml_dump_t *d)
{
    free(d->pending);
    free(d->buf);
    free(d->starts);
    free(d->offsets);
    memset(d, 0, sizeof *d);
}

/* FNV-1a over the dump, for comparing builds by a line of output each. */
static inline uint64_t xml_dump_hash(const xml_dump_t *d)
{
    uint64_t h = 0xCBF29CE484222325u;
    for (size_t i = 0; i < d->len; i++) {
        h = (h ^ (unsigned char)d->buf[i]) * 0x100000001B3u;
    }
    return h;
}

#endif /* BARTAP_TEST_XML_DUMP_H */
