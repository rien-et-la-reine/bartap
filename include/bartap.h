/* Bartap: a zero-heap EPUB reader library. See REWRITE-DECISIONS.md (R-numbers below). */
#ifndef BARTAP_H
#define BARTAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits (R33) ------------------------------------------------------------------------- */

/* Output-neutral, so configurable at build time. Define them identically for the library and its
 * callers; the CMake target does this through PUBLIC compile definitions. */
#ifndef BARTAP_INPUT_BUF
#define BARTAP_INPUT_BUF 512 /* compressed bytes read from the card per inflate refill */
#endif
#ifndef BARTAP_XML_READAHEAD
#define BARTAP_XML_READAHEAD 256 /* tokenizer lookahead */
#endif

#ifndef __cplusplus /* the library's own C build always checks these */
_Static_assert(BARTAP_INPUT_BUF > 0, "BARTAP_INPUT_BUF must be positive");
_Static_assert(BARTAP_XML_READAHEAD > 0, "BARTAP_XML_READAHEAD must be positive");
#endif

/* Fixed: these can change what is truncated, warned about or how blocks are built. */
#define BARTAP_META_MAX 127u   /* title, creator, language, identifier (R25) */
#define BARTAP_PATH_MAX 255u   /* resolved path */
#define BARTAP_ATTR_MAX 1024u  /* attribute storage, also bounds fragment ids */
#define BARTAP_DEPTH_MAX 32u   /* formatting stack; never varies between builds */
#define BARTAP_TEXT_MIN 4u     /* smallest text buffer: one full UTF-8 character (R50) */
#define BARTAP_WINDOW_SIZE 32768u /* inflate window (R27) */
#define BARTAP_BOOKMARK_SIZE 23u  /* serialized bookmark (R12) */
#define BARTAP_BOOKMARK_VERSION 1u /* bump on any change to text output or block boundaries (R12) */

#define BARTAP_NO_SPINE 0xFFFFu /* TOC entry with no resolvable target (R26) */

/* ---- Results (R42) ------------------------------------------------------------------------ */

/* Statuses are zero or positive, errors negative. Values are not stable before 1.0. */
typedef enum {
    BARTAP_OK = 0,
    BARTAP_END = 1,  /* a stream or iterator is finished */
    BARTAP_MORE = 2, /* verify has more to do (R38) */

    BARTAP_ERR_IO = -1,              /* the read callback failed (R37) */
    BARTAP_ERR_CORRUPT = -2,         /* the data contradicts itself or fails a check */
    BARTAP_ERR_UNSUPPORTED = -3,     /* valid, but not handled here (R30, R40) */
    BARTAP_ERR_ARG = -4,             /* invalid argument from the caller */
    BARTAP_ERR_BUFFER_TOO_SMALL = -5,
    BARTAP_ERR_WRONG_BOOK = -6       /* a bookmark belongs to another book (R12) */
} bartap_err_t;

/* The code's name for logs, such as "BARTAP_ERR_CORRUPT"; not user-facing text. */
const char *bartap_strerror(bartap_err_t err);

/* ---- Warnings (R32) ----------------------------------------------------------------------- */

