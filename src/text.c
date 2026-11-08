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

enum {
    EMIT_NONE = 0,
    EMIT_CONTEXT, /* container starts for every open container, after a seek (R49) */
    EMIT_CONTAINER_START,
    EMIT_BLOCK_START,
    EMIT_STYLE,
    EMIT_IMAGE,
    EMIT_BREAK
};

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
    // the state before the triggering token, which a restore processes again (R48)
    bt_checkpoint_take(text, offset, offset);
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

static void container_start(const bartap_frame_t *f, bartap_record_t *rec)
{
    rec->kind = BARTAP_REC_CONTAINER_START;
    rec->container.kind = (bartap_container_kind_t)container_kind(f);
    // items keep their own number, so a seek into a list restates it (R45)
    rec->container.ordinal = tags[f->tag].kind == BARTAP_CONTAINER_LI ? f->ordinal : 0;
}

bool bt_text_emit_pending(bartap_text_t *text, bartap_record_t *rec)
{
    bartap_parse_state_t *p = &text->parse;

    // starts first, so a self-closing tag's start comes out before its end
    switch (text->emit_state) {
    case EMIT_CONTEXT:
        // after a seek, every container open at the target, outermost first (R49)
        while (text->emit_index < p->depth) {
            const bartap_frame_t *f = &p->frames[text->emit_index++];
            if (f->class_kind == CLASS_CONTAINER) {
                container_start(f, rec);
                return true;
            }
        }
        // then the block, its style and whatever the landing token queued
        text->emit_state = EMIT_BLOCK_START;
        return bt_text_emit_pending(text, rec);
    case EMIT_CONTAINER_START:
        // the container is the top frame, nothing has been pushed since
        container_start(&p->frames[p->depth - 1u], rec);
        text->emit_state = EMIT_NONE;
        return true;
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
    if (!(framed && token->self_closing)) {
        // just past the tag (R48). not <p/>, whose close a restore past the tag would miss
        bt_checkpoint_take(text, token->offset, token->end_offset);
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

// Looks the current image tag's source up: EMIT_IMAGE with its offset kept, or EMIT_NONE with a
// warning if it can't be resolved (R9). The tag must still be the tokenizer's current token.
static bartap_err_t resolve_image(bartap_text_t *text, uint8_t *after)
{
    const char *src = NULL;
    uint32_t src_len = 0;

    if (!bt_xml_attr(&text->xml, "src", sizeof "src" - 1u, &src, &src_len) &&
        !bt_xml_attr(&text->xml, "xlink:href", sizeof "xlink:href" - 1u, &src, &src_len)) {
        // SVG 2 drops the prefix
        (void)bt_xml_attr(&text->xml, "href", sizeof "href" - 1u, &src, &src_len);
    }
    bartap_err_t err = bt_image_lookup(text, src, src_len, &text->image_dir_offset);
    if (err == BARTAP_OK) {
        *after = EMIT_IMAGE;
        return BARTAP_OK;
    }
    *after = EMIT_NONE;
    if (err == BARTAP_END) {
        // no record, but still content
        text_warn(text, BARTAP_WARN_IMAGE_UNRESOLVED, text->xml.token.offset);
        return BARTAP_OK;
    }
    // the card failed, pass error up
    return err;
}

// Images are content wherever they sit (R5), resolved or not, so replay needs no lookup (R49).
// Only the record depends on the lookup (R9).
static bartap_err_t start_image(bartap_text_t *text, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    uint8_t after = EMIT_IMAGE;

    if (!text->replaying) {
        bartap_err_t err = resolve_image(text, &after);
        if (err != BARTAP_OK) {
            return err;
        }
    }
    // replaying, it stays queued unresolved: a landing on the block it opens looks it up then
    if (!p->block_live) {
        begin_content(text, token->offset, after);
    } else {
        text->emit_state = after;
    }
    // a break opportunity either side, spaces around it go (R4, R6)
    p->space_pending = 0;
    p->space_allowed = 0;
    p->block_has_content = 1;
    return BARTAP_OK;
}

static void start_break(bartap_text_t *text, const bartap_xml_token_t *token)
{
    bartap_parse_state_t *p = &text->parse;
    if (!p->block_live) {
        // a break on its own, as books use for blank lines, gets a block to be in
        begin_content(text, token->offset, EMIT_BREAK);
    } else {
        text->emit_state = EMIT_BREAK;
    }
    // a new line starts with no space
    p->space_pending = 0;
    p->space_allowed = 0;
    p->block_has_content = 1;
}

bool bt_tag_is_inline(uint8_t tag, uint32_t *style_bit)
{
    if (tag >= BARTAP_TAG_COUNT || tags[tag].class_kind != CLASS_INLINE) {
        *style_bit = 0;
        return false;
    }
    *style_bit = tags[tag].kind;
    return true;
}

static bartap_err_t start_tag(bartap_text_t *text, const bartap_xml_token_t *token)
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

bartap_err_t bt_text_start_tag(bartap_text_t *text, const bartap_xml_token_t *token)
{
    // skipped content first: an id inside head isn't a place in the text
    bool skipping = text->parse.skip_depth != 0;
    bartap_err_t err = start_tag(text, token);
    if (err != BARTAP_OK || text->find_id == NULL || skipping) {
        return err;
    }
    // any element's id counts, transparent ones too (R13, R46). checked after the tag is handled,
    // so an id on a block start finds that block
    const char *id;
    uint32_t id_len;
    if (bt_xml_attr(&text->xml, "id", sizeof "id" - 1u, &id, &id_len) &&
        id_len == text->find_id_len && memcmp(id, text->find_id, id_len) == 0) {
        text->id_found = 1;
    }
    return BARTAP_OK;
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
    // a replay copies nothing (R49), it only counts
    char *buf = text->replaying ? NULL : text->text_buf;
    uint32_t cap = text->text_cap;
    uint32_t n = 0;

    if (text->landing) {
        // in the target block, stop exactly at its character offset. never 0, it lands there
        cap = text->target_char - p->char_offset;
    }
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
                if (buf != NULL) {
                    buf[0] = ' ';
                }
                return text_record(text, rec, 1);
            }
        }
        uint32_t len = char_len((uint8_t)c);
        if (c == '\t') {
            // pre keeps spacing, a tab as one space
            c = ' ';
        }
        if (p->space_pending + len > cap - n) {
            if (n == 0 && !p->space_pending) {
                // only a landing caps below one character: it falls inside this one (R12)
                return BARTAP_ERR_ARG;
            }
            if (n == 0) {
                // a 4-byte character and its space can't share a minimum buffer, the space goes alone
                p->space_pending = 0;
                if (buf != NULL) {
                    buf[0] = ' ';
                }
                n = 1;
            }
            break;
        }
        if (p->space_pending) {
            if (buf != NULL) {
                buf[n] = ' ';
            }
            n++;
            p->space_pending = 0;
        }
        if (buf != NULL) {
            buf[n] = c;
            memcpy(buf + n + 1u, text->stash + text->stash_pos + 1u, len - 1u);
        }
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

// What both modes share: argument checks, the item's entry and path, and a fresh parse. The byte
// source is the caller's to set up after. A failure after the reset is kept, so next repeats it.
static bartap_err_t text_init(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
                              char *text_buf, uint32_t text_cap, bartap_warn_fn warn_fn,
                              void *warn_user, bartap_entry_t *entry)
{
    if (text == NULL) {
        // nowhere to keep the failure
        return BARTAP_ERR_ARG;
    }
    memset(text, 0, sizeof *text);
    text->book = book;
    text->spine_index = spine_index;
    text->text_buf = text_buf;
    text->text_cap = text_cap;
    text->unwind_to = UNWIND_NONE;
    text->warnings.fn = warn_fn;
    text->warnings.user = warn_user;

    if (book == NULL || text_buf == NULL) {
        text->status = BARTAP_ERR_ARG;
        return text->status;
    }
    if (text_cap < BARTAP_TEXT_MIN) {
        // can't hold one whole character (R50)
        text->status = BARTAP_ERR_BUFFER_TOO_SMALL;
        return text->status;
    }
    bartap_err_t err = bt_spine_entry(book, spine_index, entry);
    if (err != BARTAP_OK) {
        // past the spine, or an item with no entry
        text->status = err;
        return err;
    }
    // kept for bookmarks, so a compact item is looked up once, here (R41)
    text->chapter_crc32 = entry->crc32;
    // the item's own path, for resolving image paths (R22). it matched a resolved href at open, so it
    // fits: anything else here is the card failing
    uint16_t len;
    err = bt_dir_name(&book->directory, entry->dir_offset, text->base_dir, BARTAP_PATH_MAX, &len);
    if (err != BARTAP_OK) {
        text->status = err == BARTAP_ERR_BUFFER_TOO_SMALL ? BARTAP_ERR_CORRUPT : err;
        return text->status;
    }
    text->base_dir_len = len;
    return BARTAP_OK;
}

bartap_err_t bartap_text_open(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
                              uint8_t *window, char *text_buf, uint32_t text_cap,
                              bartap_rolling_t *rolling, bartap_warn_fn warn_fn, void *warn_user)
{
    bartap_entry_t entry;

    bartap_err_t err = text_init(text, book, spine_index, text_buf, text_cap, warn_fn, warn_user,
                                 &entry);
    if (err != BARTAP_OK) {
        return err;
    }
    if (window == NULL || (rolling != NULL && rolling->ring == NULL)) {
        // stream mode needs a window, and a ring that holds bytes: a virtual one is memory mode's
        text->status = BARTAP_ERR_ARG;
        return text->status;
    }
    err = bt_stream_open(&text->stream, book->source, &entry, window);
    if (err != BARTAP_OK) {
        // bad local header or method, pass error up
        text->status = err;
        return err;
    }
    text->rolling = rolling;
    bt_bytes_init_stream(&text->bytes, &text->stream, rolling);
    bt_xml_init(&text->xml, &text->bytes, &text->warnings, spine_index);
    return BARTAP_OK;
}

bartap_err_t bartap_text_open_memory(bartap_text_t *text, const bartap_book_t *book,
                                     uint16_t spine_index, const uint8_t *data, uint32_t size,
                                     char *text_buf, uint32_t text_cap, bartap_rolling_t *rolling,
                                     bartap_warn_fn warn_fn, void *warn_user)
{
    bartap_entry_t entry;

    bartap_err_t err = text_init(text, book, spine_index, text_buf, text_cap, warn_fn, warn_user,
                                 &entry);
    if (err != BARTAP_OK) {
        return err;
    }
    if (data == NULL && size != 0) {
        text->status = BARTAP_ERR_ARG;
        return text->status;
    }
    // the bytes are the caller's, already inflated and checked or not (R11, R31)
    text->rolling = rolling;
    bt_bytes_init_memory(&text->bytes, data, size, rolling);
    bt_xml_init(&text->xml, &text->bytes, &text->warnings, spine_index);
    return BARTAP_OK;
}

// One token handed on. Text is only marked here, it's read by bt_text_fill.
static bartap_err_t text_token(bartap_text_t *text, const bartap_xml_token_t *token)
{
    switch (token->kind) {
    case BARTAP_XML_TEXT:
        text->in_text = 1;
        text->stash_len = 0;
        text->stash_pos = 0;
        return BARTAP_OK;
    case BARTAP_XML_START_TAG:
        return bt_text_start_tag(text, token);
    default:
        return bt_text_end_tag(text, token);
    }
}

// The next record, live or replayed. Failures aren't kept here, the callers decide.
static bartap_err_t text_step(bartap_text_t *text, bartap_record_t *rec)
{
    for (;;) {
        // what earlier tokens queued comes out before the next token is looked at (R20)
        if (bt_text_emit_pending(text, rec)) {
            return BARTAP_OK;
        }
        if (text->in_text) {
            bartap_err_t err = bt_text_fill(text, rec);
            if (err == BARTAP_OK) {
                return BARTAP_OK;
            }
            if (err != BARTAP_END) {
                // error reading text, pass error up
                return err;
            }
            // the token's done
            text->in_text = 0;
        }
        if (text->finished) {
            // closing records are out, the item is done. END again on every call after
            return BARTAP_END;
        }
        const bartap_xml_token_t *token;
        bartap_err_t err = bt_xml_next(&text->xml, &token);
        if (err == BARTAP_END) {
            // the source ended and checked out (R31). close what's open, then END (R47 rule 4)
            bt_text_finish(text);
            text->finished = 1;
            continue;
        }
        if (err == BARTAP_OK) {
            err = text_token(text, token);
        }
        if (err != BARTAP_OK) {
            // tokenizer or image lookup failed, pass error up
            return err;
        }
        if (text->id_found == 1) {
            // stop right at the element bartap_toc_resolve looks for, before anything moves on
            text->id_found = 2;
            return BARTAP_MORE;
        }
    }
}

bartap_err_t bartap_text_next(bartap_text_t *text, bartap_record_t *rec)
{
    if (text == NULL || rec == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (text->status != BARTAP_OK) {
        // failed before, open included. the same failure again
        return text->status;
    }
    bartap_err_t err = text_step(text, rec);
    if (err == BARTAP_OK || err == BARTAP_END) {
        // delivered up to here, a replay stays quiet below it (R49)
        if (text->xml.offset > text->high_water) {
            text->high_water = text->xml.offset;
        }
        return err;
    }
    // keep it
    text->status = err;
    return err;
}

uint32_t bartap_text_warnings(const bartap_text_t *text)
{
    return text != NULL ? text->warnings.bits : 0u;
}

/* ---- Seek (R12, R48, R49) ---- */

// Clears everything between tokens: queued records, a text token in progress, a failure.
static void reset_flow(bartap_text_t *text)
{
    text->emit_state = EMIT_NONE;
    text->emit_next = EMIT_NONE;
    text->emit_index = 0;
    text->unwind_to = UNWIND_NONE;
    text->in_text = 0;
    text->finished = 0;
    text->stash_len = 0;
    text->stash_pos = 0;
    text->cr_seen = 0;
    text->status = BARTAP_OK;
}

// Back to the item's start with a fresh parse, as if just opened.
static bartap_err_t restart(bartap_text_t *text)
{
    bartap_err_t err = bt_xml_seek(&text->xml, 0);
    if (err != BARTAP_OK) {
        // couldn't rewind the stream, pass error up
        return err;
    }
    memset(&text->parse, 0, sizeof text->parse);
    reset_flow(text);
    return BARTAP_OK;
}

// Whether a checkpoint's bytes can be read again without the card (R48).
static bool checkpoint_held(const bartap_text_t *text, const bartap_checkpoint_t *checkpoint)
{
    if (text->bytes.stream == NULL) {
        // memory mode, every byte is in reach
        return true;
    }
    return bt_ring_read(text->rolling, checkpoint->resume_offset, NULL, 0) == BARTAP_OK;
}

// Where a replay to block_offset starts: the live position if the block hasn't started yet, the
// latest usable checkpoint at or before it, or the item's start (R48).
static bartap_err_t replay_start(bartap_text_t *text, uint32_t block_offset)
{
    if (text->status == BARTAP_OK && !text->finished && block_offset > text->parse.block_offset) {
        // still ahead, carry on from here. blocks start in offset order
        return BARTAP_OK;
    }
    if (text->rolling != NULL) {
        // newest first
        for (uint32_t i = 2; i > 0; i--) {
            const bartap_checkpoint_t *checkpoint = &text->rolling->checkpoints[i - 1u];
            if (checkpoint->valid && checkpoint->block_offset <= block_offset &&
                checkpoint_held(text, checkpoint)) {
                return bt_checkpoint_restore(text, checkpoint);
            }
        }
    }
    // before both checkpoints, or none usable. replay from the start
    return restart(text);
}

// Replay's quiet setting: records discarded, the warning callback silent below what has already
// been delivered (R49).
static void replay_begin(bartap_text_t *text)
{
    text->replaying = 1;
    text->warnings.quiet_below = text->high_water;
}

static void replay_end(bartap_text_t *text)
{
    text->replaying = 0;
    text->landing = 0;
    text->warnings.quiet_below = 0;
}

bartap_err_t bt_text_replay(bartap_text_t *text, uint32_t block_offset, uint32_t char_offset)
{
    bartap_record_t rec;
    bool in_block = false;

    bartap_err_t err = replay_start(text, block_offset);
    if (err != BARTAP_OK) {
        // restore or rewind failed, pass error up
        return err;
    }
    replay_begin(text);
    for (;;) {
        err = text_step(text, &rec);
        if (err != BARTAP_OK) {
            if (err == BARTAP_ERR_ARG && text->landing) {
                // the offset falls inside a character (R12)
                err = BARTAP_END;
            }
            // the item ended first, or a failure
            break;
        }
        if (rec.kind == BARTAP_REC_BLOCK_START) {
            if (in_block || rec.block.block_offset > block_offset) {
                // the block ended before its character offset, or no block starts there
                err = BARTAP_END;
                break;
            }
            if (rec.block.block_offset == block_offset) {
                in_block = true;
                if (char_offset == 0) {
                    // at the block start, before anything in it
                    break;
                }
                // its text stops exactly at the character offset from here on
                text->landing = 1;
                text->target_char = char_offset;
            }
        } else if (in_block) {
            if (rec.kind == BARTAP_REC_CONTAINER_START || rec.kind == BARTAP_REC_CONTAINER_END) {
                // the block ended before its character offset
                err = BARTAP_END;
                break;
            }
            if (rec.kind == BARTAP_REC_TEXT && text->parse.char_offset == char_offset) {
                // right after that many text bytes (R10). what's at this point comes out live
                break;
            }
        }
    }
    if (err == BARTAP_OK && text->emit_next == EMIT_IMAGE) {
        // landed on a block an image opened. the replay skipped its lookup, the tag is still current
        err = resolve_image(text, &text->emit_next);
    }
    replay_end(text);
    if (err != BARTAP_OK) {
        return err;
    }
    // restate the context: containers, then the block with its character offset, then style (R49)
    text->emit_state = EMIT_CONTEXT;
    text->emit_index = 0;
    return BARTAP_OK;
}

// R12 check 3's first pass: the last block start at or before offset. BARTAP_END if there's none.
static bartap_err_t find_block(bartap_text_t *text, uint32_t offset, uint32_t *found)
{
    bartap_record_t rec;
    bool have = false;

    bartap_err_t err = replay_start(text, offset);
    if (err != BARTAP_OK) {
        return err;
    }
    replay_begin(text);
    for (;;) {
        err = text_step(text, &rec);
        if (err != BARTAP_OK) {
            // the item's end or a failure
            break;
        }
        if (rec.kind == BARTAP_REC_BLOCK_START) {
            if (rec.block.block_offset > offset) {
                // past it, the last one seen is the one
                break;
            }
            *found = rec.block.block_offset;
            have = true;
        }
    }
    replay_end(text);
    if (err != BARTAP_OK && err != BARTAP_END) {
        return err;
    }
    return have ? BARTAP_OK : BARTAP_END;
}

bartap_err_t bartap_text_seek(bartap_text_t *text, const bartap_bookmark_t *bookmark)
{
    bartap_err_t err;

    if (text == NULL || bookmark == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (text->xml.bytes == NULL) {
        // never opened, the open's failure again
        return text->status != BARTAP_OK ? text->status : BARTAP_ERR_ARG;
    }
    if (bookmark->spine_index != text->spine_index) {
        // a bookmark for another item, open that one instead
        return BARTAP_ERR_ARG;
    }
    if (bookmark->chapter_crc32 != text->chapter_crc32) {
        // check 2: the item changed, its start
        err = BARTAP_END;
    } else if (bookmark->version != BARTAP_BOOKMARK_VERSION) {
        // check 3: character offsets mean something else now, but the block offset is still a
        // raw position. the start of the block holding it
        uint32_t block_offset = 0;
        err = find_block(text, bookmark->block_offset, &block_offset);
        if (err == BARTAP_OK) {
            err = bt_text_replay(text, block_offset, 0);
        }
    } else {
        // check 4: exact
        err = bt_text_replay(text, bookmark->block_offset, bookmark->char_offset);
    }
    if (err == BARTAP_END) {
        // not a landing, the item's start (R12, R29)
        err = restart(text);
    }
    if (err != BARTAP_OK) {
        // the source failed, keep it
        text->status = err;
    }
    return err;
}

/* ---- Checkpoints (R48). The ring they index lives in xml.c ---- */

void bt_checkpoint_take(bartap_text_t *text, uint32_t block_offset, uint32_t resume_offset)
{
    bartap_rolling_t *rolling = text->rolling;

    if (rolling == NULL || block_offset < rolling->next_checkpoint) {
        // no ring, or not due yet: one per half ring
        return;
    }
    // the older goes, the newer becomes the older
    rolling->checkpoints[0] = rolling->checkpoints[1];
    bartap_checkpoint_t *checkpoint = &rolling->checkpoints[1];
    checkpoint->parse = text->parse;
    checkpoint->block_offset = block_offset;
    checkpoint->resume_offset = resume_offset;
    checkpoint->valid = 1;
    uint32_t half = rolling->size / 2u;
    rolling->next_checkpoint = block_offset > UINT32_MAX - half ? UINT32_MAX : block_offset + half;
}

bartap_err_t bt_checkpoint_restore(bartap_text_t *text, const bartap_checkpoint_t *checkpoint)
{
    // only ever a held checkpoint, so this seek never rewinds and the snapshot stays good
    bartap_err_t err = bt_xml_seek(&text->xml, checkpoint->resume_offset);
    if (err != BARTAP_OK) {
        // the source didn't move, pass error up
        return err;
    }
    text->parse = checkpoint->parse;
    reset_flow(text);
    if (checkpoint->block_offset != checkpoint->resume_offset) {
        // past an explicit block's tag, its start record is still owed. a block with no tag
        // resumes before the token that opens it, which opens it again (R48 invariant 2)
        text->emit_state = EMIT_BLOCK_START;
    }
    return BARTAP_OK;
}

/* ---- Bookmarks (R12, R19) ---- */

// Field offsets, frozen: a bookmark of any version can be read (R12).
#define BM_VERSION 0u
#define BM_BOOK_HASH 1u
#define BM_SPINE 5u
#define BM_BLOCK 7u
#define BM_CHAR 11u
#define BM_CHAPTER_CRC 15u
#define BM_CRC 19u

_Static_assert(BM_CRC + 4u == BARTAP_BOOKMARK_SIZE, "the bookmark CRC-32 ends the bookmark");

bartap_err_t bartap_bookmark_write(const bartap_book_t *book, uint16_t spine_index,
                                   uint32_t block_offset, uint32_t char_offset, uint8_t *out)
{
    bartap_entry_t entry;

    if (book == NULL || out == NULL) {
        return BARTAP_ERR_ARG;
    }
    // from the directory, so a changed chapter is caught without reading it (R12 check 2). past
    // the spine is ARG, an item with no entry CORRUPT
    bartap_err_t err = bt_spine_entry(book, spine_index, &entry);
    if (err != BARTAP_OK) {
        return err;
    }
    // byte by byte, little-endian, never a struct dump (R19)
    out[BM_VERSION] = (uint8_t)BARTAP_BOOKMARK_VERSION;
    bt_put_le32(out + BM_BOOK_HASH, book->book_hash);
    bt_put_le16(out + BM_SPINE, spine_index);
    bt_put_le32(out + BM_BLOCK, block_offset);
    bt_put_le32(out + BM_CHAR, char_offset);
    bt_put_le32(out + BM_CHAPTER_CRC, entry.crc32);
    bt_put_le32(out + BM_CRC, bt_crc32(0, out, BM_CRC));
    return BARTAP_OK;
}

bartap_err_t bartap_bookmark_read(const bartap_book_t *book, const uint8_t *in,
                                  bartap_bookmark_t *out)
{
    if (book == NULL || in == NULL || out == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (bt_get_le32(in + BM_CRC) != bt_crc32(0, in, BM_CRC)) {
        // check 0: no field can be trusted, not even the spine index
        return BARTAP_ERR_CORRUPT;
    }
    out->version = in[BM_VERSION];
    out->book_hash = bt_get_le32(in + BM_BOOK_HASH);
    out->spine_index = bt_get_le16(in + BM_SPINE);
    out->block_offset = bt_get_le32(in + BM_BLOCK);
    out->char_offset = bt_get_le32(in + BM_CHAR);
    out->chapter_crc32 = bt_get_le32(in + BM_CHAPTER_CRC);
    if (out->book_hash != book->book_hash) {
        // check 1: another book's. the caller decides what to open
        return BARTAP_ERR_WRONG_BOOK;
    }
    if (out->spine_index >= book->spine_count) {
        // no chapter to land in, spine truncation included (R41)
        return BARTAP_ERR_CORRUPT;
    }
    // checks 2 to 4 are bartap_text_seek's, once the item is open
    return BARTAP_OK;
}

/* ---- TOC (R13, R26) ---- */

enum { TOC_SEARCH = 0, TOC_LIST, TOC_LABEL, TOC_DONE };
enum { TOC_EMIT_NONE = 0, TOC_EMIT_STYLE, TOC_EMIT_REREAD };

#define TOC_NO_LABEL 0xFFFFFFFFu
#define TOC_DEPTH_MAX 0xFFFFu

// Compares a folded tag name with a lower-case literal.
static bool name_is(const bartap_xml_token_t *token, const char *name, uint32_t len)
{
    return token->name_len == len && memcmp(token->name, name, len) == 0;
}

#define NAME_IS(token, name) name_is(token, name, sizeof name - 1u)

static void toc_warn(bartap_toc_t *toc, bartap_warning_t code, uint32_t offset)
{
    bt_warn(&toc->warnings, code, BARTAP_NO_SPINE, offset);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Percent-decodes a fragment where it lies, in the tokenizer's attribute storage, so it matches the
// id it names (R26 keeps it there, needing no memory of its own). Returns the decoded length.
static uint32_t decode_fragment(char *p, uint32_t len)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < len; i++) {
        int hi;
        int lo;
        if (p[i] == '%' && len - i > 2u && (hi = hex_value(p[i + 1u])) >= 0 &&
            (lo = hex_value(p[i + 2u])) >= 0) {
            p[out++] = (char)(hi * 16 + lo);
            i += 2u;
        } else {
            // not an escape, kept as it is
            p[out++] = p[i];
        }
    }
    return out;
}

// Fills a TOC-entry record for an href: spine index of its document, and its fragment. An href that
// leads nowhere in the spine still gives an entry, with BARTAP_NO_SPINE and a warning (R26).
static void toc_entry(bartap_toc_t *toc, const char *href, uint32_t href_len, uint32_t offset,
                      bartap_record_t *rec)
{
    const bartap_book_t *book = toc->book;
    char path[BARTAP_PATH_MAX];
    uint16_t path_len;

    rec->kind = BARTAP_REC_TOC_ENTRY;
    rec->toc.depth = (uint16_t)(toc->depth > 0 ? toc->depth - 1u : 0u);
    rec->toc.spine_index = BARTAP_NO_SPINE;
    rec->toc.fragment = NULL;
    rec->toc.fragment_len = 0;
    if (href == NULL) {
        // a heading with no link, an entry with no target and nothing wrong
        return;
    }
    for (uint32_t i = 0; i < href_len; i++) {
        if (href[i] == '#') {
            // the fragment, decoded in the storage the tokenizer gave us. ours, it's toc->xml's
            char *fragment = toc->xml.attr_storage + (href + i + 1u - toc->xml.attr_storage);
            uint32_t fragment_len = decode_fragment(fragment, href_len - i - 1u);
            if (fragment_len != 0) {
                rec->toc.fragment = fragment;
                rec->toc.fragment_len = fragment_len;
            }
            break;
        }
    }
    // the resolver drops the fragment itself, and refuses remote and data: hrefs
    if (bt_href_resolve(toc->base_dir, toc->base_dir_len, href, href_len, path, sizeof path,
                        &path_len) == BARTAP_OK) {
        uint32_t hash = bt_name_hash(BT_NAME_HASH_INIT, path, path_len);
        for (uint16_t i = 0; i < book->spine_count; i++) {
            // first in spine order, the way the reader meets it
            uint32_t item_hash;
            uint16_t item_len;
            bt_spine_path(book, i, &item_hash, &item_len);
            if (item_hash == hash && item_len == path_len) {
                rec->toc.spine_index = i;
                return;
            }
        }
    }
    // not in the spine, or not a path at all
    rec->toc.fragment = NULL;
    rec->toc.fragment_len = 0;
    toc_warn(toc, BARTAP_WARN_TOC_TARGET, offset);
}

// A label's elements change style as they would in text (R26), counted per bit since a label has no
// frames to restore from.
static void toc_style(bartap_toc_t *toc, const bartap_xml_token_t *token, bool open)
{
    uint32_t bit;
    if (!bt_tag_is_inline(bt_tag_lookup(token->name, token->name_len), &bit) || token->self_closing) {
        return;
    }
    uint32_t index = 0;
    while ((bit >> (index + 1u)) != 0) {
        index++;
    }
    if (open && toc->style_count[index] < UINT8_MAX) {
        toc->style_count[index]++;
    } else if (!open && toc->style_count[index] != 0) {
        toc->style_count[index]--;
    }
    toc->style = 0;
    for (uint32_t i = 0; i < sizeof toc->style_count; i++) {
        if (toc->style_count[i] != 0) {
            toc->style |= 1u << i;
        }
    }
}

// The label starts: style restated before its text, which collapses as text does (R4, R26).
static void begin_label(bartap_toc_t *toc, uint8_t label_tag)
{
    toc->mode = TOC_LABEL;
    toc->label_tag = label_tag;
    toc->label_nest = 1;
    toc->space_pending = 0;
    toc->space_allowed = 0;
    memset(toc->style_count, 0, sizeof toc->style_count);
    toc->style = 0;
    toc->emit_state = TOC_EMIT_STYLE;
}

static void end_label(bartap_toc_t *toc)
{
    // a space at a label's end goes, as at a block's end
    toc->mode = TOC_LIST;
    toc->space_pending = 0;
}

enum { LABEL_A = 1, LABEL_SPAN, LABEL_NAVLABEL };

// nav: lists of li, each with an a (or a span heading) for its label, then perhaps a nested list.
// true with a record if the tag starts an entry.
static bool nav_start(bartap_toc_t *toc, const bartap_xml_token_t *token, bartap_record_t *rec)
{
    if (NAME_IS(token, "nav") && !token->self_closing && toc->nav_nest < UINT16_MAX) {
        // a nav inside the toc nav, counted so its close doesn't end ours
        toc->nav_nest++;
        return false;
    }
    if (toc->mode == TOC_LABEL) {
        if ((toc->label_tag == LABEL_A && NAME_IS(token, "a")) ||
            (toc->label_tag == LABEL_SPAN && NAME_IS(token, "span"))) {
            if (!token->self_closing && toc->label_nest < UINT16_MAX) {
                toc->label_nest++;
            }
            return false;
        }
        if (!NAME_IS(token, "ol") && !NAME_IS(token, "li")) {
            // markup inside the label
            if (NAME_IS(token, "br")) {
                toc->space_pending = toc->space_allowed;
            }
            toc_style(toc, token, true);
            return false;
        }
        // a list inside an unclosed label, the label ends here
        end_label(toc);
    }
    if (NAME_IS(token, "ol")) {
        if (!token->self_closing && toc->depth < TOC_DEPTH_MAX) {
            toc->depth++;
        }
        return false;
    }
    if (NAME_IS(token, "li")) {
        toc->has_entry = 0;
        return false;
    }
    bool is_a = NAME_IS(token, "a");
    if ((!is_a && !NAME_IS(token, "span")) || toc->has_entry || toc->depth == 0) {
        // outside an item's label, nothing to return
        return false;
    }
    // the item's first a or span is its entry
    const char *href = NULL;
    uint32_t href_len = 0;
    if (is_a) {
        (void)bt_xml_attr(&toc->xml, "href", sizeof "href" - 1u, &href, &href_len);
    }
    toc_entry(toc, href, href_len, token->offset, rec);
    toc->has_entry = 1;
    if (!token->self_closing) {
        begin_label(toc, is_a ? LABEL_A : LABEL_SPAN);
    }
    return true;
}

static void nav_end(bartap_toc_t *toc, const bartap_xml_token_t *token)
{
    if (NAME_IS(token, "nav")) {
        if (--toc->nav_nest == 0) {
            // the toc nav is over, the TOC with it
            toc->mode = TOC_DONE;
        }
        return;
    }
    if (toc->mode == TOC_LABEL) {
        if ((toc->label_tag == LABEL_A && NAME_IS(token, "a")) ||
            (toc->label_tag == LABEL_SPAN && NAME_IS(token, "span"))) {
            if (--toc->label_nest == 0) {
                end_label(toc);
            }
            return;
        }
        if (!NAME_IS(token, "ol") && !NAME_IS(token, "li")) {
            toc_style(toc, token, false);
            return;
        }
        // the item or list closed with the label still open
        end_label(toc);
    }
    if (NAME_IS(token, "ol") && toc->depth > 0) {
        toc->depth--;
    }
}

// NCX: navPoints nest; each has a navLabel, normally before its content (R26).
static bool ncx_start(bartap_toc_t *toc, const bartap_xml_token_t *token, bartap_record_t *rec)
{
    if (toc->mode == TOC_LABEL) {
        // labels hold text, and sometimes an img, nothing that matters here
        return false;
    }
    if (NAME_IS(token, "navpoint")) {
        if (toc->depth > 0 && !toc->has_entry) {
            // the enclosing navPoint never had a content, so it has no entry
            toc_warn(toc, BARTAP_WARN_TOC_TARGET, token->offset);
        }
        if (!token->self_closing && toc->depth < TOC_DEPTH_MAX) {
            toc->depth++;
        }
        toc->has_entry = 0;
        toc->label_offset = TOC_NO_LABEL;
        return false;
    }
    if (toc->depth == 0) {
        return false;
    }
    if (NAME_IS(token, "navlabel") && !token->self_closing) {
        if (toc->has_entry) {
            // its content came first, or this is the re-read: the label now
            begin_label(toc, LABEL_NAVLABEL);
        } else if (toc->label_offset == TOC_NO_LABEL) {
            // before its content, the usual order. come back to it once the entry is out
            toc->label_offset = token->offset;
        }
        return false;
    }
    if (!NAME_IS(token, "content") || toc->has_entry) {
        // a content already returned is the re-read passing it again
        return false;
    }
    const char *src = NULL;
    uint32_t src_len = 0;
    if (!bt_xml_attr(&toc->xml, "src", sizeof "src" - 1u, &src, &src_len)) {
        // nowhere to point, but still an entry
        toc_warn(toc, BARTAP_WARN_TOC_TARGET, token->offset);
    }
    toc_entry(toc, src, src_len, token->offset, rec);
    toc->has_entry = 1;
    if (toc->label_offset != TOC_NO_LABEL) {
        // back for the label on the next call, once the fragment has been used
        toc->emit_state = TOC_EMIT_REREAD;
    }
    return true;
}

static void ncx_end(bartap_toc_t *toc, const bartap_xml_token_t *token)
{
    if (toc->mode == TOC_LABEL) {
        if (NAME_IS(token, "navlabel")) {
            end_label(toc);
        }
        return;
    }
    if (NAME_IS(token, "navpoint") && toc->depth > 0) {
        if (!toc->has_entry) {
            // no content in it, no entry
            toc_warn(toc, BARTAP_WARN_TOC_TARGET, token->offset);
        }
        toc->depth--;
        // the enclosing navPoint's entry came before its children
        toc->has_entry = 1;
        toc->label_offset = TOC_NO_LABEL;
    } else if (NAME_IS(token, "navmap")) {
        toc->mode = TOC_DONE;
    }
}

bartap_err_t bartap_toc_open(bartap_toc_t *toc, const bartap_book_t *book, uint8_t *window,
                             char *text_buf, uint32_t text_cap, bartap_warn_fn warn_fn,
                             void *warn_user)
{
    bartap_entry_t entry;

    if (toc == NULL) {
        return BARTAP_ERR_ARG;
    }
    memset(toc, 0, sizeof *toc);
    toc->book = book;
    toc->text_buf = text_buf;
    toc->text_cap = text_cap;
    toc->warnings.fn = warn_fn;
    toc->warnings.user = warn_user;
    toc->label_offset = TOC_NO_LABEL;
    toc->mode = TOC_DONE;

    if (book == NULL || window == NULL || text_buf == NULL) {
        toc->status = BARTAP_ERR_ARG;
        return toc->status;
    }
    if (text_cap < BARTAP_TEXT_MIN) {
        toc->status = BARTAP_ERR_BUFFER_TOO_SMALL;
        return toc->status;
    }
    // EPUB 3's nav first, the NCX for EPUB 2
    uint32_t dir_offset = book->nav_dir_offset;
    toc->is_ncx = dir_offset == BARTAP_NO_DIR_OFFSET;
    if (toc->is_ncx) {
        dir_offset = book->ncx_dir_offset;
    }
    if (dir_offset == BARTAP_NO_DIR_OFFSET) {
        // neither, an empty TOC: END now and on every next
        return BARTAP_END;
    }
    bartap_err_t err = bt_dir_entry(&book->directory, dir_offset, &entry);
    if (err == BARTAP_OK) {
        // its path, for resolving hrefs. it matched a resolved href at open, so it fits
        uint16_t len;
        err = bt_dir_name(&book->directory, dir_offset, toc->base_dir, BARTAP_PATH_MAX, &len);
        toc->base_dir_len = len;
    }
    if (err == BARTAP_OK) {
        err = bt_stream_open(&toc->stream, book->source, &entry, window);
    }
    if (err != BARTAP_OK) {
        // card failure or a damaged entry, keep it
        toc->status = err == BARTAP_ERR_BUFFER_TOO_SMALL ? BARTAP_ERR_CORRUPT : err;
        return toc->status;
    }
    // no ring: the NCX re-read needs only the inflate window (R26)
    bt_bytes_init_stream(&toc->bytes, &toc->stream, NULL);
    bt_xml_init(&toc->xml, &toc->bytes, &toc->warnings, BARTAP_NO_SPINE);
    toc->mode = TOC_SEARCH;
    return BARTAP_OK;
}

// A label's next text record, collapsed as text is (R4). BARTAP_END when the token has no more.
static bartap_err_t toc_fill(bartap_toc_t *toc, bartap_record_t *rec)
{
    char *buf = toc->text_buf;
    uint32_t n = 0;

    for (;;) {
        if (toc->stash_pos == toc->stash_len) {
            uint32_t got;
            bartap_err_t err = bt_xml_text(&toc->xml, toc->stash, sizeof toc->stash, &got);
            if (err != BARTAP_OK) {
                // error reading text, pass error up
                return err;
            }
            if (got == 0) {
                // the token's end
                break;
            }
            toc->stash_len = (uint8_t)got;
            toc->stash_pos = 0;
        }
        char c = toc->stash[toc->stash_pos];
        if (is_space(c)) {
            // only between words, never leading
            toc->stash_pos++;
            toc->space_pending = toc->space_allowed;
            continue;
        }
        if (n == 0 && toc->style != toc->style_out) {
            // a style change goes before the text it applies to
            rec->kind = BARTAP_REC_STYLE;
            rec->style.bits = toc->style;
            toc->style_out = toc->style;
            return BARTAP_OK;
        }
        uint32_t len = char_len((uint8_t)c);
        if (toc->space_pending + len > toc->text_cap - n) {
            if (n == 0) {
                // a 4-byte character and its space can't share a minimum buffer, the space goes alone
                toc->space_pending = 0;
                buf[n++] = ' ';
            }
            break;
        }
        if (toc->space_pending) {
            buf[n++] = ' ';
            toc->space_pending = 0;
        }
        memcpy(buf + n, toc->stash + toc->stash_pos, len);
        n += len;
        toc->stash_pos = (uint8_t)(toc->stash_pos + len);
        toc->space_allowed = 1;
    }
    if (n == 0) {
        return BARTAP_END;
    }
    rec->kind = BARTAP_REC_TEXT;
    rec->text.ptr = buf;
    rec->text.len = n;
    return BARTAP_OK;
}

// One step of the TOC, without the sticky failure.
static bartap_err_t toc_step(bartap_toc_t *toc, bartap_record_t *rec)
{
    for (;;) {
        if (toc->emit_state == TOC_EMIT_STYLE) {
            // a label starts with its style (R26), as a block does (R8)
            toc->emit_state = TOC_EMIT_NONE;
            rec->kind = BARTAP_REC_STYLE;
            rec->style.bits = toc->style;
            toc->style_out = toc->style;
            return BARTAP_OK;
        }
        if (toc->emit_state == TOC_EMIT_REREAD) {
            // back to the navLabel. from the inflate window if it's within 32 KiB, else the entry
            // rewinds (R26). once per navPoint: the entry is out, so the label is read this time.
            // warnings already raised there stay quiet
            toc->emit_state = TOC_EMIT_NONE;
            if (toc->xml.offset > toc->warnings.quiet_below) {
                toc->warnings.quiet_below = toc->xml.offset;
            }
            uint32_t offset = toc->label_offset;
            toc->label_offset = TOC_NO_LABEL;
            toc->in_text = 0;
            bartap_err_t err = bt_xml_seek(&toc->xml, offset);
            if (err != BARTAP_OK) {
                return err;
            }
            continue;
        }
        if (toc->in_text) {
            bartap_err_t err = toc_fill(toc, rec);
            if (err != BARTAP_END) {
                return err;
            }
            toc->in_text = 0;
        }
        if (toc->mode == TOC_DONE) {
            return BARTAP_END;
        }
        const bartap_xml_token_t *token;
        bartap_err_t err = bt_xml_next(&toc->xml, &token);
        if (err == BARTAP_END) {
            // the document ended, the list with it
            toc->mode = TOC_DONE;
            return BARTAP_END;
        }
        if (err != BARTAP_OK) {
            return err;
        }
        if (token->kind == BARTAP_XML_TEXT) {
            // only a label's text is output, the tokenizer runs the rest out
            toc->in_text = toc->mode == TOC_LABEL;
            toc->stash_len = 0;
            toc->stash_pos = 0;
            continue;
        }
        bool start = token->kind == BARTAP_XML_START_TAG;
        if (toc->mode == TOC_SEARCH) {
            const char *type;
            uint32_t type_len;
            if (!toc->is_ncx && start && NAME_IS(token, "nav") && !token->self_closing &&
                bt_xml_attr(&toc->xml, "epub:type", sizeof "epub:type" - 1u, &type, &type_len) &&
                bt_has_token(type, type_len, "toc", sizeof "toc" - 1u)) {
                // the toc nav, landmarks and page-list are others (R26)
                toc->mode = TOC_LIST;
                toc->nav_nest = 1;
            } else if (toc->is_ncx && start && NAME_IS(token, "navmap") && !token->self_closing) {
                toc->mode = TOC_LIST;
            }
            continue;
        }
        if (start) {
            bool entry = toc->is_ncx ? ncx_start(toc, token, rec) : nav_start(toc, token, rec);
            if (entry) {
                return BARTAP_OK;
            }
        } else if (toc->is_ncx) {
            ncx_end(toc, token);
        } else {
            nav_end(toc, token);
        }
    }
}

bartap_err_t bartap_toc_next(bartap_toc_t *toc, bartap_record_t *rec)
{
    if (toc == NULL || rec == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (toc->status != BARTAP_OK) {
        // failed before, open included
        return toc->status;
    }
    bartap_err_t err = toc_step(toc, rec);
    if (err != BARTAP_OK && err != BARTAP_END) {
        // keep it
        toc->status = err;
    }
    return err;
}

bartap_err_t bartap_toc_resolve(bartap_text_t *text, const char *id, uint32_t id_len,
                                bartap_bookmark_t *out)
{
    bartap_record_t rec;
    bool found = false;

    if (text == NULL || id == NULL || id_len == 0 || out == NULL) {
        return BARTAP_ERR_ARG;
    }
    if (text->xml.bytes == NULL) {
        // never opened
        return text->status != BARTAP_OK ? text->status : BARTAP_ERR_ARG;
    }
    out->version = (uint8_t)BARTAP_BOOKMARK_VERSION;
    out->book_hash = text->book->book_hash;
    out->spine_index = text->spine_index;
    out->chapter_crc32 = text->chapter_crc32;
    out->block_offset = 0;
    out->char_offset = 0;

    // from the start, quietly, records discarded (R13, R49)
    bartap_err_t err = restart(text);
    if (err != BARTAP_OK) {
        text->status = err;
        return err;
    }
    text->find_id = id;
    text->find_id_len = id_len;
    text->id_found = 0;
    replay_begin(text);
    for (;;) {
        err = text_step(text, &rec);
        if (err == BARTAP_MORE) {
            // just past the element's start tag
            found = true;
            if (text->parse.block_live) {
                // inside a block, or starting one: here in its text (R10)
                out->block_offset = text->parse.block_offset;
                out->char_offset = text->parse.char_offset;
                err = BARTAP_OK;
                break;
            }
            // between blocks, the next block holds where it starts
            continue;
        }
        if (err != BARTAP_OK) {
            break;
        }
        if (found && rec.kind == BARTAP_REC_BLOCK_START) {
            out->block_offset = rec.block.block_offset;
            out->char_offset = 0;
            break;
        }
    }
    replay_end(text);
    text->find_id = NULL;
    text->find_id_len = 0;
    text->id_found = 0;
    if (err == BARTAP_END && found) {
        // nothing after it: the end of the last block
        out->block_offset = text->parse.block_offset;
        out->char_offset = text->parse.char_offset;
        err = BARTAP_OK;
    } else if (err == BARTAP_END) {
        // a broken link, the chapter start (R13)
        text_warn(text, BARTAP_WARN_BROKEN_LINK, 0);
        err = BARTAP_OK;
    }
    if (err == BARTAP_OK) {
        // the caller seeks next, from the start like anyone else
        err = restart(text);
    }
    if (err != BARTAP_OK) {
        text->status = err;
    }
    return err;
}

uint32_t bartap_toc_warnings(const bartap_toc_t *toc)
{
    return toc != NULL ? toc->warnings.bits : 0u;
}
