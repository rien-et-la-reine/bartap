/* Builds zip archives and EPUBs byte by byte for tests, sharing no code with what is tested: its own
 * CRC-32 and little-endian writes. Entries are stored, or deflated as stored blocks (enough to take a
 * reader down the deflate path), or carry any other method number untouched. Test code only. */
#ifndef BARTAP_TEST_EPUB_BUILD_H
#define BARTAP_TEST_EPUB_BUILD_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EPUB_MAX_ENTRIES 400u

enum { EPUB_STORED = 0, EPUB_DEFLATED = 8 };

typedef struct {
    uint32_t local_offset;
    uint32_t dir_offset; /* of its central-directory record, once finished */
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc;
    uint16_t method;
    uint16_t name_len;
    char name[1024];
} epub_entry_t;

typedef struct {
    uint8_t *bytes;
    uint32_t len;
    uint32_t cap;
    epub_entry_t entries[EPUB_MAX_ENTRIES];
    uint32_t count;
    uint32_t dir_offset;
    uint32_t dir_size;
} epub_t;

static inline uint32_t epub_crc32(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static inline void epub_reserve(epub_t *e, uint32_t more)
{
    if (e->len + more <= e->cap) {
        return;
    }
    uint32_t cap = e->cap != 0 ? e->cap : 4096u;
    while (e->len + more > cap) {
        cap *= 2u;
    }
    e->bytes = realloc(e->bytes, cap);
    if (e->bytes == NULL) {
        abort();
    }
    e->cap = cap;
}

static inline void epub_put(epub_t *e, const void *p, uint32_t len)
{
    epub_reserve(e, len);
    if (len != 0) {
        memcpy(e->bytes + e->len, p, len);
    }
    e->len += len;
}

static inline void epub_u16(epub_t *e, uint32_t v)
{
    uint8_t b[2] = {(uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu)};
    epub_put(e, b, 2);
}

static inline void epub_u32(epub_t *e, uint32_t v)
{
    epub_u16(e, v & 0xFFFFu);
    epub_u16(e, v >> 16);
}

static inline void epub_begin(epub_t *e)
{
    e->len = 0;
    e->count = 0;
    e->dir_offset = 0;
    e->dir_size = 0;
}

static inline void epub_free(epub_t *e)
{
    free(e->bytes);
    memset(e, 0, sizeof *e);
}

/* Adds an entry with a name of any bytes. EPUB_DEFLATED wraps data in stored deflate blocks; any
 * other method but EPUB_STORED writes data as it is under that method number. */
static inline epub_entry_t *epub_add_raw(epub_t *e, const char *name, uint32_t name_len,
                                         const void *data, uint32_t len, uint16_t method)
{
    if (e->count == EPUB_MAX_ENTRIES || name_len > sizeof e->entries[0].name) {
        abort();
    }
    epub_entry_t *entry = &e->entries[e->count++];
    const uint8_t *bytes = data;
    uint32_t blocks = method == EPUB_DEFLATED ? len / 65535u + 1u : 0u;
    uint32_t compressed = len + blocks * 5u;

    entry->local_offset = e->len;
    entry->compressed_size = compressed;
    entry->uncompressed_size = len;
    entry->crc = epub_crc32(bytes, len);
    entry->method = method;
    entry->name_len = (uint16_t)name_len;
    memcpy(entry->name, name, name_len);

    epub_u32(e, 0x04034B50u);
    epub_u16(e, 20);
    epub_u16(e, 0);
    epub_u16(e, method);
    epub_u32(e, 0); /* time and date */
    epub_u32(e, entry->crc);
    epub_u32(e, compressed);
    epub_u32(e, len);
    epub_u16(e, name_len);
    epub_u16(e, 0);
    epub_put(e, name, name_len);
    if (method != EPUB_DEFLATED) {
        epub_put(e, bytes, len);
        return entry;
    }
    for (uint32_t b = 0, in = 0; b < blocks; b++) {
        uint32_t size = len - in < 65535u ? len - in : 65535u;
        uint8_t header[5] = {(uint8_t)(b + 1u == blocks ? 1u : 0u), (uint8_t)(size & 0xFFu),
                             (uint8_t)(size >> 8), (uint8_t)(~size & 0xFFu),
                             (uint8_t)((~size >> 8) & 0xFFu)};
        epub_put(e, header, 5);
        epub_put(e, bytes + in, size);
        in += size;
    }
    return entry;
}

static inline epub_entry_t *epub_add(epub_t *e, const char *name, const char *text, uint16_t method)
{
    return epub_add_raw(e, name, (uint32_t)strlen(name), text, (uint32_t)strlen(text), method);
}

/* Writes the central directory and the end record. */
static inline void epub_finish(epub_t *e)
{
    e->dir_offset = e->len;
    for (uint32_t i = 0; i < e->count; i++) {
        epub_entry_t *entry = &e->entries[i];
        entry->dir_offset = e->len;
        epub_u32(e, 0x02014B50u);
        epub_u16(e, 20);
        epub_u16(e, 20);
        epub_u16(e, 0);
        epub_u16(e, entry->method);
        epub_u32(e, 0);
        epub_u32(e, entry->crc);
        epub_u32(e, entry->compressed_size);
        epub_u32(e, entry->uncompressed_size);
        epub_u16(e, entry->name_len);
        epub_u32(e, 0); /* extra and comment lengths */
        epub_u32(e, 0); /* disk and internal attributes */
        epub_u32(e, 0); /* external attributes */
        epub_u32(e, entry->local_offset);
        epub_put(e, entry->name, entry->name_len);
    }
    e->dir_size = e->len - e->dir_offset;
    epub_u32(e, 0x06054B50u);
    epub_u32(e, 0);
    epub_u16(e, e->count);
    epub_u16(e, e->count);
    epub_u32(e, e->dir_size);
    epub_u32(e, e->dir_offset);
    epub_u16(e, 0);
}

/* ---- EPUB documents ---- */

/* A growable string for building documents. */
typedef struct {
    char *s;
    size_t len;
    size_t cap;
} epub_str_t;

static inline void str_add(epub_str_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        abort();
    }
    if (b->len + (size_t)n + 1u > b->cap) {
        b->cap = (b->len + (size_t)n + 1u) * 2u;
        b->s = realloc(b->s, b->cap);
        if (b->s == NULL) {
            abort();
        }
    }
    vsnprintf(b->s + b->len, (size_t)n + 1u, fmt, ap);
    b->len += (size_t)n;
    va_end(ap);
}

