/* Bartap: a zero-heap EPUB reader library. See REWRITE-DECISIONS.md (R-numbers below). */
#ifndef BARTAP_H
#define BARTAP_H

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

#ifdef __cplusplus
}
#endif

#endif /* BARTAP_H */