/* One bit per kind, accumulated by each book, text and TOC object. */
typedef enum {
    BARTAP_WARN_PACKAGING = 1 << 0,         /* mimetype missing, compressed or not first (R30) */
    BARTAP_WARN_MALFORMED_MARKUP = 1 << 1,  /* (R30) */
    BARTAP_WARN_INVALID_UTF8 = 1 << 2,      /* read as Windows-1252 (R30) */
    BARTAP_WARN_CONTROL_CHAR = 1 << 3,      /* dropped from text (R21) */
    BARTAP_WARN_UNKNOWN_ENTITY = 1 << 4,    /* passed through literally (R30) */
    BARTAP_WARN_METADATA_TRUNCATED = 1 << 5, /* (R25) */
    BARTAP_WARN_NO_UNIQUE_ID = 1 << 6,      /* first dc:identifier stood in (R25) */
    BARTAP_WARN_ATTR_TRUNCATED = 1 << 7,    /* attribute storage limit (R33) */
    BARTAP_WARN_PATH_TOO_LONG = 1 << 8,     /* resolved path limit (R33) */
    BARTAP_WARN_SPINE_TRUNCATED = 1 << 9,   /* more items than the caller's array (R41) */
    BARTAP_WARN_SPINE_ITEM_MISSING = 1 << 10, /* no archive entry for a spine item */
    BARTAP_WARN_DUPLICATE_NAME = 1 << 11,   /* (R44) */
    BARTAP_WARN_BROKEN_LINK = 1 << 12,      /* TOC fragment id not found (R13) */
    BARTAP_WARN_TOC_TARGET = 1 << 13,       /* TOC target not in the spine (R26) */
    BARTAP_WARN_IMAGE_UNRESOLVED = 1 << 14, /* no record produced (R9) */
    BARTAP_WARN_ORDINAL_CLAMPED = 1 << 15,  /* out-of-range list number (R45) */
    BARTAP_WARN_REVERSED_NO_START = 1 << 16, /* counted up from 1 instead (R45) */
    BARTAP_WARN_CLOSE_UNWOUND = 1 << 17,    /* close tag closed frames above it (R47) */
    BARTAP_WARN_CLOSE_UNMATCHED = 1 << 18,  /* close tag matched nothing (R47) */
    BARTAP_WARN_UNCLOSED_AT_END = 1 << 19,  /* (R47) */
    BARTAP_WARN_DEPTH_EXCEEDED = 1 << 20    /* element ignored past the limit (R47) */
} bartap_warning_t;

/* Called at each occurrence. Must return quickly and must not call into Bartap. spine_index is
 * BARTAP_NO_SPINE when the warning has no chapter. */
typedef void (*bartap_warn_fn)(void *user, bartap_warning_t code, uint16_t spine_index,
                               uint32_t offset);

/* Embedded in each object that warns. Fields are Bartap's (R28). */
typedef struct {
    uint32_t bits;
    bartap_warn_fn fn; /* NULL is always valid */
    void *user;
    uint32_t quiet_below; /* no callback for offsets below this, during a replay (R49) */
} bartap_warnings_t;

/* ---- I/O (R37) ---------------------------------------------------------------------------- */

/* Reads exactly len bytes at offset, returning 0 on success. All-or-nothing; any non-zero return
 * is one I/O error, and the adapter logs the platform's cause. Never asked for bytes past size. */
typedef int (*bartap_read_fn)(void *user, uint32_t offset, uint8_t *buf, uint32_t len);

typedef struct {
    bartap_read_fn read;
    void *user;
    uint32_t size; /* the archive's size */
} bartap_source_t;

/* Fills src to read an archive held in memory. data must outlive src and is never written to. */
bartap_err_t bartap_source_memory(bartap_source_t *src, const void *data, uint32_t size);

/* ---- Records (R6, R20) -------------------------------------------------------------------- */

typedef enum {
    BARTAP_REC_BLOCK_START = 1, /* R16 */
    BARTAP_REC_CONTAINER_START, /* R45 */
    BARTAP_REC_CONTAINER_END,   /* R45 */
    BARTAP_REC_TEXT,            /* R6 */
    BARTAP_REC_STYLE,           /* R7 */
    BARTAP_REC_IMAGE,           /* R9 */
    BARTAP_REC_LINE_BREAK,      /* R6; no fields */
    BARTAP_REC_TOC_ENTRY        /* R26 */
} bartap_record_kind_t;

/* R46. Implicit paragraphs and restarts (R46) are ordinary blocks of their kind. */
typedef enum {
    BARTAP_BLOCK_PARAGRAPH = 1,
    BARTAP_BLOCK_HEADING, /* level is 1 to 6 */
    BARTAP_BLOCK_PRE,
    BARTAP_BLOCK_HR,
    BARTAP_BLOCK_DT
} bartap_block_kind_t;

/* R46 */
typedef enum {
    BARTAP_CONTAINER_BLOCKQUOTE = 1,
    BARTAP_CONTAINER_OL,
    BARTAP_CONTAINER_UL,
    BARTAP_CONTAINER_LI,
    BARTAP_CONTAINER_DL,
    BARTAP_CONTAINER_DD,
    BARTAP_CONTAINER_NOTE
} bartap_container_kind_t;

