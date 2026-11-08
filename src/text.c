/* Layer 3 (R3): records, TOC, bookmarks, seek. Depends only on package.c, xml.c and zip.c. */
#include "internal.h"

#include <string.h>

/* ---- Element classes and records (R4, R6, R45, R46, R47) ---- */

enum {
    CLASS_CONTAINER = 1,
    CLASS_BLOCK,
    CLASS_INLINE,
    CLASS_BOUNDARY,
    CLASS_ASIDE, /* a container or a boundary, by its epub:type */
    CLASS_SKIP,
    CLASS_IMAGE,
    CLASS_BREAK
};

enum { EMIT_NONE = 0, EMIT_CONTAINER_START, EMIT_BLOCK_START, EMIT_STYLE, EMIT_IMAGE, EMIT_BREAK };

#define FRAME_REVERSED 0x01u /* a reversed list with a start, counting down (R45) */
#define FRAME_LIST 0x02u     /* ol or ul, which number the items inside them */
#define UNWIND_NONE 0xFFu

typedef struct {
    char name[11];
    uint8_t len;
    uint8_t class_kind;
    uint8_t kind;  /* block or container kind, or style bit */
    uint8_t level; /* headings */
} tag_info_t;

#define TAG(name, class_kind, kind, level) {name, sizeof name - 1u, class_kind, kind, level}

// The elements of R46, br included. A tag's index here is what frames and ignored counts hold.
static const tag_info_t tags[] = {
    TAG("blockquote", CLASS_CONTAINER, BARTAP_CONTAINER_BLOCKQUOTE, 0),
    TAG("ol", CLASS_CONTAINER, BARTAP_CONTAINER_OL, 0),
    TAG("ul", CLASS_CONTAINER, BARTAP_CONTAINER_UL, 0),
    TAG("li", CLASS_CONTAINER, BARTAP_CONTAINER_LI, 0),
    TAG("dl", CLASS_CONTAINER, BARTAP_CONTAINER_DL, 0),
    TAG("dd", CLASS_CONTAINER, BARTAP_CONTAINER_DD, 0),
    TAG("aside", CLASS_ASIDE, BARTAP_CONTAINER_NOTE, 0),
    TAG("p", CLASS_BLOCK, BARTAP_BLOCK_PARAGRAPH, 0),
    TAG("h1", CLASS_BLOCK, BARTAP_BLOCK_HEADING, 1),
    TAG("h2", CLASS_BLOCK, BARTAP_BLOCK_HEADING, 2),
    TAG("h3", CLASS_BLOCK, BARTAP_BLOCK_HEADING, 3),
    TAG("h4", CLASS_BLOCK, BARTAP_BLOCK_HEADING, 4),
    TAG("h5", CLASS_BLOCK, BARTAP_BLOCK_HEADING, 5),
    TAG("h6", CLASS_BLOCK, BARTAP_BLOCK_HEADING, 6),
    TAG("pre", CLASS_BLOCK, BARTAP_BLOCK_PRE, 0),
    TAG("hr", CLASS_BLOCK, BARTAP_BLOCK_HR, 0),
    TAG("dt", CLASS_BLOCK, BARTAP_BLOCK_DT, 0),
    TAG("i", CLASS_INLINE, BARTAP_STYLE_ITALIC, 0),
    TAG("em", CLASS_INLINE, BARTAP_STYLE_ITALIC, 0),
    TAG("cite", CLASS_INLINE, BARTAP_STYLE_ITALIC, 0),
    TAG("b", CLASS_INLINE, BARTAP_STYLE_BOLD, 0),
    TAG("strong", CLASS_INLINE, BARTAP_STYLE_BOLD, 0),
    TAG("code", CLASS_INLINE, BARTAP_STYLE_MONO, 0),
    TAG("kbd", CLASS_INLINE, BARTAP_STYLE_MONO, 0),
    TAG("samp", CLASS_INLINE, BARTAP_STYLE_MONO, 0),
    TAG("tt", CLASS_INLINE, BARTAP_STYLE_MONO, 0),
    TAG("sub", CLASS_INLINE, BARTAP_STYLE_SUB, 0),
    TAG("sup", CLASS_INLINE, BARTAP_STYLE_SUP, 0),
    TAG("u", CLASS_INLINE, BARTAP_STYLE_UNDERLINE, 0),
    TAG("ins", CLASS_INLINE, BARTAP_STYLE_UNDERLINE, 0),
    TAG("s", CLASS_INLINE, BARTAP_STYLE_STRIKE, 0),
    TAG("del", CLASS_INLINE, BARTAP_STYLE_STRIKE, 0),
    TAG("strike", CLASS_INLINE, BARTAP_STYLE_STRIKE, 0),
    TAG("div", CLASS_BOUNDARY, 0, 0),
    TAG("section", CLASS_BOUNDARY, 0, 0),
    TAG("article", CLASS_BOUNDARY, 0, 0),
    TAG("header", CLASS_BOUNDARY, 0, 0),
    TAG("footer", CLASS_BOUNDARY, 0, 0),
    TAG("nav", CLASS_BOUNDARY, 0, 0),
    TAG("figure", CLASS_BOUNDARY, 0, 0),
    TAG("address", CLASS_BOUNDARY, 0, 0),
    TAG("body", CLASS_BOUNDARY, 0, 0),
    TAG("table", CLASS_BOUNDARY, 0, 0),
    TAG("tr", CLASS_BOUNDARY, 0, 0),
    TAG("td", CLASS_BOUNDARY, 0, 0),
    TAG("th", CLASS_BOUNDARY, 0, 0),
    TAG("caption", CLASS_BOUNDARY, 0, 0),
    TAG("head", CLASS_SKIP, 0, 0),
    TAG("style", CLASS_SKIP, 0, 0),
    TAG("script", CLASS_SKIP, 0, 0),
    TAG("title", CLASS_SKIP, 0, 0),
    TAG("desc", CLASS_SKIP, 0, 0),
    TAG("img", CLASS_IMAGE, 0, 0),
    TAG("image", CLASS_IMAGE, 0, 0),
    TAG("br", CLASS_BREAK, 0, 0),
};

