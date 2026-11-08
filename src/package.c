/* Layer 2 (R3): container, package and book. Depends only on xml.c and zip.c. */
#include <string.h>

#include "internal.h"

// Bits of work->found: which ids the OPF named, so a hash of 0 is never mistaken for one.
#define FOUND_UNIQUE_ID 0x01u
#define FOUND_TOC_ID 0x02u
#define FOUND_COVER_ID 0x04u

/* ---- Paths (R43) ---- */

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    return (uint8_t)((c | 0x20) - 'a' + 10);
}

bartap_err_t bt_href_resolve(const char *base, uint32_t base_len, const char *href, uint32_t href_len,
                             char *out, uint32_t cap, uint16_t *out_len)
{
    uint32_t len = 0;
    uint32_t end = 0;

    if (cap > 0xFFFFu) {
        // out_len can't say more
        cap = 0xFFFFu;
    }
    // the fragment and query aren't part of the path
    while (end < href_len && href[end] != '#' && href[end] != '?') {
        end++;
    }
    // a scheme before the first '/' makes it a URI of its own: remote, data: and the like aren't in
    // the archive
    for (uint32_t i = 0; i < end && href[i] != '/'; i++) {
        if (href[i] == ':') {
            return BARTAP_ERR_UNSUPPORTED;
        }
    }

    if (end == 0) {
        // only a fragment, or nothing: the document itself
        if (base_len > cap) {
            return BARTAP_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(out, base, base_len);
        *out_len = (uint16_t)base_len;
        return BARTAP_OK;
    }
    if (href[0] != '/') {
        // relative: start from base's directory, everything up to its last '/'
        uint32_t dir_len = base_len;
        while (dir_len > 0 && base[dir_len - 1u] != '/') {
            dir_len--;
        }
        if (dir_len > cap) {
            return BARTAP_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(out, base, dir_len);
        len = dir_len;
    }
    // an absolute path starts from the archive's root, so out stays empty

    // one segment at a time. out always ends in '/' or is empty here
    uint32_t i = 0;
    while (i < end) {
        uint32_t start = i;
        while (i < end && href[i] != '/') {
            i++;
        }
        uint32_t seg_len = i - start;
        bool last = i == end;
        i++; // past the '/'

        if (seg_len == 0 || (seg_len == 1 && href[start] == '.')) {
            // "a//b" and "./": nothing to add
            continue;
        }
        if (seg_len == 2 && href[start] == '.' && href[start + 1u] == '.') {
            // up one directory, but never above the root (R29)
            if (len > 0) {
                len--;
                while (len > 0 && out[len - 1u] != '/') {
                    len--;
                }
            }
            continue;
        }
        // copy the segment, percent-decoded once
        for (uint32_t k = start; k < start + seg_len; k++) {
            char c = href[k];
            // both hex digits must be inside the segment
            if (c == '%' && k + 2u < start + seg_len && is_hex(href[k + 1u]) &&
                is_hex(href[k + 2u])) {
                c = (char)((hex_value(href[k + 1u]) << 4) | hex_value(href[k + 2u]));
                k += 2;
                if (c == '/') {
                    // %2F is data, not a separator, and no entry name can hold it (R43)
                    return BARTAP_ERR_CORRUPT;
                }
            }
            // a '%' without two hex digits stays as it is
            if (len >= cap) {
                return BARTAP_ERR_BUFFER_TOO_SMALL;
            }
            out[len++] = c;
        }
        if (!last) {
            if (len >= cap) {
                return BARTAP_ERR_BUFFER_TOO_SMALL;
            }
            out[len++] = '/';
        }
    }
    *out_len = (uint16_t)len;
    return BARTAP_OK;
}

/* ---- Helpers ---- */

static uint32_t hash_of(const char *p, uint32_t len)
{
    return bt_name_hash(BT_NAME_HASH_INIT, p, len);
}

// Whether a token is a tag with this local name. Names are already folded (R46).
static bool is_tag(const bartap_xml_token_t *t, bartap_xml_token_kind_t kind, const char *name,
                   uint32_t name_len)
{
    return t->kind == kind && t->name_len == name_len && memcmp(t->name, name, name_len) == 0;
}

#define IS_START(t, name) is_tag(t, BARTAP_XML_START_TAG, name, sizeof name - 1u)
#define IS_END(t, name) is_tag(t, BARTAP_XML_END_TAG, name, sizeof name - 1u)

// An attribute of the current tag, or false if it's missing.
static bool attr(bartap_open_work_t *work, const char *name, uint32_t name_len, const char **value,
                 uint32_t *value_len)
{
    return bt_xml_attr(&work->xml, name, name_len, value, value_len);
}

#define ATTR(work, name, value, value_len) attr(work, name, sizeof name - 1u, value, value_len)

// Compares a value with a known ASCII string, ignoring case, as media types are.
static bool equals_folded(const char *value, uint32_t value_len, const char *want, uint32_t want_len)
{
    if (value_len != want_len) {
        return false;
    }
    for (uint32_t i = 0; i < value_len; i++) {
        char a = value[i];
        char b = want[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

#define EQUALS(value, len, want) equals_folded(value, len, want, sizeof want - 1u)

// Whether a space-separated list, such as properties="nav scripted", holds a token. Case matters.
bool bt_has_token(const char *list, uint32_t list_len, const char *token, uint32_t token_len)
{
    uint32_t i = 0;
    while (i < list_len) {
        while (i < list_len && (list[i] == ' ' || list[i] == '\t' || list[i] == '\n' || list[i] == '\r')) {
            i++;
        }
        uint32_t start = i;
        while (i < list_len && list[i] != ' ' && list[i] != '\t' && list[i] != '\n' && list[i] != '\r') {
            i++;
        }
        if (i - start == token_len && memcmp(list + start, token, token_len) == 0) {
            return true;
        }
    }
    return false;
}

#define HAS_TOKEN(list, len, token) bt_has_token(list, len, token, sizeof token - 1u)

static void book_warn(bartap_book_t *book, bartap_warning_t code, uint16_t spine_index,
                      uint32_t offset)
{
    bt_warn(&book->warnings, code, spine_index, offset);
}

// Opens an archive entry by name and starts tokenizing it, through the open work's stream.
static bartap_err_t open_document(bartap_book_t *book, bartap_open_work_t *work, const char *path,
                                  uint16_t path_len, uint8_t *window)
{
    bartap_entry_t entry;

    bartap_err_t err = bt_dir_find(&book->directory, path, path_len, &entry);
    if (err == BARTAP_END) {
        // no such entry. container.xml and the OPF are what make a book, so it's unreadable
        return BARTAP_ERR_CORRUPT;
    }
    if (err != BARTAP_OK) {
        return err;
    }
    err = bt_stream_open(&work->stream, book->source, &entry, window);
    if (err != BARTAP_OK) {
        // an unsupported method or a bad local header, pass it up
        return err;
    }
    bt_bytes_init_stream(&work->bytes, &work->stream, NULL);
    bt_xml_init(&work->xml, &work->bytes, &book->warnings, BARTAP_NO_SPINE);
    return BARTAP_OK;
}

/* ---- Container (R23) ---- */

// Finds the OPF's path in META-INF/container.xml: the first rootfile of the OPF media type, else the
// first rootfile at all. Directory walk 1 of 3.
bartap_err_t bt_read_container(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window)
{
    static const char container[] = "META-INF/container.xml";
    bool have_fallback = false;

    bartap_err_t err = open_document(book, work, container, sizeof container - 1u, window);
    if (err != BARTAP_OK) {
        return err;
    }
    for (;;) {
        const bartap_xml_token_t *t;
        const char *path;
        const char *type;
        uint32_t path_len;
        uint32_t type_len;

        err = bt_xml_next(&work->xml, &t);
        if (err == BARTAP_END) {
            break;
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if (!IS_START(t, "rootfile") || !ATTR(work, "full-path", &path, &path_len)) {
            continue;
        }
        bool is_opf = ATTR(work, "media-type", &type, &type_len) &&
                      EQUALS(type, type_len, "application/oebps-package+xml");
        if (!is_opf && have_fallback) {
            // a later rootfile of another kind, the fallback stands
            continue;
        }
        // full-path is from the archive's root, so it resolves against an empty base
        err = bt_href_resolve("", 0, path, path_len, work->opf_path, BARTAP_PATH_MAX,
                              &work->opf_path_len);
        if (err == BARTAP_ERR_BUFFER_TOO_SMALL) {
            // valid, but longer than Bartap keeps (R33)
            book_warn(book, BARTAP_WARN_PATH_TOO_LONG, BARTAP_NO_SPINE, t->offset);
            return BARTAP_ERR_UNSUPPORTED;
        }
        if (err != BARTAP_OK) {
            // a path no entry can have, the OPF can't be found
            return BARTAP_ERR_CORRUPT;
        }
        have_fallback = true;
        if (is_opf) {
            break;
        }
    }
    // no rootfile, nowhere to go
    return have_fallback ? BARTAP_OK : BARTAP_ERR_CORRUPT;
}

/* ---- OPF (R23, R25, R41) ---- */

static uint32_t utf8_length(uint8_t lead)
{
    return lead < 0x80u ? 1u : lead < 0xE0u ? 2u : lead < 0xF0u ? 3u : 4u;
}

// Copies an element's text into a metadata field, whitespace collapsed and trimmed, cut at a
// character boundary (R25). Reads on to the element's end tag; *truncated says if anything was cut.
static bartap_err_t read_meta(bartap_open_work_t *work, char *dst, uint8_t *dst_len, bool *truncated)
{
    char buf[64];
    uint32_t len = 0;
    uint32_t depth = 0;
    bool space = false;

    *truncated = false;
    for (;;) {
        const bartap_xml_token_t *t;
        bartap_err_t err = bt_xml_next(&work->xml, &t);
        if (err == BARTAP_END) {
            // the OPF ended inside the element, keep what was read
            break;
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if (t->kind == BARTAP_XML_START_TAG) {
            // markup inside a field, such as <span>, is read through
            depth += t->self_closing ? 0u : 1u;
            continue;
        }
        if (t->kind == BARTAP_XML_END_TAG) {
            if (depth == 0) {
                // the element's own end tag
                break;
            }
            depth--;
            continue;
        }
        for (;;) {
            uint32_t got;
            err = bt_xml_text(&work->xml, buf, sizeof buf, &got);
            if (err != BARTAP_OK) {
                return err;
            }
            if (got == 0) {
                break;
            }
            // whole characters only, so a character never straddles two chunks
            for (uint32_t i = 0; i < got;) {
                uint8_t c = (uint8_t)buf[i];
                uint32_t n = utf8_length(c);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    // held, and only between words, so the field is trimmed
                    space = len > 0;
                    i++;
                    continue;
                }
                uint32_t need = n + (space ? 1u : 0u);
                if (*truncated || need > BARTAP_META_MAX - len) {
                    // full. keep reading to the end tag, but nothing more fits
                    *truncated = true;
                    i += n;
                    continue;
                }
                if (space) {
                    dst[len++] = ' ';
                    space = false;
                }
                memcpy(dst + len, buf + i, n);
                len += n;
                i += n;
            }
        }
    }
    dst[len] = '\0';
    *dst_len = (uint8_t)len;
    return BARTAP_OK;
}

// One field: read it, warning if it was cut (R25).
static bartap_err_t read_field(bartap_book_t *book, bartap_open_work_t *work, uint32_t at, char *dst,
                               uint8_t *dst_len)
{
    bool truncated;
    bartap_err_t err = read_meta(work, dst, dst_len, &truncated);
    if (err == BARTAP_OK && truncated) {
        book_warn(book, BARTAP_WARN_METADATA_TRUNCATED, BARTAP_NO_SPINE, at);
    }
    return err;
}

// How many compact items the caller's array holds, short of BARTAP_NO_SPINE (R41).
static uint16_t compact_capacity(const bartap_book_t *book)
{
    uint32_t n = (uint32_t)book->spine_capacity * BARTAP_SPINE_COMPACT_PER_ITEM;
    return (uint16_t)(n < BARTAP_NO_SPINE ? n : BARTAP_NO_SPINE - 1u);
}

// Metadata, the itemref count and the idref hashes in the spine slots. The manifest comes before the
// spine, so idrefs can't be resolved yet: the second pass does that. Directory walk 2 of 3 finds the
// OPF.
bartap_err_t bt_opf_first_pass(bartap_book_t *book, bartap_open_work_t *work, uint8_t *window)
{
    bool in_metadata = false;
    bool in_spine = false;
    bool have_title = false;
    bool have_language = false;
    bool found_unique = false;
    bool fallback_truncated = false;
    uint32_t required = 0;

    bartap_err_t err = open_document(book, work, work->opf_path, work->opf_path_len, window);
    if (err != BARTAP_OK) {
        return err;
    }
    for (;;) {
        const bartap_xml_token_t *t;
        const char *value;
        uint32_t value_len;

        err = bt_xml_next(&work->xml, &t);
        if (err == BARTAP_END) {
            break;
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if (IS_END(t, "metadata")) {
            in_metadata = false;
            continue;
        }
        if (IS_END(t, "spine")) {
            in_spine = false;
            continue;
        }
        if (t->kind != BARTAP_XML_START_TAG) {
            continue;
        }
        uint32_t at = t->offset;

        if (IS_START(t, "package")) {
            // names which dc:identifier is the book's (R25)
            if (ATTR(work, "unique-identifier", &value, &value_len)) {
                work->unique_id_hash = hash_of(value, value_len);
                work->found |= FOUND_UNIQUE_ID;
            }
        } else if (IS_START(t, "metadata")) {
            in_metadata = !t->self_closing;
        } else if (IS_START(t, "spine")) {
            in_spine = !t->self_closing;
            // EPUB 2's NCX, by manifest id
            if (ATTR(work, "toc", &value, &value_len)) {
                work->toc_id_hash = hash_of(value, value_len);
                work->found |= FOUND_TOC_ID;
            }
        } else if (in_metadata && !t->self_closing && IS_START(t, "title") && !have_title) {
            // the first dc:title only
            have_title = true;
            err = read_field(book, work, at, book->title, &book->title_len);
        } else if (in_metadata && !t->self_closing && IS_START(t, "language") && !have_language) {
            have_language = true;
            err = read_field(book, work, at, book->language, &book->language_len);
        } else if (in_metadata && IS_START(t, "creator")) {
            // every one counts, the first is kept as it is (R25)
            if (book->creator_count == 0 && !t->self_closing) {
                err = read_field(book, work, at, book->creator, &book->creator_len);
            }
            if (book->creator_count < 255u) {
                book->creator_count++;
            }
        } else if (in_metadata && !t->self_closing && IS_START(t, "identifier") && !found_unique) {
            // the one unique-identifier names, else the first non-empty one stands in (R25)
            bool is_unique = (work->found & FOUND_UNIQUE_ID) != 0 &&
                             ATTR(work, "id", &value, &value_len) &&
                             hash_of(value, value_len) == work->unique_id_hash;
            if (is_unique) {
                found_unique = true;
                err = read_field(book, work, at, book->identifier, &book->identifier_len);
            } else if (book->identifier_len == 0) {
                // a stand-in, which a later unique one replaces. its truncation is only reported
                // if it's kept
                err = read_meta(work, book->identifier, &book->identifier_len, &fallback_truncated);
            }
        } else if (in_metadata && IS_START(t, "meta")) {
            // EPUB 2's cover: <meta name="cover" content="manifest id">
            const char *name;
            uint32_t name_len;
            if (ATTR(work, "name", &name, &name_len) && EQUALS(name, name_len, "cover") &&
                ATTR(work, "content", &value, &value_len)) {
                work->cover_id_hash = hash_of(value, value_len);
                work->found |= FOUND_COVER_ID;
            }
        } else if (in_spine && IS_START(t, "itemref")) {
            // counted whether or not it fits, so the caller learns what it would need (R41)
            required++;
            if (book->spine_count < compact_capacity(book)) {
                // compact for now: how many items there are is only known at the end (R41)
                if (book->spine_count % BARTAP_SPINE_COMPACT_PER_ITEM == 0) {
                    // a slot's first item clears it all, so no stale byte is left past its last
                    memset(&book->spine[book->spine_count / BARTAP_SPINE_COMPACT_PER_ITEM], 0,
                           sizeof book->spine[0]);
                }
                bartap_spine_compact_t *slot = BT_SPINE_COMPACT(book->spine, book->spine_count);
                book->spine_count++;
                if (ATTR(work, "idref", &value, &value_len)) {
                    // the idref's hash waits here for the manifest, in the second pass
                    slot->path_hash = hash_of(value, value_len);
                    slot->flags |= BT_SPINE_PENDING;
                }
                // an itemref without one can never resolve, and is reported as missing
                if (ATTR(work, "linear", &value, &value_len) && EQUALS(value, value_len, "no")) {
                    // non-linear items are the caller's policy (R17)
                    slot->flags |= BT_SPINE_NONLINEAR;
                }
            }
        }
        if (err != BARTAP_OK) {
            return err;
        }
    }

    book->spine_required = required;
    if (required > book->spine_count) {
        // later chapters are unreachable, even as compact items. a bigger array would hold them (R41)
        book_warn(book, BARTAP_WARN_SPINE_TRUNCATED, BARTAP_NO_SPINE, required);
    }
    if (!found_unique) {
        // the stand-in, or nothing at all (R25)
        book_warn(book, BARTAP_WARN_NO_UNIQUE_ID, BARTAP_NO_SPINE, 0);
        if (fallback_truncated) {
            book_warn(book, BARTAP_WARN_METADATA_TRUNCATED, BARTAP_NO_SPINE, 0);
        }
    }
    return BARTAP_OK;
}

// Notes a path as the nav, NCX or cover, the first one only.
static void set_path(uint32_t *hash, uint16_t *len, uint32_t path_hash, uint16_t path_len)
{
    if (*len == 0) {
        *hash = path_hash;
        *len = path_len;
    }
}

// The manifest: idrefs become resolved path hashes and lengths, and the nav, NCX and cover are
// captured. Reads the OPF again, from the window if it's small, else from the card. Spine items are
// still compact here.
bartap_err_t bt_opf_second_pass(bartap_book_t *book, bartap_open_work_t *work)
{
    bool in_manifest = false;

    bartap_err_t err = bt_xml_seek(&work->xml, 0);
    if (err != BARTAP_OK) {
        return err;
    }
    for (;;) {
        const bartap_xml_token_t *t;
        const char *id;
        const char *href;
        const char *value;
        uint32_t id_len;
        uint32_t href_len;
        uint32_t value_len;

        err = bt_xml_next(&work->xml, &t);
        if (err == BARTAP_END) {
            break;
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if (IS_END(t, "manifest")) {
            in_manifest = false;
            continue;
        }
        if (IS_START(t, "manifest")) {
            in_manifest = !t->self_closing;
            continue;
        }
        if (!in_manifest || !IS_START(t, "item") || !ATTR(work, "id", &id, &id_len) ||
            !ATTR(work, "href", &href, &href_len)) {
            // only manifest items with both matter
            continue;
        }
        uint32_t id_hash = hash_of(id, id_len);
        bool has_props = ATTR(work, "properties", &value, &value_len);
        bool is_nav = has_props && HAS_TOKEN(value, value_len, "nav");
        bool is_cover = (has_props && HAS_TOKEN(value, value_len, "cover-image")) ||
                        ((work->found & FOUND_COVER_ID) != 0 && id_hash == work->cover_id_hash);
        bool is_ncx;
        if ((work->found & FOUND_TOC_ID) != 0) {
            is_ncx = id_hash == work->toc_id_hash;
        } else {
            // no spine toc attribute: the NCX by its media type
            is_ncx = ATTR(work, "media-type", &value, &value_len) &&
                     EQUALS(value, value_len, "application/x-dtbncx+xml");
        }
        bool for_spine = false;
        for (uint16_t i = 0; i < book->spine_count && !for_spine; i++) {
            const bartap_spine_compact_t *slot = BT_SPINE_COMPACT(book->spine, i);
            for_spine = (slot->flags & BT_SPINE_PENDING) != 0 && slot->path_hash == id_hash;
        }
        if (!for_spine && !is_nav && !is_cover && !is_ncx) {
            // nothing wants this item
            continue;
        }

        // hrefs are relative to the OPF itself
        uint16_t path_len = 0;
        err = bt_href_resolve(work->opf_path, work->opf_path_len, href, href_len, work->resolved,
                              BARTAP_PATH_MAX, &path_len);
        if (err == BARTAP_ERR_BUFFER_TOO_SMALL) {
            book_warn(book, BARTAP_WARN_PATH_TOO_LONG, BARTAP_NO_SPINE, t->offset);
        }
        // anything unresolved (too long, remote, %2F) has no entry: length 0 never matches one
        if (err != BARTAP_OK) {
            path_len = 0;
        }
        uint32_t path_hash = hash_of(work->resolved, path_len);

        for (uint16_t i = 0; i < book->spine_count; i++) {
            bartap_spine_compact_t *slot = BT_SPINE_COMPACT(book->spine, i);
            if ((slot->flags & BT_SPINE_PENDING) != 0 && slot->path_hash == id_hash) {
                // resolved, or settled as missing. a later item with the same id doesn't count
                slot->path_hash = path_hash;
                slot->path_len = path_len;
                slot->flags &= (uint8_t)~BT_SPINE_PENDING;
            }
        }
        if (path_len == 0) {
            continue;
        }
        if (is_nav) {
            set_path(&work->nav_path_hash, &work->nav_path_len, path_hash, path_len);
        }
        if (is_ncx) {
            set_path(&work->ncx_path_hash, &work->ncx_path_len, path_hash, path_len);
        }
        if (is_cover) {
            set_path(&work->cover_path_hash, &work->cover_path_len, path_hash, path_len);
        }
    }

    // idrefs with no manifest item are settled as missing
    for (uint16_t i = 0; i < book->spine_count; i++) {
        bartap_spine_compact_t *slot = BT_SPINE_COMPACT(book->spine, i);
        if ((slot->flags & BT_SPINE_PENDING) != 0) {
            slot->flags &= (uint8_t)~BT_SPINE_PENDING;
            slot->path_len = 0;
        }
    }
    return BARTAP_OK;
}

/* ---- Spine layout (R41) ---- */

// The OPF passes leave compact items. If the book's items fit full slots, they are spread into them,
// so a chapter opens with no lookup; otherwise they stay compact. Last first: full slot i covers
// compact items 3i to 3i+2, all already moved for i above 0, and item 0 is copied out before its
// slot is written.
static void settle_spine(bartap_book_t *book)
{
    if (book->spine_count > book->spine_capacity) {
        return;
    }
    for (uint16_t i = book->spine_count; i-- != 0;) {
        bartap_spine_compact_t item = *BT_SPINE_COMPACT(book->spine, i);
        bartap_spine_item_t *slot = &book->spine[i];
        memset(slot, 0, sizeof *slot);
        slot->path_hash = item.path_hash;
        slot->path_len = item.path_len;
        slot->flags = item.flags;
        slot->dir_offset = BARTAP_NO_DIR_OFFSET;
    }
    book->spine_compact = false;
}

/* ---- The directory walk (R22, R23, R44) ---- */

// Fills a path's directory-record offset on its first match. Returns true for a second match, a
// duplicate name (R44).
static bool match_path(uint32_t *dir_offset, uint32_t path_hash, uint16_t path_len,
                       uint32_t name_hash, uint16_t name_len, uint32_t record)
{
    if (path_len == 0 || path_len != name_len || path_hash != name_hash) {
        return false;
    }
    if (*dir_offset != BARTAP_NO_DIR_OFFSET) {
        return true;
    }
    *dir_offset = record;
    return false;
}

// One full walk fills every spine slot, and finds the nav, NCX and cover, all by hash and length
// (R22: a collision is accepted). Directory walk 3 of 3.
bartap_err_t bt_match_spine(bartap_book_t *book, bartap_open_work_t *work)
{
    bt_dir_cursor_t cursor;

    bt_dir_begin(&book->directory, &cursor);
    for (;;) {
        bartap_entry_t entry;
        uint32_t name_hash;
        uint16_t name_len;
        bool duplicate = false;

        bartap_err_t err = bt_dir_next(&book->directory, &cursor, &entry, &name_hash, &name_len);
        if (err == BARTAP_END) {
            break;
        }
        if (err != BARTAP_OK) {
            // the directory itself is bad, nothing can be trusted
            return err;
        }
        for (uint16_t i = 0; i < book->spine_count; i++) {
            if (book->spine_compact) {
                // the path is all a compact item keeps: it notes the match, and is looked up again
                // by the same rule when opened (R41)
                bartap_spine_compact_t *compact = BT_SPINE_COMPACT(book->spine, i);
                if (compact->path_len != 0 && compact->path_len == name_len &&
                    compact->path_hash == name_hash) {
                    duplicate = duplicate || (compact->flags & BT_SPINE_FOUND) != 0;
                    compact->flags |= BT_SPINE_FOUND;
                }
                continue;
            }
            bartap_spine_item_t *slot = &book->spine[i];
            if (slot->path_len == 0 || slot->path_len != name_len || slot->path_hash != name_hash) {
                continue;
            }
            if (slot->dir_offset != BARTAP_NO_DIR_OFFSET) {
                // a second entry with an already matched name: the first won (R44)
                duplicate = duplicate || slot->dir_offset != entry.dir_offset;
                continue;
            }
            // the entry's facts, so a chapter opens with no lookup (R41)
            slot->dir_offset = entry.dir_offset;
            slot->local_offset = entry.local_offset;
            slot->compressed_size = entry.compressed_size;
            slot->uncompressed_size = entry.uncompressed_size;
            slot->crc32 = entry.crc32;
            slot->method = (uint8_t)entry.method; // normalised to fit by the directory code
        }
        duplicate |= match_path(&book->nav_dir_offset, work->nav_path_hash, work->nav_path_len,
                                name_hash, name_len, entry.dir_offset);
        duplicate |= match_path(&book->ncx_dir_offset, work->ncx_path_hash, work->ncx_path_len,
                                name_hash, name_len, entry.dir_offset);
        duplicate |= match_path(&book->cover_dir_offset, work->cover_path_hash,
                                work->cover_path_len, name_hash, name_len, entry.dir_offset);
        if (duplicate) {
            book_warn(book, BARTAP_WARN_DUPLICATE_NAME, BARTAP_NO_SPINE, entry.dir_offset);
        }
    }

    // a slot nothing matched fails when opened, not now (R23)
    for (uint16_t i = 0; i < book->spine_count; i++) {
        bool found = book->spine_compact ? (BT_SPINE_COMPACT(book->spine, i)->flags & BT_SPINE_FOUND) != 0
                                         : book->spine[i].dir_offset != BARTAP_NO_DIR_OFFSET;
        if (!found) {
            book_warn(book, BARTAP_WARN_SPINE_ITEM_MISSING, i, 0);
        }
    }
    return BARTAP_OK;
}

/* ---- Book ---- */

// R30's packaging check, from the first directory record alone so it costs no walk: mimetype should
// be first and stored. Missing and not first look the same from here, and get the same warning.
static bartap_err_t check_packaging(bartap_book_t *book)
{
    static const char mimetype[] = "mimetype";
    bt_dir_cursor_t cursor;
    bartap_entry_t entry;
    uint32_t name_hash;
    uint16_t name_len;

    bt_dir_begin(&book->directory, &cursor);
    bartap_err_t err = bt_dir_next(&book->directory, &cursor, &entry, &name_hash, &name_len);
    if (err == BARTAP_END) {
        // an empty archive, the container lookup will say so
        return BARTAP_OK;
    }
    if (err != BARTAP_OK) {
        return err;
    }
    bool first = name_len == sizeof mimetype - 1u &&
                 name_hash == hash_of(mimetype, sizeof mimetype - 1u);
    if (!first || entry.method != 0) {
        book_warn(book, BARTAP_WARN_PACKAGING, BARTAP_NO_SPINE, entry.dir_offset);
    }
    return BARTAP_OK;
}

bartap_err_t bartap_open(bartap_book_t *book, const bartap_source_t *source,
                         bartap_spine_item_t *spine, uint16_t spine_capacity,
                         uint8_t *dir_buf, uint32_t dir_buf_size,
                         bartap_open_work_t *work, uint8_t *window,
                         bartap_warn_fn warn_fn, void *warn_user)
{
    if (book == NULL || source == NULL || work == NULL || window == NULL ||
        (spine == NULL && spine_capacity != 0)) {
        return BARTAP_ERR_ARG;
    }
    // a clean slate, so nothing from an earlier book survives a failure
    memset(book, 0, sizeof *book);
    book->source = source;
    book->spine = spine;
    book->spine_capacity = spine_capacity;
    // the OPF passes write compact items, settled once their number is known (R41)
    book->spine_compact = true;
    book->warnings.fn = warn_fn;
    book->warnings.user = warn_user;
    book->nav_dir_offset = BARTAP_NO_DIR_OFFSET;
    book->ncx_dir_offset = BARTAP_NO_DIR_OFFSET;
    book->cover_dir_offset = BARTAP_NO_DIR_OFFSET;
    work->found = 0;
    work->nav_path_len = 0;
    work->ncx_path_len = 0;
    work->cover_path_len = 0;

    // raw or none is chosen here, once (R24)
    bartap_err_t err = bt_dir_init(&book->directory, source, dir_buf, dir_buf_size);
    if (err == BARTAP_OK) {
        err = check_packaging(book);
    }
    if (err == BARTAP_OK) {
        err = bt_read_container(book, work, window);
    }
    if (err == BARTAP_OK) {
        err = bt_opf_first_pass(book, work, window);
    }
    if (err == BARTAP_OK) {
        err = bt_opf_second_pass(book, work);
    }
    if (err == BARTAP_OK) {
        settle_spine(book);
        err = bt_match_spine(book, work);
    }
    if (err != BARTAP_OK) {
        // the book can't be used. its warnings still say what was seen on the way
        return err;
    }
    // bookmarks carry this, so they can't be applied to another book (R12)
    book->book_hash = hash_of(book->identifier, book->identifier_len);
    return BARTAP_OK;
}

bartap_err_t bartap_book_metadata(const bartap_book_t *book, bartap_metadata_t *out)
{
    out->title = book->title;
    out->creator = book->creator;
    out->language = book->language;
    out->identifier = book->identifier;
    out->title_len = book->title_len;
    out->creator_len = book->creator_len;
    out->language_len = book->language_len;
    out->identifier_len = book->identifier_len;
    out->creator_count = book->creator_count;
    return BARTAP_OK;
}

bartap_err_t bartap_book_spine_count(const bartap_book_t *book, uint16_t *count, uint32_t *required)
{
    *count = book->spine_count;
    *required = book->spine_required;
    return BARTAP_OK;
}

bartap_err_t bartap_book_spine_linear(const bartap_book_t *book, uint16_t spine_index, bool *linear)
{
    if (spine_index >= book->spine_count) {
        return BARTAP_ERR_ARG;
    }
    *linear = (bt_spine_flags(book, spine_index) & BT_SPINE_NONLINEAR) == 0;
    return BARTAP_OK;
}

bartap_err_t bartap_book_cover(const bartap_book_t *book, uint32_t *dir_offset)
{
    if (book->cover_dir_offset == BARTAP_NO_DIR_OFFSET) {
        // no cover named, or its entry is missing
        return BARTAP_END;
    }
    *dir_offset = book->cover_dir_offset;
    return BARTAP_OK;
}

uint32_t bartap_book_warnings(const bartap_book_t *book)
{
    return book->warnings.bits;
}