/* Independent and additive; a record holds the complete style (R7). */
#define BARTAP_STYLE_ITALIC (1u << 0)
#define BARTAP_STYLE_BOLD (1u << 1)
#define BARTAP_STYLE_MONO (1u << 2)
#define BARTAP_STYLE_SUB (1u << 3)
#define BARTAP_STYLE_SUP (1u << 4)
#define BARTAP_STYLE_UNDERLINE (1u << 5)
#define BARTAP_STYLE_STRIKE (1u << 6)

/* One record per call. Pointers are valid only until the next call on the same object. */
typedef struct {
    bartap_record_kind_t kind;
    union {
        struct {
            bartap_block_kind_t kind;
            uint8_t level;         /* headings only, otherwise 0 */
            uint16_t spine_index;
            uint32_t block_offset; /* raw XHTML offset (R12) */
            uint32_t char_offset;  /* 0, unless a seek landed inside the block (R16, R49) */
        } block;
        struct {
            bartap_container_kind_t kind;
            int32_t ordinal; /* a list item's number, otherwise 0 (R45) */
        } container;
        struct {
            const char *ptr; /* in the caller's text buffer */
            uint32_t len;
        } text;
        struct {
            uint32_t bits; /* ignore unknown bits; apply the known ones (R7) */
        } style;
        struct {
            uint32_t dir_offset; /* the entry's central-directory record (R9) */
        } image;
        struct {
            uint16_t depth;
            uint16_t spine_index; /* or BARTAP_NO_SPINE */
            const char *fragment; /* no terminator; NULL when there is none */
            uint32_t fragment_len;
        } toc;
    };
} bartap_record_t;

/* ---- State structs (R28) ------------------------------------------------------------------ */

/* Declared by the caller by real type, so sizeof is the size. Fields are Bartap's: callers read
 * nothing from them. */

/* -- Layer 1: archive and entries (zip.c) -- */

/* Huffman tables live here; the window is the caller's (R27, R39). */
typedef struct {
    uint32_t bit_buf;
    uint8_t bit_count;
    uint8_t block_type;
    uint8_t final_block;
    uint8_t state;
    uint32_t stored_left;
    uint16_t lit_count[16];
    uint16_t lit_symbol[288];
    uint16_t dist_count[16];
    uint16_t dist_symbol[32];
} bartap_inflate_t;

/* An entry's facts, always from its central-directory record (R22). */
typedef struct {
    uint32_t dir_offset;
    uint32_t local_offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc32;
    uint16_t method; /* 0 stored, 8 deflate */
} bartap_entry_t;

/* One entry, streamed out inflated, checked as bytes pass (R31). */
typedef struct {
    const bartap_source_t *source;
    bartap_entry_t entry;
    uint32_t data_offset;     /* past the local header */
    uint32_t compressed_read;
    uint32_t delivered;
    uint32_t crc32;
    uint8_t *window;          /* BARTAP_WINDOW_SIZE bytes (R27) */
    uint32_t window_write;
    uint32_t window_read;
    uint32_t window_valid;    /* how far back the window can be re-read (R26) */
    uint32_t input_pos;
    uint32_t input_len;
    bartap_err_t status;      /* sticky once finished or failed */
    bartap_inflate_t inflate;
    uint8_t input[BARTAP_INPUT_BUF];
} bartap_stream_t;

/* Raw when raw is non-NULL, otherwise every walk reads the card; never partial (R24). */
typedef struct {
    const bartap_source_t *source;
    const uint8_t *raw;
    uint32_t offset;
    uint32_t size;
    uint32_t count;
} bartap_directory_t;

/* A spine slot in the caller's array (R22, R41). */
typedef struct {
    uint32_t path_hash; /* holds the idref hash until OPF pass 2 */
    uint32_t dir_offset;
    uint32_t local_offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc32;
    uint16_t path_len;
    uint8_t method;
    uint8_t flags;
} bartap_spine_item_t;

typedef struct {
    bartap_stream_t stream;
} bartap_resource_t;

/* Incremental, and never moves a reader's position (R31, R38). */
typedef struct {
    bartap_stream_t stream;
} bartap_verify_t;

/* -- Layer 2: tokenizer and package (xml.c, package.c) -- */

struct bartap_rolling;

/* What the tokenizer reads from: an entry stream or a block of memory (R15). */
typedef struct {
    bartap_stream_t *stream;       /* NULL in memory mode */
    const uint8_t *memory;
    uint32_t memory_size;
    struct bartap_rolling *rolling; /* ring tap, or NULL (R48) */
    uint32_t position;
} bartap_bytes_t;