static inline void str_free(epub_str_t *b)
{
    free(b->s);
    memset(b, 0, sizeof *b);
}

static inline void epub_add_mimetype(epub_t *e)
{
    epub_add(e, "mimetype", "application/epub+zip", EPUB_STORED);
}

static inline void epub_add_container(epub_t *e, const char *opf_path)
{
    epub_str_t s = {0};
    str_add(&s,
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
            "full-path=\"%s\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>",
            opf_path);
    epub_add(e, "META-INF/container.xml", s.s, EPUB_DEFLATED);
    str_free(&s);
}

/* An OPF of the parts given, each the inner markup of its element. toc is the spine's toc attribute
 * value, or NULL for none. */
static inline void epub_add_opf(epub_t *e, const char *path, const char *unique_id,
                                const char *metadata, const char *manifest, const char *spine,
                                const char *toc)
{
    epub_str_t s = {0};
    str_add(&s,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"%s\"><metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">%s</metadata><manifest>%s</manifest>"
            "<spine%s%s%s>%s</spine></package>",
            unique_id, metadata, manifest, toc != NULL ? " toc=\"" : "", toc != NULL ? toc : "",
            toc != NULL ? "\"" : "", spine);
    epub_add(e, path, s.s, EPUB_DEFLATED);
    str_free(&s);
}

static inline const char *epub_chapter(epub_str_t *s, const char *body)
{
    s->len = 0;
    str_add(s, "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>%s</p></body></html>", body);
    return s->s;
}

#endif /* BARTAP_TEST_EPUB_BUILD_H */