_Static_assert(sizeof tags / sizeof tags[0] == BARTAP_TAG_COUNT, "BARTAP_TAG_COUNT is the table's size");

uint8_t bt_tag_lookup(const char *name, uint32_t name_len)
{
    for (uint8_t i = 0; i < BARTAP_TAG_COUNT; i++) {
        if (tags[i].len == name_len && memcmp(tags[i].name, name, name_len) == 0) {
            return i;
        }
    }
    // not in R46, transparent
    return BARTAP_TAG_COUNT;
}

static void text_warn(bartap_text_t *text, bartap_warning_t code, uint32_t offset)
{
    bt_warn(&text->warnings, code, text->spine_index, offset);
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Bytes in the UTF-8 character led by c. The tokenizer only hands on valid UTF-8 (R30).
static uint32_t char_len(uint8_t c)
{
    return c < 0x80u ? 1u : c < 0xE0u ? 2u : c < 0xF0u ? 3u : 4u;
}

// hr, br and images take no frame, so an unclosed one can't swallow what follows (R46).
static bool takes_frame(uint8_t tag)
{
    uint8_t c = tags[tag].class_kind;
    if (c == CLASS_BLOCK) {
        return tags[tag].kind != BARTAP_BLOCK_HR;
    }
    return c != CLASS_IMAGE && c != CLASS_BREAK && c != CLASS_SKIP;
}

static uint8_t container_kind(const bartap_frame_t *frame)
{
    // an aside frame is only ever a container as a note
    return tags[frame->tag].kind;
}

// The output block ends, whatever ends it. A pending space never outlives its block (R4).
static void end_block(bartap_parse_state_t *p)
{
    p->block_live = 0;
    p->space_pending = 0;
}

// Opens an output block and queues its start record and style (R8, R16). after is what follows
// the style: nothing, an image or a line break.
static void begin_block(bartap_text_t *text, uint8_t kind, uint8_t level, uint32_t offset,
                        uint8_t after)
{
    bartap_parse_state_t *p = &text->parse;
    p->block_live = 1;
    p->block_kind = kind;
    p->block_level = level;
    p->block_has_content = 0;
    p->space_allowed = 0;
    p->space_pending = 0;
    p->block_offset = offset;
    p->char_offset = 0;
    text->emit_state = EMIT_BLOCK_START;
    text->emit_next = after;
}

// Content with no live block: restarts the nearest block frame's kind, or opens an implicit
// paragraph (R46). The search stops at a container, so nothing restarts a block from outside it.
static void begin_content(bartap_text_t *text, uint32_t offset, uint8_t after)
{
    bartap_parse_state_t *p = &text->parse;
    for (uint8_t i = p->depth; i > 0; i--) {
        const bartap_frame_t *f = &p->frames[i - 1u];
        if (f->class_kind == CLASS_CONTAINER) {
            break;
        }
        if (f->class_kind == CLASS_BLOCK) {
            // a restart, same kind and level
            begin_block(text, tags[f->tag].kind, tags[f->tag].level, offset, after);
            return;
        }
    }
    begin_block(text, BARTAP_BLOCK_PARAGRAPH, 0, offset, after);
}

static void push_frame(bartap_parse_state_t *p, uint8_t tag, uint8_t class_kind)
{
    bartap_frame_t *f = &p->frames[p->depth++];
    f->tag = tag;
    f->class_kind = class_kind;
    f->flags = 0;
    f->style = p->style;
    f->ordinal = 0;
}

// Pops the top frame. true with a record if it was a container (R45).
static bool pop_frame(bartap_parse_state_t *p, bartap_record_t *rec)
{
    const bartap_frame_t *f = &p->frames[--p->depth];
    p->style = f->style;
    switch (f->class_kind) {
    case CLASS_BLOCK:
        // its block, or a restart of it, ends here
        end_block(p);
        return false;
    case CLASS_BOUNDARY:
        if (p->block_has_content) {
            // words either side never fuse (R46)
            end_block(p);
        }
        return false;
    case CLASS_CONTAINER:
        end_block(p);
        rec->kind = BARTAP_REC_CONTAINER_END;
        rec->container.kind = (bartap_container_kind_t)container_kind(f);
        rec->container.ordinal = 0;
        return true;
    default:
        // inline, its style restored above
        return false;
    }
}

bool bt_text_emit_pending(bartap_text_t *text, bartap_record_t *rec)
{
    bartap_parse_state_t *p = &text->parse;

    // starts first, so a self-closing tag's start comes out before its end
    switch (text->emit_state) {
    case EMIT_CONTAINER_START: {
        // the container is the top frame, nothing has been pushed since
        const bartap_frame_t *f = &p->frames[p->depth - 1u];
        rec->kind = BARTAP_REC_CONTAINER_START;
        rec->container.kind = (bartap_container_kind_t)container_kind(f);
        rec->container.ordinal = tags[f->tag].kind == BARTAP_CONTAINER_LI ? f->ordinal : 0;
        text->emit_state = EMIT_NONE;
        return true;
    }
    case EMIT_BLOCK_START:
        rec->kind = BARTAP_REC_BLOCK_START;
        rec->block.kind = (bartap_block_kind_t)p->block_kind;
        rec->block.level = p->block_level;
        rec->block.spine_index = text->spine_index;
        rec->block.block_offset = p->block_offset;
        rec->block.char_offset = p->char_offset;
        // every block start is followed by a style (R8)
        text->emit_state = EMIT_STYLE;
        return true;
    case EMIT_STYLE:
        rec->kind = BARTAP_REC_STYLE;
        rec->style.bits = p->style;
        p->style_out = p->style;
        text->emit_state = text->emit_next;
        text->emit_next = EMIT_NONE;
        return true;
    case EMIT_IMAGE:
        rec->kind = BARTAP_REC_IMAGE;
        rec->image.dir_offset = text->image_dir_offset;
        text->emit_state = EMIT_NONE;
        return true;
    case EMIT_BREAK:
        rec->kind = BARTAP_REC_LINE_BREAK;
        text->emit_state = EMIT_NONE;
        return true;
    default:
        break;
    }

    // then whatever an end tag unwound, one frame at a time so each container end has its kind
    if (text->unwind_to != UNWIND_NONE) {
        while (p->depth > text->unwind_to) {
            if (pop_frame(p, rec)) {
                return true;
            }
        }
        text->unwind_to = UNWIND_NONE;
    }
    return false;
}

// Reads an ordinal attribute as HTML parses integers: optional space and sign, then digits, any
// junk after them ignored. Out of int32 range clamps, with a warning (R45). false if no digits.
static bool read_ordinal(bartap_text_t *text, const char *name, uint32_t name_len, uint32_t offset,
                         int32_t *out)
{
    const char *v;
    uint32_t len;
    uint32_t i = 0;
    bool negative = false;
    int64_t n = 0;
    bool clamped = false;

    if (!bt_xml_attr(&text->xml, name, name_len, &v, &len)) {
        return false;
    }
    while (i < len && is_space(v[i])) {
        i++;
    }
    if (i < len && (v[i] == '-' || v[i] == '+')) {
        negative = v[i] == '-';
        i++;
    }
    if (i == len || v[i] < '0' || v[i] > '9') {
        // no number, as if absent
        return false;
    }
    for (; i < len && v[i] >= '0' && v[i] <= '9'; i++) {
        n = n * 10 + (v[i] - '0');
        if (n > (int64_t)INT32_MAX + 1) {
            // past either limit, stop growing
            n = (int64_t)INT32_MAX + 1;
            clamped = true;
        }
    }
    n = negative ? -n : n;
    if (n > INT32_MAX) {
        n = INT32_MAX;
        clamped = true;
    }
    if (clamped) {
        text_warn(text, BARTAP_WARN_ORDINAL_CLAMPED, offset);
    }
    *out = (int32_t)n;
    return true;
}

// Saturating, since signed overflow is undefined (R29, R45).
static int32_t step_ordinal(int32_t n, bool down)
{
    if (down) {
        return n == INT32_MIN ? n : n - 1;
    }
    return n == INT32_MAX ? n : n + 1;
}

// A list's or item's number, set on the frame just pushed (R45).
static void number_frame(bartap_text_t *text, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    bartap_frame_t *f = &p->frames[p->depth - 1u];
    uint8_t kind = tags[f->tag].kind;

    if (kind == BARTAP_CONTAINER_OL || kind == BARTAP_CONTAINER_UL) {
        int32_t start;
        bool has_start = read_ordinal(text, "start", sizeof "start" - 1u, token->offset, &start);
        f->flags = FRAME_LIST;
        f->ordinal = has_start ? start : 1;
        const char *v;
        uint32_t v_len;
        if (bt_xml_attr(&text->xml, "reversed", sizeof "reversed" - 1u, &v, &v_len)) {
            if (has_start) {
                f->flags |= FRAME_REVERSED;
            } else {
                // the first number would be the item count, not known until the end. count up
                text_warn(text, BARTAP_WARN_REVERSED_NO_START, token->offset);
            }
        }
        return;
    }
    if (kind != BARTAP_CONTAINER_LI) {
        return;
    }
    for (uint8_t i = (uint8_t)(p->depth - 1u); i > 0; i--) {
        bartap_frame_t *list = &p->frames[i - 1u];
        if ((list->flags & FRAME_LIST) == 0) {
            continue;
        }
        // the nearest list numbers it. value overrides and later items follow on from it
        int32_t value;
        if (read_ordinal(text, "value", sizeof "value" - 1u, token->offset, &value)) {
            list->ordinal = value;
        }
        f->ordinal = list->ordinal;
        list->ordinal = step_ordinal(list->ordinal, (list->flags & FRAME_REVERSED) != 0);
        return;
    }
    // outside any list, 0
}

// Rule 5 (R47): a recognised element past the depth limit is ignored, its close tag too.
static bool depth_full(bartap_text_t *text, uint8_t tag, uint32_t offset)
{
    bartap_parse_state_t *p = &text->parse;
    if (p->depth < BARTAP_DEPTH_MAX) {
        return false;
    }
    text_warn(text, BARTAP_WARN_DEPTH_EXCEEDED, offset);
    if (p->ignored[tag] < UINT8_MAX) {
        p->ignored[tag]++;
    }
    return true;
}

static void start_block(bartap_text_t *text, uint8_t tag, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    bool framed = takes_frame(tag);

    if (framed && depth_full(text, tag, token->offset)) {
        // its text stays in the enclosing block
        return;
    }
    if (p->block_live) {
        // rule 1 (R47): blocks are flat, one directly inside a live block pops that block's frame.
        // past a boundary or container the live block is a restart or implicit, its frame stays
        for (uint8_t i = p->depth; i > 0; i--) {
            uint8_t c = p->frames[i - 1u].class_kind;
            if (c == CLASS_CONTAINER || c == CLASS_BOUNDARY) {
                break;
            }
            if (c == CLASS_BLOCK) {
                // inline frames above it stay, so style carries into the new block
                memmove(&p->frames[i - 1u], &p->frames[i], (size_t)(p->depth - i) * sizeof p->frames[0]);
                p->depth--;
                break;
            }
        }
    }
    begin_block(text, tags[tag].kind, tags[tag].level, token->offset, EMIT_NONE);
    if (framed) {
        push_frame(p, tag, CLASS_BLOCK);
    } else {
        // hr takes no frame, so content after it opens a block of its own
        p->block_live = 0;
    }
}

static void start_container(bartap_text_t *text, uint8_t tag, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    if (depth_full(text, tag, token->offset)) {
        return;
    }
    // the start record is structural, the output block ends whatever it held
    end_block(p);
    push_frame(p, tag, CLASS_CONTAINER);
    number_frame(text, token);
    text->emit_state = EMIT_CONTAINER_START;
}

static void start_boundary(bartap_text_t *text, uint8_t tag, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    if (p->block_has_content) {
        // only a block with content ends, so <h2><div>A</div></h2> stays one h2. even when the
        // boundary itself is ignored past the depth limit
        end_block(p);
    }
    if (!depth_full(text, tag, token->offset)) {
        push_frame(p, tag, CLASS_BOUNDARY);
    }
}

// Images are content wherever they sit (R5), resolved or not, so replay needs no lookup (R49).
// Only the record depends on the lookup (R9).
static bartap_err_t start_image(bartap_text_t *text, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    const char *src = NULL;
    uint32_t src_len = 0;
    uint8_t after = EMIT_NONE;

    if (!text->replaying) {
        if (!bt_xml_attr(&text->xml, "src", sizeof "src" - 1u, &src, &src_len) &&
            !bt_xml_attr(&text->xml, "xlink:href", sizeof "xlink:href" - 1u, &src, &src_len)) {
            // SVG 2 drops the prefix
            (void)bt_xml_attr(&text->xml, "href", sizeof "href" - 1u, &src, &src_len);
        }
        bartap_err_t err = bt_image_lookup(text, src, src_len, &text->image_dir_offset);
        if (err == BARTAP_OK) {
            after = EMIT_IMAGE;
        } else if (err == BARTAP_END) {
            // no record, but still content
            text_warn(text, BARTAP_WARN_IMAGE_UNRESOLVED, token->offset);
        } else {
            // the card failed, pass error up
            return err;
        }
    }
    // a break opportunity either side, spaces around it go (R4, R6)
    p->space_pending = 0;
    p->space_allowed = 0;
    if (!p->block_live) {
        begin_content(text, token->offset, after);
    } else {
        text->emit_state = after;
    }
    p->block_has_content = 1;
    return BARTAP_OK;
}

static void start_break(bartap_text_t *text, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    // a new line starts with no space
    p->space_pending = 0;
    p->space_allowed = 0;
    if (!p->block_live) {
        // a break on its own, as books use for blank lines, gets a block to be in
        begin_content(text, token->offset, EMIT_BREAK);
    } else {
        text->emit_state = EMIT_BREAK;
    }
    p->block_has_content = 1;
}

bartap_err_t bt_text_start_tag(bartap_text_t *text, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    uint8_t tag = bt_tag_lookup(token->name, token->name_len);

    text->cr_seen = 0;
    if (p->skip_depth != 0) {
        // inside head, script and the like, only the same element nesting matters
        if (tag == p->skip_tag && !token->self_closing && p->skip_depth < UINT16_MAX) {
            p->skip_depth++;
        }
        return BARTAP_OK;
    }
    if (tag == BARTAP_TAG_COUNT) {
        // transparent, content passes through
        return BARTAP_OK;
    }
    uint8_t class_kind = tags[tag].class_kind;
    if (class_kind == CLASS_ASIDE) {
        // a footnote aside is a note container, any other a boundary
        const char *type;
        uint32_t type_len;
        bool note = bt_xml_attr(&text->xml, "epub:type", sizeof "epub:type" - 1u, &type, &type_len) &&
                    (bt_has_token(type, type_len, "footnote", sizeof "footnote" - 1u) ||
                     bt_has_token(type, type_len, "endnote", sizeof "endnote" - 1u) ||
                     bt_has_token(type, type_len, "rearnote", sizeof "rearnote" - 1u));
        class_kind = note ? CLASS_CONTAINER : CLASS_BOUNDARY;
    }

    switch (class_kind) {
    case CLASS_CONTAINER:
        start_container(text, tag, token);
        break;
    case CLASS_BLOCK:
        start_block(text, tag, token);
        break;
    case CLASS_INLINE:
        if (!depth_full(text, tag, token->offset)) {
            push_frame(p, tag, CLASS_INLINE);
            p->style |= tags[tag].kind;
        }
        break;
    case CLASS_BOUNDARY:
        start_boundary(text, tag, token);
        break;
    case CLASS_SKIP:
        if (!token->self_closing) {
            p->skip_tag = tag;
            p->skip_depth = 1;
        }
        // nothing to close
        return BARTAP_OK;
    case CLASS_IMAGE:
        // no frame, an unclosed one can't swallow what follows
        return start_image(text, token);
    default:
        start_break(text, token);
        return BARTAP_OK;
    }
    // <p/> and the like open and close at once
    return token->self_closing ? bt_text_end_tag(text, token) : BARTAP_OK;
}

bartap_err_t bt_text_end_tag(bartap_text_t *text, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    uint8_t tag = bt_tag_lookup(token->name, token->name_len);

    text->cr_seen = 0;
    if (p->skip_depth != 0) {
        if (tag == p->skip_tag) {
            p->skip_depth--;
        }
        return BARTAP_OK;
    }
    if (tag == BARTAP_TAG_COUNT) {
        // transparent, ignored silently
        return BARTAP_OK;
    }
    uint8_t class_kind = tags[tag].class_kind;
    if (!takes_frame(tag) && class_kind != CLASS_SKIP) {
        // never framed, and <img></img> is fine XHTML
        return BARTAP_OK;
    }
    if (p->ignored[tag] != 0) {
        // opened past the depth limit (R47 rule 5)
        p->ignored[tag]--;
        if ((class_kind == CLASS_BOUNDARY || class_kind == CLASS_ASIDE) && p->block_has_content) {
            // a boundary still ends the live block
            end_block(p);
        }
        return BARTAP_OK;
    }
    for (uint8_t i = p->depth; i > 0; i--) {
        if (p->frames[i - 1u].tag != tag) {
            continue;
        }
        if (i != p->depth) {
            // rule 2: closes everything above it, in order
            text_warn(text, BARTAP_WARN_CLOSE_UNWOUND, token->offset);
        }
        text->unwind_to = (uint8_t)(i - 1u);
        return BARTAP_OK;
    }
    // rule 3: matches nothing open
    text_warn(text, BARTAP_WARN_CLOSE_UNMATCHED, token->offset);
    return BARTAP_OK;
}

void bt_text_finish(bartap_text_t *text)
{
    bartap_parse_state_t *p = &text->parse;
    for (uint8_t i = 0; i < p->depth; i++) {
        uint8_t c = p->frames[i].class_kind;
        if (c == CLASS_CONTAINER || c == CLASS_BLOCK) {
            // rule 4, once
            text_warn(text, BARTAP_WARN_UNCLOSED_AT_END, text->xml.offset);
            break;
        }
    }
    p->skip_depth = 0;
    text->unwind_to = 0;
}

// The text record so far, counted for bookmarks (R10).
static bartap_err_t text_record(bartap_text_t *text, bartap_record_t *rec, uint32_t n)
{
    rec->kind = BARTAP_REC_TEXT;
    rec->text.ptr = text->text_buf;
    rec->text.len = n;
    text->parse.char_offset += n;
    return BARTAP_OK;
}

bartap_err_t bt_text_fill(bartap_text_t *text, bartap_record_t *rec)
{
    bartap_parse_state_t *p = &text->parse;
    char *buf = text->text_buf;
    uint32_t cap = text->text_cap;
    uint32_t n = 0;

    if (p->skip_depth != 0) {
        // skipped content, the tokenizer runs it out
        return BARTAP_END;
    }
    for (;;) {
        if (text->stash_pos == text->stash_len) {
            uint32_t got;
            bartap_err_t err = bt_xml_text(&text->xml, text->stash, sizeof text->stash, &got);
            if (err != BARTAP_OK) {
                // error reading text, pass error up
                return err;
            }
            if (got == 0) {
                // the token's end
                break;
            }
            text->stash_len = (uint8_t)got;
            text->stash_pos = 0;
        }
        char c = text->stash[text->stash_pos];
        bool pre = p->block_live && p->block_kind == BARTAP_BLOCK_PRE;

        if (pre && (c == '\n' || c == '\r')) {
            // a line break record, after the text before it
            if (n != 0) {
                break;
            }
            text->stash_pos++;
            if (c == '\n' && text->cr_seen) {
                // CRLF is one break
                text->cr_seen = 0;
                continue;
            }
            text->cr_seen = c == '\r';
            p->block_has_content = 1;
            p->space_allowed = 0;
            rec->kind = BARTAP_REC_LINE_BREAK;
            return BARTAP_OK;
        }
        text->cr_seen = 0;
        if (is_space(c) && !pre) {
            // collapsed, held pending until content follows (R4). only after text: none at a
            // block's start or after a break or image
            text->stash_pos++;
            if (!p->space_pending && p->block_live && p->space_allowed) {
                // the space keeps the style it was collapsed in, so <u>a</u> b isn't underlined
                p->space_pending = 1;
                p->space_style = p->style;
            }
            continue;
        }
        if (n == 0) {
            // whatever comes before this text goes first, the character waits in the stash
            if (!p->block_live) {
                begin_content(text, text->xml.token.offset, EMIT_NONE);
                (void)bt_text_emit_pending(text, rec);
                return BARTAP_OK;
            }
            // a pending space goes in its own style, before the style record that follows it (R4)
            uint32_t want = p->space_pending ? p->space_style : p->style;
            if (want != p->style_out) {
                rec->kind = BARTAP_REC_STYLE;
                rec->style.bits = want;
                p->style_out = want;
                return BARTAP_OK;
            }
            if (p->space_pending && p->style != p->style_out) {
                // the space alone, the text after it needs another style
                p->space_pending = 0;
                buf[0] = ' ';
                return text_record(text, rec, 1);
            }
        }
        uint32_t len = char_len((uint8_t)c);
        if (c == '\t') {
            // pre keeps spacing, a tab as one space
            c = ' ';
        }
        if (p->space_pending + len > cap - n) {
            if (n == 0) {
                // a 4-byte character and its space can't share a minimum buffer, the space goes alone
                p->space_pending = 0;
                buf[0] = ' ';
                n = 1;
            }
            break;
        }
        if (p->space_pending) {
            buf[n++] = ' ';
            p->space_pending = 0;
        }
        buf[n] = c;
        memcpy(buf + n + 1u, text->stash + text->stash_pos + 1u, len - 1u);
        n += len;
        text->stash_pos = (uint8_t)(text->stash_pos + len);
        p->block_has_content = 1;
        p->space_allowed = 1;
    }
    if (n == 0) {
        return BARTAP_END;
    }
    return text_record(text, rec, n);
}

bartap_err_t bt_image_lookup(bartap_text_t *text, const char *src, uint32_t src_len,
                             uint32_t *dir_offset)
{
    char path[BARTAP_PATH_MAX];
    uint16_t path_len;
    bartap_entry_t entry;

    if (src == NULL) {
        // no source attribute
        return BARTAP_END;
    }
    // relative to the chapter's own path (R22)
    bartap_err_t err = bt_href_resolve(text->base_dir, text->base_dir_len, src, src_len, path,
                                       sizeof path, &path_len);
    if (err != BARTAP_OK) {
        // data: or remote, malformed, or too long. not an entry either way
        return BARTAP_END;
    }
    err = bt_dir_find(&text->book->directory, path, path_len, &entry);
    if (err != BARTAP_OK) {
        // missing from the archive, or the card failed
        return err;
    }
    *dir_offset = entry.dir_offset;
    return BARTAP_OK;
}

/* ---- Text streams (R11, R17, R20) ---- */

bartap_err_t bartap_text_open(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
                              uint8_t *window, char *text_buf, uint32_t text_cap,
                              bartap_rolling_t *rolling, bartap_warn_fn warn_fn, void *warn_user)
{
    (void)text;
    (void)book;
    (void)spine_index;
    (void)window;
    (void)text_buf;
    (void)text_cap;
    (void)rolling;
    (void)warn_fn;
    (void)warn_user;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_open_memory(bartap_text_t *text, const bartap_book_t *book,
                                     uint16_t spine_index, const uint8_t *data, uint32_t size,
                                     char *text_buf, uint32_t text_cap, bartap_rolling_t *rolling,
                                     bartap_warn_fn warn_fn, void *warn_user)
{
    (void)text;
    (void)book;
    (void)spine_index;
    (void)data;
    (void)size;
    (void)text_buf;
    (void)text_cap;
    (void)rolling;
    (void)warn_fn;
    (void)warn_user;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_text_next(bartap_text_t *text, bartap_record_t *rec)
{
    (void)text;
    (void)rec;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_text_warnings(const bartap_text_t *text)
{
    (void)text;
    return 0;
}

/* ---- Seek (R12, R48, R49) ---- */

bartap_err_t bartap_text_seek(bartap_text_t *text, const bartap_bookmark_t *bookmark)
{
    (void)text;
    (void)bookmark;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bt_text_replay(bartap_text_t *text, uint32_t block_offset, uint32_t char_offset)
{
    (void)text;
    (void)block_offset;
    (void)char_offset;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Checkpoints (R48). The ring they index lives in xml.c ---- */

void bt_checkpoint_take(bartap_text_t *text, uint32_t block_offset, uint32_t resume_offset)
{
    (void)text;
    (void)block_offset;
    (void)resume_offset;
}

bartap_err_t bt_checkpoint_restore(bartap_text_t *text, const bartap_checkpoint_t *checkpoint)
{
    (void)text;
    (void)checkpoint;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- Bookmarks (R12, R19) ---- */

bartap_err_t bartap_bookmark_write(const bartap_book_t *book, uint16_t spine_index,
                                   uint32_t block_offset, uint32_t char_offset, uint8_t *out)
{
    (void)book;
    (void)spine_index;
    (void)block_offset;
    (void)char_offset;
    (void)out;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_bookmark_read(const bartap_book_t *book, const uint8_t *in,
                                  bartap_bookmark_t *out)
{
    (void)book;
    (void)in;
    (void)out;
    return BARTAP_ERR_UNSUPPORTED;
}

/* ---- TOC (R13, R26) ---- */

bartap_err_t bartap_toc_open(bartap_toc_t *toc, const bartap_book_t *book, uint8_t *window,
                             char *text_buf, uint32_t text_cap, bartap_warn_fn warn_fn,
                             void *warn_user)
{
    (void)toc;
    (void)book;
    (void)window;
    (void)text_buf;
    (void)text_cap;
    (void)warn_fn;
    (void)warn_user;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_toc_next(bartap_toc_t *toc, bartap_record_t *rec)
{
    (void)toc;
    (void)rec;
    return BARTAP_ERR_UNSUPPORTED;
}

bartap_err_t bartap_toc_resolve(bartap_text_t *text, const char *id, uint32_t id_len,
                                bartap_bookmark_t *out)
{
    (void)text;
    (void)id;
    (void)id_len;
    (void)out;
    return BARTAP_ERR_UNSUPPORTED;
}

uint32_t bartap_toc_warnings(const bartap_toc_t *toc)
{
    (void)toc;
    return 0;
}