#define BARTAP_XML_NAME_MAX 32u  /* longer names cannot match a known element */
#define BARTAP_XML_ATTRS_MAX 16u /* attributes kept per tag, in BARTAP_ATTR_MAX bytes */
#define BARTAP_XML_ENTITY_MAX 32u /* longer entity names pass through literally (R30) */

typedef struct {
    uint16_t name_offset;
    uint16_t name_len;
    uint16_t value_offset;
    uint16_t value_len;
} bartap_xml_attr_t;

typedef enum {
    BARTAP_XML_TEXT = 1, /* read the decoded bytes with bt_xml_text */
    BARTAP_XML_START_TAG,
    BARTAP_XML_END_TAG
} bartap_xml_token_kind_t;

typedef struct {
    bartap_xml_token_kind_t kind;
    uint32_t offset;      /* source offset of the token's first byte */
    uint32_t end_offset;  /* first byte past it; text tokens only know this once read */
    uint8_t self_closing;
    uint8_t name_len;
    char name[BARTAP_XML_NAME_MAX]; /* local name, ASCII-folded (R46) */
} bartap_xml_token_t;

/* Tokens must not depend on the read-ahead's size (R33): bytes consumed but not yet delivered are held
 * here, never in the read-ahead. */
typedef struct {
    bartap_bytes_t *bytes;
    bartap_warnings_t *warnings; /* NULL when the tokenizer is tested alone */
    uint32_t offset;        /* consumed, excluding read-ahead (R48) */
    uint32_t readahead_pos;
    uint32_t readahead_len;
    uint32_t attr_used;
    uint32_t attr_count;
    bartap_err_t status;    /* sticky once failed */
    uint16_t spine_index;   /* reported with warnings */
    uint8_t state;
    uint8_t text_done;
    uint8_t cdata;          /* inside a CDATA section */
    uint8_t brackets;       /* CDATA ']' bytes held, which may be its end */
    uint8_t staged_len;
    uint8_t held_len;
    uint8_t replay_len;
    uint8_t replay_pos;
    uint8_t staged[4];      /* UTF-8 bytes awaiting decode */
    char held[BARTAP_TEXT_MIN]; /* one decoded character */
    char replay[BARTAP_XML_ENTITY_MAX + 2u]; /* literal bytes owed: '&', a name, ';' */
    bartap_xml_token_t token;
    bartap_xml_attr_t attrs[BARTAP_XML_ATTRS_MAX];
    char attr_storage[BARTAP_ATTR_MAX];
    uint8_t readahead[BARTAP_XML_READAHEAD];
} bartap_xml_t;

#define BARTAP_NO_DIR_OFFSET 0xFFFFFFFFu

typedef struct {
    const bartap_source_t *source;
    bartap_directory_t directory;
    bartap_spine_item_t *spine;
    bartap_warnings_t warnings;
    uint32_t book_hash;     /* of the unique identifier (R12, R25) */
    uint32_t nav_dir_offset;
    uint32_t ncx_dir_offset;
    uint32_t cover_dir_offset;
    uint16_t spine_capacity;
    uint16_t spine_count;
    uint32_t spine_required; /* always reported, even when truncated (R41) */
    uint8_t title_len;
    uint8_t creator_len;
    uint8_t language_len;
    uint8_t identifier_len;
    uint8_t creator_count;  /* saturates at 255 (R25) */
    char title[BARTAP_META_MAX + 1u];
    char creator[BARTAP_META_MAX + 1u];
    char language[BARTAP_META_MAX + 1u];
    char identifier[BARTAP_META_MAX + 1u];
} bartap_book_t;

/* Opening's parsing state, caller-owned because it is too big for a small stack (R27). */
typedef struct {
    bartap_stream_t stream;
    bartap_bytes_t bytes;
    bartap_xml_t xml;
    uint32_t unique_id_hash; /* ids from the OPF's first pass, matched in its second */
    uint32_t toc_id_hash;
    uint32_t cover_id_hash;
    uint32_t nav_path_hash;  /* paths from the second pass, matched in the directory walk (R23) */
    uint32_t ncx_path_hash;
    uint32_t cover_path_hash;
    uint16_t nav_path_len;   /* 0 while not found */
    uint16_t ncx_path_len;
    uint16_t cover_path_len;
    uint16_t opf_path_len;
    uint8_t found;           /* which of the ids above the OPF has */
    char opf_path[BARTAP_PATH_MAX + 1u];
    char resolved[BARTAP_PATH_MAX + 1u];
} bartap_open_work_t;

/* What callers read about a book, through bartap_book_metadata. Strings have no terminator and live
 * as long as the book (R25). */
typedef struct {
    const char *title;
    const char *creator; /* the first dc:creator */
    const char *language;
    const char *identifier;
    uint8_t title_len;
    uint8_t creator_len;
    uint8_t language_len;
    uint8_t identifier_len;
    uint8_t creator_count;
} bartap_metadata_t;

/* -- Layer 3: text, TOC, bookmarks, seek (text.c) -- */

/* Tags tracked for ignored-past-depth close tags (R47): every element named in R46, br included. */
#define BARTAP_TAG_COUNT 55u

typedef struct {
    uint8_t tag;
    uint8_t class_kind; /* container, block, inline or boundary */
    uint8_t flags;      /* such as a list's reversed */
    uint32_t style;     /* style to restore when this frame pops */
    int32_t ordinal;    /* lists: the next item's number; items: their own (R45) */
} bartap_frame_t;

/* All a checkpoint needs to resume parsing (R48). */
typedef struct {
    bartap_frame_t frames[BARTAP_DEPTH_MAX];
    uint8_t ignored[BARTAP_TAG_COUNT]; /* per-tag counts past the depth limit (R47) */
    uint8_t depth;
    uint8_t block_live;    /* an output block is open, explicit or not (R46) */
    uint8_t block_kind;
    uint8_t block_level;
    uint8_t block_has_content;
    uint8_t space_allowed; /* the block's last content was text (R4) */
    uint8_t space_pending; /* (R4) */
    uint8_t skip_tag;
    uint16_t skip_depth;
    uint32_t style;        /* from the frames */
    uint32_t style_out;    /* in the last style record (R7) */
    uint32_t space_style;  /* the pending space's, from its first whitespace (R4) */
    uint32_t block_offset;
    uint32_t char_offset;  /* text bytes into the block (R10) */
} bartap_parse_state_t;

typedef struct {
    bartap_parse_state_t parse;
    uint32_t block_offset;  /* the value bookmarks hold */
    uint32_t resume_offset; /* first byte the snapshot has not consumed (R48) */
    uint8_t valid;
} bartap_checkpoint_t;

/* Optional, passed when a text stream is opened. ring NULL with size non-zero is memory mode's
 * virtual window (R48). */
typedef struct bartap_rolling {
    bartap_checkpoint_t checkpoints[2];
    uint8_t *ring;
    uint32_t size;
    uint32_t delivered;      /* total bytes the stream has delivered */
    uint32_t next_checkpoint;
} bartap_rolling_t;

/* One spine item from its start to END (R17). */
typedef struct {
    const bartap_book_t *book;
    bartap_rolling_t *rolling;
    char *text_buf;
    uint32_t text_cap;
    bartap_warnings_t warnings;
    uint32_t high_water;  /* callback stays silent below this during replay (R49) */
    uint32_t target_char; /* where a replay lands in its block (R49) */
    const char *find_id;  /* an element id bartap_toc_resolve looks for (R13), or NULL */
    uint32_t find_id_len;
    uint16_t spine_index;
    uint16_t base_dir_len;
    uint32_t image_dir_offset; /* a queued image record's (R9) */
    uint8_t emit_state;   /* for moments that produce several records (R20) */
    uint8_t emit_next;    /* what follows a queued block start's style */
    uint8_t emit_index;
    uint8_t unwind_to;    /* depth an end tag unwinds to, popped one frame per call */
    uint8_t replaying;    /* records are discarded: no text copied, no images looked up (R49) */
    uint8_t landing;      /* replaying inside the target block, text stops at target_char */
    uint8_t id_found;     /* the start tag just handled carried find_id */
    uint8_t stash_len;    /* decoded text read but not yet delivered */
    uint8_t stash_pos;
    uint8_t cr_seen;      /* in pre, a CR's LF is part of the same break */
    uint8_t in_text;      /* a text token is being delivered */
    uint8_t finished;     /* the source ended, only closing records are left */
    bartap_err_t status;  /* sticky once failed */
    char stash[BARTAP_TEXT_MIN];
    bartap_stream_t stream;
    bartap_bytes_t bytes;
    bartap_xml_t xml;
    bartap_parse_state_t parse;
    char base_dir[BARTAP_PATH_MAX + 1u]; /* for resolving image paths (R22) */
} bartap_text_t;

/* Same records as text (R26). */
typedef struct {
    const bartap_book_t *book;
    char *text_buf;
    uint32_t text_cap;
    bartap_warnings_t warnings;
    uint32_t label_offset; /* where an NCX label re-read starts (R26), or none */
    uint32_t style;
    uint32_t style_out;    /* in the last style record */
    bartap_err_t status;   /* sticky once failed */
    uint16_t depth;        /* open lists (nav) or navPoints (NCX) */
    uint16_t nav_nest;     /* nav elements open inside the toc nav */
    uint16_t label_nest;   /* label elements of the label's own name open */
    uint16_t base_dir_len;
    uint8_t is_ncx;
    uint8_t mode;          /* looking for the list, in it, in a label, or done */
    uint8_t label_tag;
    uint8_t has_entry;     /* the current li or navPoint has returned its entry */
    uint8_t emit_state;
    uint8_t in_text;
    uint8_t space_pending;
    uint8_t space_allowed;
    uint8_t stash_len;
    uint8_t stash_pos;
    char stash[BARTAP_TEXT_MIN];
    uint8_t style_count[8]; /* open inline elements per style bit, labels can nest them */
    bartap_stream_t stream;
    bartap_bytes_t bytes;
    bartap_xml_t xml;
    char base_dir[BARTAP_PATH_MAX + 1u];
} bartap_toc_t;

/* A bookmark's fields, parsed. The caller stores only the serialized bytes (R1, R12). */
typedef struct {
    uint8_t version;
    uint32_t book_hash;
    uint16_t spine_index;
    uint32_t block_offset;
    uint32_t char_offset;
    uint32_t chapter_crc32;
} bartap_bookmark_t;

/* ---- API ---------------------------------------------------------------------------------- */

/* -- Book (R23, R24, R25, R27, R41) -- */

/* dir_buf may be NULL; dir_buf and spine must outlive the book. A window belongs to this call until
 * it returns; work may be reused afterwards. warn_fn may be NULL. */
bartap_err_t bartap_open(bartap_book_t *book, const bartap_source_t *source,
                         bartap_spine_item_t *spine, uint16_t spine_capacity,
                         uint8_t *dir_buf, uint32_t dir_buf_size,
                         bartap_open_work_t *work, uint8_t *window,
                         bartap_warn_fn warn_fn, void *warn_user);

bartap_err_t bartap_book_metadata(const bartap_book_t *book, bartap_metadata_t *out);

/* count is what the array holds; required is what the book needs (R41). */
bartap_err_t bartap_book_spine_count(const bartap_book_t *book, uint16_t *count, uint32_t *required);

/* Non-linear items are the caller's policy (R17). */
bartap_err_t bartap_book_spine_linear(const bartap_book_t *book, uint16_t spine_index, bool *linear);

/* Directory-record offset of the cover, for bartap_resource_open_dir. BARTAP_END if none. */
bartap_err_t bartap_book_cover(const bartap_book_t *book, uint32_t *dir_offset);

/* The sticky warning bits of each kind of object (R32). */
uint32_t bartap_book_warnings(const bartap_book_t *book);
uint32_t bartap_text_warnings(const bartap_text_t *text);
uint32_t bartap_toc_warnings(const bartap_toc_t *toc);

/* -- Resources: one buffer per call (R38) -- */

/* By name, under R43's matching; first in the directory wins (R44). */
bartap_err_t bartap_resource_open(bartap_resource_t *res, const bartap_book_t *book,
                                  const char *path, uint32_t path_len, uint8_t *window);

/* From an image record's or the cover's directory-record offset (R9). Untrusted: checked (R29). */
bartap_err_t bartap_resource_open_dir(bartap_resource_t *res, const bartap_book_t *book,
                                      uint32_t dir_offset, uint8_t *window);

/* A whole chapter, such as for inflating it into PSRAM for memory mode (R11). */
bartap_err_t bartap_resource_open_spine(bartap_resource_t *res, const bartap_book_t *book,
                                        uint16_t spine_index, uint8_t *window);

/* The entry's inflated size, for sizing a destination. */
uint32_t bartap_resource_size(const bartap_resource_t *res);

/* BARTAP_OK with *got bytes, or BARTAP_END only once CRC and size match (R31). */
bartap_err_t bartap_resource_read(bartap_resource_t *res, uint8_t *buf, uint32_t cap, uint32_t *got);

/* -- Verify (R31, R38) -- */

bartap_err_t bartap_verify_init(bartap_verify_t *verify, const bartap_book_t *book,
                                uint16_t spine_index, uint8_t *window);

bartap_err_t bartap_verify_init_dir(bartap_verify_t *verify, const bartap_book_t *book,
                                    uint32_t dir_offset, uint8_t *window);

/* Inflates up to max_bytes. BARTAP_MORE, BARTAP_OK when the entry checks out, or an error. */
bartap_err_t bartap_verify_step(bartap_verify_t *verify, uint32_t max_bytes);

/* -- Text (R6, R11, R17, R20) -- */

/* Stream mode. text_cap must be at least BARTAP_TEXT_MIN (R50); rolling and warn_fn may be NULL. */
bartap_err_t bartap_text_open(bartap_text_t *text, const bartap_book_t *book, uint16_t spine_index,
                              uint8_t *window, char *text_buf, uint32_t text_cap,
                              bartap_rolling_t *rolling, bartap_warn_fn warn_fn, void *warn_user);

/* Memory mode: data is the item's already-inflated bytes, and must outlive the text object. */
bartap_err_t bartap_text_open_memory(bartap_text_t *text, const bartap_book_t *book,
                                     uint16_t spine_index, const uint8_t *data, uint32_t size,
                                     char *text_buf, uint32_t text_cap, bartap_rolling_t *rolling,
                                     bartap_warn_fn warn_fn, void *warn_user);

/* BARTAP_OK with a record, or BARTAP_END when the item is finished. */
bartap_err_t bartap_text_next(bartap_text_t *text, bartap_record_t *rec);

/* Applies R12's checks 2 to 4 for an item already opened; check spine_index with
 * bartap_bookmark_read first. The next records are as R49 describes. */
bartap_err_t bartap_text_seek(bartap_text_t *text, const bartap_bookmark_t *bookmark);

/* -- Rolling buffer (R48) -- */

/* size must be a power of two. ring NULL is memory mode's virtual window. */
bartap_err_t bartap_rolling_init(bartap_rolling_t *rolling, uint8_t *ring, uint32_t size);

/* -- Bookmarks (R12, R19) -- */

/* Serializes byte by byte into out, BARTAP_BOOKMARK_SIZE bytes. */
bartap_err_t bartap_bookmark_write(const bartap_book_t *book, uint16_t spine_index,
                                   uint32_t block_offset, uint32_t char_offset, uint8_t *out);

/* Untrusted input (R29). BARTAP_ERR_CORRUPT on a failed bookmark CRC-32 or a spine index out of
 * range, BARTAP_ERR_WRONG_BOOK on a differing book hash. */
bartap_err_t bartap_bookmark_read(const bartap_book_t *book, const uint8_t *in,
                                  bartap_bookmark_t *out);

/* -- TOC (R13, R26) -- */

/* BARTAP_END if the book has neither nav nor NCX. text_cap as for bartap_text_open. */
bartap_err_t bartap_toc_open(bartap_toc_t *toc, const bartap_book_t *book, uint8_t *window,
                             char *text_buf, uint32_t text_cap, bartap_warn_fn warn_fn,
                             void *warn_user);

/* BARTAP_OK with a record, or BARTAP_END. A record's fragment is valid until the next call. */
bartap_err_t bartap_toc_next(bartap_toc_t *toc, bartap_record_t *rec);

/* Streams an opened text object from its start to the element with this id, and fills out for
 * bartap_text_seek. An id not found gives the chapter start, with a warning (R13). */
bartap_err_t bartap_toc_resolve(bartap_text_t *text, const char *id, uint32_t id_len,
                                bartap_bookmark_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BARTAP_H */
