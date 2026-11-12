# Bartap rewrite — decisions

Decisions for the manual rewrite of Bartap, settled 2026-11-04, revised 2026-11-05 (one record
per call, R20) and amended 2026-11-07 (implementation planning). The pre-rewrite code is `bartap` at
`ccdc61d`; its D1–D22 (`lib/bartap/DECISIONS.md`) are all open to reconsideration except R1.

## R1 — Fixed constraints

- No heap. Bartap never allocates; the caller owns all memory.
- The only thing written to the card is book state (essentially bookmarks).
- Everything else, including every old decision, is open.

## R2 — Plan

- The manual rewrite is the next step. The stream-mode seek buffer (old D16's tail buffer, now R48's rolling buffer) is implemented as part of the
  rewrite, against the updated contract, rather than retrofitted onto the old code.
- Bartap lives in its own repository, this one, which holds the library and its host tests only. The
  SD driver and FatFs belong to Tavernkeep, which imports Bartap (R51).

## R3 — Three layers

1. **Zip** — the archive and its entries, streamed out inflated.
2. **OCF / package** — container, package and navigation; XML handling sits with this layer.
3. **Text** — returns the chapter's content one record per call (R6, R20), in a form a layout engine
   consumes.

## R4 — Whitespace

- Whitespace collapsing happens in the text layer, so every caller gets the same words.
- Collapsing state carries across style records: in `a <i> b</i>` the two spaces collapse to one, so
  the italic text arrives as "b", not " b". This is what makes "no space at the join" (R6)
  trustworthy.
- A collapsed space is held pending. It is flushed before a style record or the next non-space
  character, and dropped at any structural record (R6), so blocks have no trailing spaces.

## R5 — Images

Images may sit inline inside a block wherever the EPUB places them; Bartap does not assume they are
block-level.

## R6 — Output is a sequence of records

- The text layer returns one record per call (R20). A record is a typed struct, `bartap_record_t`:
  a kind and that kind's fields. Kinds: block start (R16), container start and end (R45), text,
  style (R7), image (R9), line break (`pre`, `<br>`), and TOC entry (R26).
- A text record carries a pointer and length into a text buffer the caller provides. The text is
  valid only until the next call, which reuses the buffer.
- Text longer than the caller's text buffer is split into consecutive text records, at a UTF-8
  character boundary.
- Word boundaries are decided by text alone:
  - a break opportunity is a space inside text (collapsed, R4) or a structural record: block start,
    container start or end, line break, image, or `END`;
  - style records never create one. `un<i>like</i>ly` arrives as text "un", style, text "like",
    style, text "ly": no space at either join, so one word of three runs;
  - consecutive text records, with only style records between them, are one continuous run.
- An inline image is a break opportunity on both sides, as CSS treats inline replaced elements.
- Consequence for layout: a word can span several calls, so layout copies each fragment into its own
  word buffer before the next call, and needs its own maximum word length and a forced-break policy
  for words longer than a line. Kerning across a style change is not attempted.
- Forward compatibility: layout ignores record kinds it does not know (its `switch` default), which
  is enough because Bartap is compiled with its caller (R28).

## R7 — Style records carry the complete style

- A style record holds the whole current style, not a toggle of one attribute.
- Unknown style bits are ignored; the known bits are still applied.
- Style bits are independent and additive. A change that alters what existing bits mean is not made;
  a new bit is added instead.

## R8 — Style is restated at every block start

- Every block start is followed by a style record, and the records after a seek (R49) include one,
  so layout never needs style carried over from before a block or a seek.

## R9 — Image records

An image record carries the offset of the image entry's central-directory record (4 bytes), not its
path. (Revised from the local header offset to follow R22.)

- The text layer looks the image's path up when it emits the record. With a directory buffer
  (R24) this is a RAM walk; without one, it is a card walk per image, paid even if the caller never
  draws the image. A caller reading heavily illustrated books can avoid that by passing a buffer.
- Opening the image later starts from the directory record, the source of truth for its sizes,
  CRC-32 and local header offset.
- An image that cannot be resolved (entry missing, `data:` or remote URL) produces no record and a
  warning. An image outside any block opens an implicit paragraph.

## R10 — Bookmarks count text bytes only

Character offsets in bookmarks count the bytes of text records only, never other records.

## R11 — Stream mode is a first-class path

- Context: Rev F's U206 footprint takes either PSRAM or a second NOR flash; the part is chosen just
  before fab, so the firmware cannot assume PSRAM. Other users of Bartap may have none either.
- Decision: Stream mode (inflating from the card as text is read) and memory mode (reading an
  already-inflated chapter) are both fully supported, and seeking in stream mode must be acceptably
  fast. A stream-mode seek buffer therefore stays in scope (R48).
- Bartap does not know or care which part is fitted; the caller chooses the mode per chapter.

## R12 — Bookmark contents

- Fields, serialized one by one (never a struct dump), 23 bytes:
  - version, 1 byte: covers **the text layer's behaviour**. Any change that can alter text output or
    block boundaries (whitespace rules R4, control characters R21, element classes R46, recovery R47,
    depth R33, named entities R30) bumps it, even if no field changes.
  - book hash, 4 bytes: hash of the book's unique identifier (R25), using Bartap's name hash.
  - spine index, 2 bytes
  - block offset, 4 bytes: raw XHTML offset where the block starts
  - character offset, 4 bytes: text bytes into the block (R10); 4 bytes because a
    single block can exceed 64 KiB
  - chapter CRC-32, 4 bytes, taken from the central directory
  - bookmark CRC-32, 4 bytes: CRC-32 of the preceding 19 bytes
- Field offsets, the length and the position of the bookmark CRC-32 are frozen, so a bookmark of any
  version can be read. A true layout change would need new code to refuse old layouts.
- Resolving a bookmark, each check falling back one level:
  0. Bookmark CRC-32 differs: no field can be trusted, not even the spine index, so there is no
     chapter to fall back to. Refused with `BARTAP_ERR_CORRUPT`; the caller decides what to open instead.
  1. Book hash differs: the bookmark belongs to another book and is refused with an error; the
     caller decides what to open instead.
  2. Chapter CRC-32 differs: the chapter changed; land at the start of that chapter.
  3. Version differs (book and chapter unchanged): the character offset is not meaningful under
     the current rules, but the block offset is a raw position in the book's bytes. Land at the
     start of the block containing that offset.
  4. All match: exact landing at block offset and character offset. A block offset that is not a
     block start, or a character offset past the block's end or inside a UTF-8 sequence, is invalid
     and lands at the chapter start (R29).
- A valid bookmark whose spine index is out of range, including a chapter lost to spine truncation
  (R41), has no chapter to land in and is refused with `BARTAP_ERR_CORRUPT`.
- Check 3 finds its block in two passes: find the last block start at or before the offset, then
  replay to it. This needs no extra memory, and the path is rare.
- The book hash uses the unique identifier rather than file bytes, so a bookmark survives the EPUB
  being renamed, moved or repacked; content changes are caught by the chapter CRC-32 instead.
- Rejected: anchoring the character position in raw bytes (exact across Bartap versions, but every
  text record would carry a 4-byte source offset, for the sake of library upgrades).
- 2 bytes of spine index are enough: a spine's length is bounded by the number of archive entries,
  and the caller's spine array runs out of RAM far below 65,535 items anyway.

## R13 — TOC links resolve to bookmarks

- A TOC target (`chapter.xhtml#id`) is resolved when it is followed, by streaming that chapter from
  its start until the element with that `id` is reached.
- The result is an exact landing in bookmark form (R12): the block offset of the block containing the
  element, plus the character offset of the element within that block's text.
- Resolving also lands the text object there, in the same pass, with R49's records next: the scan
  has already stopped at the element, so a seek afterwards would only read the chapter's start a
  second time. The bookmark is for saving the position. A seek to it gives the same landing, except
  that it stops right after the text before the element, so a line break or image between that text
  and the element comes out again.
- An `id` not found in the chapter lands at the chapter start, the same fallback as R12, and raises
  a warning (R32) so the caller can log the broken link.

## R14 — Pull, not push

- The caller pulls the next record; the text layer pulls tokens from the tokenizer, which pulls bytes
  from its byte source.
- Rationale: one pipeline serves normal reading, flipping back, seeking forward and back, bookmarks
  and TOC links. Under pull the layer doing the work knows which of these it is serving and can
  rewind or replay its own source; under push the lower layers cannot know what is coming. Stream
  and memory modes (R11) differ only in the byte source, and no layer has to suspend mid-token.

## R15 — The tokenizer lives in the package layer

- The XML tokenizer stays in the OCF/package layer (R3), internal to Bartap. The text layer, which
  already sits above the package layer, uses it as an ordinary downward dependency: text → package →
  zip.
- The tokenizer reads through a byte-source interface (an entry stream or a block of memory), never
  from zip streams directly, so memory mode (R11) needs no second path.
- It keeps its own interface, so it can be tested and fuzzed on its own.

## R16 — Block starts are records

- A block begins with a block-start record: kind, level (headings), source location (spine index and
  block offset) and starting character offset. The starting character offset is 0, except for the
  first block after a seek that lands inside a block (R49).
- The block offset of a block with no start tag (an implicit paragraph, or a restart under R46) is the
  offset of the token that triggered it: the first byte of the text token holding the first
  non-whitespace character (its `<` for CDATA), or the `<` of an image or `br`. Not the character's own
  byte, which can sit inside CDATA or an entity where parsing cannot resume (R48); the whitespace
  before it collapses away on replay, so the output is the same.

## R17 — A text stream covers one spine item

- A text stream reads one spine item (one XHTML content document, one archive entry) from its start
  to `END`. "Chapter" elsewhere in these notes means this unit; a book that is one large XHTML file is
  one stream.
- Moving across spine items, and the policy for non-linear items, is the caller's. Continuous
  reading can be built on top by the caller.
- Rationale: one entry, one CRC-32, one replay scope; a corrupt item fails on its own.

## R18 — Block ends are implicit

- Blocks are flat and have no end record. A block ends when the next call returns a block start, a
  container start or end, or `END`.
- With one record per call, layout learns that a block has finished one call later, which costs
  nothing; it finishes the block's last line then.
- (Superseded: blocks cut across pitchers with continue records, and back-patched lengths and
  continues flags. Dropped with pitchers, R20.)

## R19 — Byte order of stored fields

- Records are structs (R20), so they have no byte encoding. The only Bartap-defined bytes that are
  stored are bookmarks (R12).
- Every multi-byte bookmark field is little-endian and written and read byte by byte: no struct
  overlays, no unaligned accesses, correct on big-endian hosts.

## R20 — One record per call, as a struct (pitchers dropped)

- Each text or TOC call returns exactly one record, filled into a caller-declared `bartap_record_t`
  (a tagged union, declared by its real type, R28).
- Superseded design: a **pitcher**, a caller buffer packed with as many byte-encoded
  `[type][len][payload]` records as fit, with pitcher-start records, blocks cut across pitchers,
  back-patched lengths and flags, and a minimum buffer size.
- Rationale: the byte encoding existed only to pack many records into one buffer. Nothing stores
  records (R1) or sends them anywhere, so a struct removes the encoding and decoding, byte-order rules
  and minimum-buffer arithmetic, and lets the compiler check every field access. Block ends and
  context after a seek become simpler too (R18, R49).
- Tests compare record sequences field by field, or through a canonical serialization in the test
  harness (test code, not library code).

## R21 — Control characters are dropped from text

- C0 controls and DEL (U+007F), and C1 controls (U+0080–U+009F) arriving through UTF-8 or character
  references, are removed from text, with a warning. Tab, LF and CR are handled by
  whitespace collapsing (R4) and line-break records in `pre`.
- With the Windows-1252 fallback, raw bytes 0x80–0x9F decode to punctuation, never to C1 controls.
- Character references to 0x80–0x9F are read as Windows-1252 too, as HTML reads them: `&#150;` is
  an en dash in books converted from old Windows text. Only the five values Windows-1252 leaves
  undefined remain C1 controls, and are dropped.

## R22 — Entry names come from the central directory

- Each spine item keeps its path's hash and length (used for matching during open) and the offset
  of its central-directory record, besides its local header offset, sizes, CRC-32 and method.
- Whenever the real name is needed after open (verifying a TOC target that matched by hash, an
  image base directory), it is read from the central-directory record, never from the local header.
- Rationale: the central directory is the single source of truth for every entry's name, sizes and
  CRC; a crafted local header cannot disagree with it unnoticed. Costs 4 bytes per spine item.
- Hash collisions during the one matching walk at open are accepted: both hash and length must match.

## R23 — Three directory walks at open

- Opening walks the central directory at most three times: to find `container.xml`, to find the OPF,
  and one full walk that matches the spine items and the nav, NCX and cover paths together (all
  hashed like spine paths, their directory-record offsets recorded per R22).
- The first two walks stop at their match. Guessing the OPF during the first walk was rejected:
  more code for little gain.
- In hashed mode (R24) all three read the index instead, and only the records that match are read
  from the card. Building the index is the one full walk of the card.

## R24 — Optional directory buffer, kept with the book

- The caller may pass a buffer at open. The book keeps a pointer to it, and the caller keeps it
  alive until the book is finished with (the same rule as the spine array). Passing none is always
  valid.
- The central directory's size is known from the end record, so the mode is chosen up front:
  1. **Raw**: the whole central directory fits. It is read in one go, and every walk and lookup
     (at open and later) runs over it in RAM.
  2. **Hashed**: it does not fit, but an index of it does: 10 bytes per entry (name hash, name
     length and directory-record offset, little-endian per R19), in directory order. One walk of
     the card at open builds it. A lookup by name scans the index and confirms a candidate by
     reading its name from the directory record (R22); the first match still wins (R44). A lookup
     by hash alone, as compact spine items make (R41), reads only the matching record. Walks still
     read the card.
  3. **None**: not even the index fits. The buffer is ignored and every walk reads the card.
- A partial directory or index is never used. A bad record while building the index leaves none
  mode, so the walks that reach that record fail as they would have without a buffer.
- Hashed mode matters for lookups after open: in none mode each image record (R9) and resource
  opened by name walks the directory on the card, about half of it on average; hashed reads one
  record. Code cost about 0.5 KB.
- Testing: every lookup has three paths that must give identical results. Every fixture is opened
  in each mode and the results compared, and the fuzzers run in each.

## R25 — Metadata copied at open

- Title, creator, language and unique identifier are copied into the book at open and live as long
  as the book: about 512 bytes. Kept as the robust choice, since any of them may be what an
  implementation needs.
- Each is up to 127 bytes, cut at a UTF-8 character boundary, with a truncation warning.
- The identifier is the one `<package unique-identifier>` names; if that names none, the first
  non-empty `dc:identifier` stands in, with a warning.
- Of several `dc:creator` entries, the first is kept unchanged, and the book also holds a creator
  count (1 byte, saturating at 255). Formatting the rest ("et al.", "+2", a localized phrase) is the
  caller's; Bartap writes no display strings. Tavernkeep appends "et al.".

## R26 — The TOC uses the same records

- The TOC iterator returns one record per call, like the text layer: one output structure across
  the library.
- Each entry starts with a TOC-entry record carrying its depth and target (spine index, and the
  fragment `id` as a pointer and length valid until the next call; it points into the tokenizer's
  bounded attribute storage, R33, so it needs no extra memory). Its label follows as style and text
  records. Labels are not truncated: a long label is several text records (R6).
- NCX places `<navLabel>` before `<content src>`, the reverse of this order. On a `navPoint`, the
  iterator notes the label's offset, scans to `content`, returns the entry, then re-reads the label
  from the inflate window: at most 32 KiB back, with no card reads and no second CRC fold. A longer
  label rewinds the entry from its start, which is correct but slow. A `navPoint` rewinds at most once,
  so the loop stays bounded (R29).
- An entry whose target cannot be resolved is still returned, with spine index `BARTAP_NO_SPINE` and a
  warning. Only `nav` with `toc` among its `epub:type` tokens is read.
- An entry with no target at all differs by format:
  - In nav, a `<span>` label is a valid heading over its children, so it is returned with
    `BARTAP_NO_SPINE` and no warning.
  - In NCX, `<content>` is required. A `navPoint` without one is skipped, with a TOC-target warning.
    Its children keep their depth, so they appear one level below a missing parent.
  - Nav and NCX versions of a valid book therefore still give identical entries.

## R27 — Every inflating call takes its window explicitly

- `bartap_open` takes the 32 KiB inflate window as its own argument, separate from its work struct,
  like every other call that inflates. The work struct holds only opening's parsing state (about
  3.5 KB, still too big for a small stack, so still caller-owned).
- The window is always required, even for stored entries, so a caller's memory budget does not depend
  on a book's contents.
- Rule: a window belongs to the object it was passed to until the caller is done with that object.
  Sharing one window between opening and reading is visible at the caller's call sites.
- Rationale: the window is the one buffer callers most want to place deliberately (fast SRAM); old
  D12's "one buffer to place" hid it inside the work struct.

## R28 — Public struct layouts (old D9 kept)

- `bartap.h` defines every state struct in full; the caller declares objects by their real type
  and `sizeof` is the size. Fields are Bartap's: callers declare them and read nothing from them.
- Rejected: opaque byte arrays cast to internal structs (undefined behaviour under C11 6.5p7), and
  incomplete types with a runtime size query (no static declaration without a heap or `alloca`).
- Cost accepted: internal layouts are visible, and an internal change changes the ABI. Bartap is
  compiled with its caller, so this matters little.

## R29 — All input is untrusted, bookmarks included (old D6 kept)

- Every byte of an EPUB is attacker-controlled. Bartap guarantees no out-of-bounds reads or writes;
  every loop bounded by the input's size or a fixed limit; fixed limits on everything kept, with
  warnings when hit; every offset and length checked before use; errors as result codes, never
  undefined behaviour or aborts.
- This covers the values the new design later dereferences: directory-record offsets (R9, R22) and
  the directory buffer (R24).
- A bookmark passed back by the caller (R12) is untrusted input too: a corrupted or edited bookmark
  file can hold any value, and a crafted one can carry a valid bookmark CRC-32. Seeking range-checks
  every field and falls back to the chapter start on anything invalid, as on a chapter CRC mismatch.
  The exceptions are the cases with no chapter to land in, which R12 refuses with an error.

## R30 — Lenient only (old D19 kept)

- One mode. Packaging faults (mimetype missing, compressed or not first), malformed markup and
  invalid UTF-8 are recovered from and reported as warnings; only what makes the book unreadable is
  an error. Invalid UTF-8 is read as Windows-1252. The R29 safety checks always apply.
- No strict mode: validation is a different tool (EPUBCheck). A caller wanting strictness rejects
  books on warnings.
- Named entities: XML's five plus every entity of the XHTML 1 DTDs (the HTML 4 set, 253 names in
  all, about 3 KB of flash), which EPUB 2 content may use. Unknown ones, HTML 5's additions
  included, pass through literally, with a warning. Changing the table bumps the bookmark version
  (R12) once a release has issued bookmarks; before 1.0 none exist, so it stays at 1.
- A declaration is skipped to its `>`, past quoted strings, an internal subset in brackets and the
  comments inside it. Markup that is nothing recognised (`</` with no name, `<!-`, `<![` that is not
  CDATA) is skipped to its first `>`, as HTML skips a bogus comment: quotes mean nothing there.
- A UTF-8 BOM is skipped. XML encoding declarations are ignored, since the Windows-1252 fallback
  covers Latin-1. A document with a UTF-16 BOM cannot be read: that item returns `ERR_UNSUPPORTED`.
- R13 (unresolved TOC links) and R21 (dropped control characters) follow this stance.

## R31 — CRC-32 checks, plus a verify call (old D22 kept)

- Every entry read through a stream is checked against its central-directory CRC-32 as bytes pass;
  at the entry's end a stream returns `END` only if CRC and size match, else a corruption error.
- Gap: in stream mode a chapter that is never read to its end is never checked, and rewinding
  restarts the check.
- Bartap exposes a verify call that inflates an entry to its end only to check it. The firmware
  chooses when (or whether) to pay for it, e.g. while a page is on screen; calling it before
  showing a chapter gives a check-before-display guarantee.
- Bytes and text delivered before a check completes are unverified until then.
- The verify call takes its own entry-stream state and window from the caller (R27), so it never
  moves a reader's position:
  - with about 34 KB to spare (e.g. a stream-mode device with free SRAM, such as Tome's flash
    variant in reader mode), it can run alongside an open reader, while a page is on screen;
  - without it, the firmware verifies first and then reuses the same window for the reader once
    verification has finished, which is check-before-display at the cost of one extra inflate.
- With whole chapters inflated into PSRAM (memory mode), every chapter is already checked before
  display, and the verify call is not needed.

## R32 — Warnings: sticky bits plus an optional callback

- Each book, text and TOC object accumulates warnings as a bitmask, one bit per kind, read with a
  getter: "did anything of this kind happen".
- The caller may also pass a warning callback, called at each occurrence with the code and context
  (spine index, offset). It must return quickly and must not call into Bartap. NULL is always
  valid and costs nothing.
- Warnings are not content, so they are not records; records carry only content.

## R33 — Limits: only output-neutral ones are configurable

- Rule: a limit may be configurable at build time only if changing it cannot change Bartap's
  output. Anything that can move block boundaries is fixed, because bookmark offsets (R12) depend
  on them and the bookmark version cannot detect a build with different limits.
- Configurable (build-time, defined for the library and its callers alike):
  1. **Compressed input buffer** (default 512 B): bytes read from the card per inflate refill.
  2. **XML read-ahead** (default 256 B): tokenizer lookahead. The tokenizer must be written so its
     tokens do not depend on this size; tested by running the fuzzers at two sizes and comparing
     output.
- Fixed: metadata fields (127 B, R25), resolved path (255 B; OCF caps each file name at 255 bytes
  but whole paths at 65,535, so a deep valid path can be refused, accepted as realistic paths are
  far shorter), attribute buffer (1 KiB, also bounds
  fragment ids), formatting stack depth (32). The first three change only what is truncated or
  warned about; depth changes block structure and must never vary between builds.
- Gone with the redesign: TOC label limit (R26), word limit (text is records), anchor size (R12),
  minimum pitcher size (R50).
- Values reviewed and kept as they are. Depth stays at 32 because it can never be raised later.
  The compressed-input default stays 512 B (one SD sector); Tavernkeep measures 512 B against 2 and
  4 KiB on hardware before choosing its own.

## R34 — C conventions (old D7 kept, runtime calls narrowed)

- ISO C11 without extensions. Headers: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<string.h>`.
- Raw input bytes are `uint8_t`; decoded strings are `char`. No object accessed through an lvalue
  of another type (C11 6.5p7).
- Allowed runtime calls: `memcpy`, `memset`, `memcmp`, `memmove` only. No `str*` functions:
  lengths are always known, so nothing scans for a terminator. Enforced by the symbol check.
- Targets: Arm Cortex-M first; Xtensa and RISC-V possible; x86 is the host test platform only.
  Big-endian needs only a test configuration (R19).

## R35 — Guiding principle: minimal dependencies

- Bartap depends on nothing but four `mem*` functions. Its zip reader, inflater and XML tokenizer
  are its own, written for this design, rather than existing microcontroller libraries.
- This is the project's distinguishing feature as a portfolio piece, and it is chosen knowingly:
  existing libraries are likely more mature and faster. The robustness claim therefore has to be
  earned with evidence (fuzzing, differential testing against reference implementations).

## R36 — Testing the rewrite

- **Reference implementations per layer:** the inflater against zlib, zip directory handling
  against Python's `zipfile`, under the fuzzers and the generated fixtures. This is the evidence R35
  needs.
- **Properties, the core of the suite:** checked across all fixtures and fuzz inputs, e.g.
  - seeks served from a checkpoint and from a full replay give identical record sequences (R48 P1);
  - a seek's output matches a straight read after normalising (R48 P2);
  - every text-buffer size from the minimum up gives the same content once consecutive text records
    are merged (R6, R50);
  - every directory mode and spine layout gives identical results (R24, R41);
  - a bookmark round trip lands on the same block and character offset (R12);
  - XML read-ahead and compressed-input sizes do not change the output (R33);
  - a corrupted bookmark is refused or lands at the chapter start, never out of bounds (R29).
  - each bookmark fallback level (R12) is taken exactly when its check fails: failed bookmark CRC-32
    and wrong book are refused, changed chapter lands at its start, other version lands at the block
    start.
- The pre-rewrite code is not used as a reference: it was never validated on hardware, so agreeing
  with it proves little.

## R37 — One read callback (old D4 kept)

- All I/O goes through `read(user, offset, buf, len)` (0 on success) plus the archive's size. Reads
  are all-or-nothing; Bartap never asks for bytes past the size. A memory source is included.
- Any callback failure is one I/O error; the adapter logs the platform's cause.

## R38 — Synchronous calls, verify incremental (old D5 kept)

- Opening a book and finding things in it run to completion. Text and TOC streams return one record
  per call; resource streams one buffer per call. Real-time work belongs on another core or behind
  DMA; on a single core, the read callback is the hook for it.
- Exception: verify (R31) is incremental. Each call does a bounded amount of work and reports
  whether more remains; the verify object keeps its position between calls, like a stream.
- Rationale: idle verification alongside reading must not block a page turn. Firmware advances it
  a step at a time between reader calls and stops at any input. A one-shot verify would only make
  sense before reading starts, and that case is already served by verifying first and reusing the
  window (R31), so the separate verify buffer is only worth having if verify is incremental.

## R39 — Inflate tables in the state; fast lookup table deferred (old D11 revised)

- Huffman decode tables live inside the inflate state; the window is the caller's (R27).
- The optional fast literal/length lookup table is left out of the rewrite. It is output-neutral,
  so it can be added later as a configurable option (R33) if profiling on the device shows inflate,
  not card reads, is the bottleneck.

## R40 — 32-bit offsets, ZIP64 where it fits (old D13 kept)

- Archive offsets and sizes are `uint32_t`. ZIP64 records are read where their values fit in 32
  bits; a larger value is unsupported. Consistent with the 4-byte offsets in bookmarks (R12),
  directory-record offsets (R22) and image records (R9).

## R41 — Caller's spine array, with truncation reported (old D14 revised)

- The caller supplies an array of spine slots and its capacity, which must outlive the book. A full
  item (28 bytes on the Cortex-M33) holds its path hash and length, local header and
  directory-record offsets, sizes, CRC-32, method and flags, so a chapter opens with no lookup.
- If the book has more spine items than the array holds as compact items (below), the first ones
  are kept, a truncation warning is set, and later chapters are unreachable.
- The required item count is always reported (the first OPF pass counts itemrefs before the directory
  walk), so a caller with spare memory can reopen with a larger array.
- **Compact** mode, for spines longer than the array: each slot holds three 8-byte compact items
  (path hash, length, flags) instead of one full item of 28 bytes. The slot is a union of the full
  item's fields and the compact array, declared by its real type (R28), so the caller's code doesn't
  change.
  - The OPF's first pass always writes compact items, since how many there are is only known at the
    end of it. If they fit full slots, they are spread into them, last first, so none is overwritten
    before it is moved; otherwise they stay compact, up to three times the capacity.
  - The open walk notes which compact items matched an entry. Opening one looks its entry up by
    path hash and length, the rule the walk matched it by (R22), so it finds the entry a full item
    would have held. That lookup is one record read in hashed mode (R24), a walk otherwise.
  - A text object keeps its chapter's CRC-32 from open, so seeks and TOC resolution need no lookup.
    `bartap_bookmark_write` looks the entry up, and refuses an item with no entry.
  - Truncation, and its warning, starts past three items per slot.
- A rolling spine (refilling a window by re-parsing the OPF) was rejected: OPF re-parses, TOC
  targets outside the window, and refill logic, for books no compact spine fails to hold.

## R42 — One result enum (old D18 kept)

- Every call returns one `bartap_err_t`. Non-error statuses: `OK`, `END` (a stream or iterator is
  finished), and verify's "more to do" (R38). Errors include I/O, corrupt, unsupported (R40),
  argument, buffer too small, and wrong book (R12). Warnings are separate (R32).
- `bartap_strerror` returns code names for logs (e.g. "BARTAP_ERR_CORRUPT"), not user-facing text;
  firmware maps codes to its own messages (consistent with R25: no display strings).
- Values are not stable before a release; after 1.0 they are append-only.

## R43 — Names compared ASCII case-insensitively (old D20 revised)

- Entry names and resolved hrefs are compared byte for byte with ASCII letters folded to lower
  case, always. All name hashes (spine items, R22's lookups, and any later index) use the same folding.
- The name hash is 32-bit FNV-1a over the folded bytes, also used for the book hash (R12). It is
  frozen, because bookmarks store it.
  No case-mismatch warning: it is how lookup works, not a fault.
- Spec basis: EPUB OCF requires file names in a directory to be unique after Unicode case
  normalization, which implies uniqueness under ASCII-only folding, so a conforming book can never
  be matched to the wrong entry. Non-conforming case-only duplicates fall under R44.
- Hrefs are percent-decoded once; `%2F` in a path is refused (RFC 3986: data, not a separator). No
  Unicode normalization (it would need tables, against R35); OCF only recommends NFC/NFD
  uniqueness.

## R44 — Duplicate names: first in the directory wins (old D21 kept)

- When several entries match a name (identical, or differing only in case under R43), the first in
  central-directory order wins.
- Every directory mode (R24) gives the same answer, as does any index added later (it must keep
  directory order and return the earliest hit). Part of R36's identical-results property.
- The full spine walk (R23) sets a duplicate-name warning when a second entry hits an already
  resolved spine slot. Single lookups stop at their first match and do not look for duplicates.

## R45 — Containers as start and end records

- Blocks are flat (R18). Nesting (quotes, lists, list items) is carried by container-start and
  container-end records, emitted from the text layer's frame stack as it pushes and pops, so they
  balance by construction even on broken markup.
- Layout keeps a container stack across calls (at most the frame depth, R33). After a seek, layout
  clears its stack and the text layer first emits start records for every container open at the
  target (R49).
- Siblings stay distinct (an end, then a new start).
- Every container-start record has the same fields: kind and ordinal. The ordinal is a signed
  32-bit value (HTML allows `start="-3"`, and `reversed` lists count past zero; out-of-range markup
  values are clamped with a warning). It is a list item's number, computed by the text layer
  (honouring `start` and `value`; `reversed` is honoured when the list has a `start`), so layout never
  counts and a seek into a list re-emits the right number; it is 0 for other kinds.
- Lists (`ol`, `ul`) and list items (`li`) are containers, not blocks: one item can hold several
  paragraphs.
- List counters use saturating arithmetic, since signed overflow is undefined (R29). An `li` numbers
  from its nearest list frame, and is 0 outside any list.
- `reversed` without `start` would number the first item with the list's item count, which a
  streaming text layer does not know until the list ends. Such a list counts up from 1 instead, with
  a warning. Rare in books; a pre-scan of the list was rejected as not worth it.
- Rejected: a container path on every block-start record (needs container identities and adds
  information to every block for no benefit in sequential reading or seeking). Cost accepted:
  records cannot be laid out out of order without the container stack from before them.

## R46 — Element classes (old D17 revised)

| Class | Elements | Output |
|---|---|---|
| Container | `blockquote`, `ol`, `ul`, `li`, `dl`, `dd`, footnote `aside` (kind `note`) | Container start/end records (R45) |
| Block | `p`, `h1`–`h6` (with level), `pre`, `hr`, `dt` (its own kind) | Block-start record |
| Inline style | italic: `i` `em` `cite`; bold: `b` `strong`; monospace: `code` `kbd` `samp` `tt`; `sub`; `sup`; underline: `u` `ins`; strikethrough: `s` `del` `strike` | Style records (R7) |
| Boundary | `div`, `section`, `article`, `aside` (not a note), `header`, `footer`, `nav`, `figure`, `address`, `body`, `table`, `tr`, `td`, `th`, `caption` | No record; closes an implicit paragraph so words never fuse |
| Skipped | `head`, `style`, `script`, `title`, SVG `title`/`desc` | Content dropped |
| Image | `img`, SVG `image` | Image record (R9), inline where it sits (R5) |

- Text outside any block gets an implicit paragraph.
- Tables are flattened: each cell's text becomes its own paragraph. Grid containers can be added
  later as new kinds (layout ignores unknown kinds, R6).
- `div` is a boundary, not a block: books that use `div` for paragraphs get implicit paragraphs, and
  blocks never nest.
- Footnotes: an `aside` whose `epub:type` is `footnote`, `endnote` or `rearnote` is a `note`
  container, so firmware decides whether to show, skip or collect notes. The tokenizer keeps
  `epub:type` for this, within its bounded attribute storage.
- `ins`/`del` are HTML's inserted/deleted-text markup; they map to underline/strikethrough as
  browsers draw them by default.
- Tags match on their ASCII-folded local name (`html:p` is `p`). `epub:type` matches per token.
- Elements not in the table (`span`, `a`, `small`, `svg` and so on) are transparent: their content
  passes through, with no frame and no warning, and their close tags are ignored silently. Their `id`
  is still seen for R13.
- `hr`, `br` and `img` take no frame, whether or not they self-close, so an unclosed one cannot swallow
  what follows.
- A boundary or container inside an explicit block ends the output block but keeps its frame, and
  later content restarts a block of the same kind and level. So `<p>a<div>b</div>c</p>` gives three
  `p` blocks, words never fuse, and `</p>` matches its frame normally:
  - New content looks down the frames for the nearest block frame, or opens an implicit paragraph if
    there is none. The search stops at a container, so content inside one never restarts a block from
    outside it; the outer kind resumes after the container closes.
  - A boundary only ends a block that already has content, so `<h2><div>A</div></h2>` is one `h2`.
  - Inline frames stay on the stack, so style carries across the restart.
  - A boundary ignored past the depth limit (R47) has no frame, but its open and close tags both still
    end the live block.
  - Cost accepted: records do not distinguish a restart from a new paragraph, so layout indents each
    one if it indents paragraphs.

## R47 — Recovery from broken markup

1. A block opened directly inside a live block pops that block's frame (blocks are flat). No warning.
   A surplus close tag that results falls under rule 3. A block opened after a boundary has already
   ended the output block leaves that frame in place (R46).
2. A close tag matching an open frame further down unwinds to it, closing everything above in order
   (container-end records balanced by construction, R45), with a warning.
3. A close tag matching nothing open is ignored, with a warning.
4. At the end of the chapter everything still open is closed; a warning if any container or explicit
   block was never closed.
5. Recognised elements nested past the depth limit (R33) are ignored with a warning; their text stays
   in the enclosing block and style, and a per-tag count makes their own close tags ignored too.

## R48 — Rolling buffer with two checkpoints (replaces old D16's tail buffer)

- A tail buffer holds only an entry's last bytes, so it does not help a flip back in the middle of a
  large single-file book. It is replaced by a rolling buffer.
- In stream mode, a caller-provided ring buffer holds the most recent N bytes of inflated XHTML the
  stream has delivered.
- A checkpoint is a snapshot of the parser state (frame stack, style, whitespace state, tokenizer
  position) taken at a block start inside the buffered range.
- Two rolling checkpoints: one is taken at the first block start after every N/2 bytes, and the
  latest two are kept. When the older one's bytes are evicted, the newer becomes the older. At least
  about N/2 bytes of history behind the read position are always seekable without the card.
- A seek to a target between a checkpoint and the read position restores the checkpoint and replays
  from the ring: no card reads, no inflate. Earlier targets replay from the chapter start.
- The checkpoints live in a separate optional object, `bartap_rolling_t`, declared by the caller by
  its real type (R28): two checkpoints, a pointer to the ring bytes and the ring's size. It is passed
  when a text stream is opened, or NULL; without it a text iterator costs nothing extra (old D16's
  saved state grew every iterator).
- Memory mode uses the same object with no ring: the chapter's bytes are already in RAM, so
  checkpoints let a seek skip re-parsing everything before them. A NULL ring pointer with a non-zero
  size is a virtual window: the same offset logic, with no bytes stored.
- A seek to a target ahead of the read position continues from the live position. Otherwise it uses
  the latest usable checkpoint, then the chapter start.
- Invariants:
  1. Checkpoints are taken only at block starts, just after the start tag: no text run, partial
     entity, partial UTF-8 sequence or CDATA is pending there.
  2. A checkpoint stores two source offsets, not the tokenizer's read-ahead: the block offset (the
     tag's `<`, the value bookmarks hold) and the resume offset (the first byte the snapshot has not
     consumed, just past the tag). Restoring resets the read-ahead and resumes at the resume offset,
     so the tag is not processed twice. For a block with no start tag (R16) the two are equal, and
     the triggering token is processed after the restore.
  3. After a seek into the ring, reading comes from the ring up to its newest byte, then switches to
     the live stream, which has not moved. Bytes re-read from the ring are never appended to it again.
  4. A seek before both checkpoints rewinds the stream and replays from the chapter start; the ring
     refills and checkpoints restart from that replay.
  5. Bytes are folded into the CRC-32 once, when the stream first delivers them; replays from the
     ring do not re-fold them (R31 verifies the entry as a whole).
- A seek's first records are as R49 describes; no special record kind for resuming mid-block.
- Properties (R36):
  - P1: the same target and text-buffer size give identical record sequences whether the seek is
    served from a checkpoint and the ring, or from a full replay from the chapter start.
  - P2: from the target onward, a seek's output matches a straight read of the chapter after
    normalising: consecutive text records merged, the restated context at the seek dropped, and the
    first block's starting character offset ignored.

## R49 — Records after a seek

- Layout knows it has seeked (it made the call), so it clears its container stack and style itself;
  no flag is needed.
- The text layer's first records after a seek, or after opening at a bookmark: container starts for
  every container open at the target (R45), a block-start record whose starting character offset is
  the bookmark's (0 when landing at a block start), a style record (R8), then text from the target.
- Landing at a character offset lands right after that many text bytes (R10), so an image or line
  break at exactly that position is emitted, and earlier ones are not.
- Replay copies no text and looks up no images, so a seek past N images does not cost N directory
  walks (R9). The warning callback (R32) stays silent below the highest offset already delivered.
- (Superseded: the pitcher-start record with its length and context-reset and chapter-finished
  flags. The next call returning `END` replaces "chapter finished".)

## R50 — Minimum text buffer

- The caller's text buffer must hold at least one full UTF-8 character: `BARTAP_TEXT_MIN` is 4 bytes.
  Larger buffers just mean fewer, longer text records. A smaller one is a "buffer too small" error
  (R42).
- TOC fragments do not use the text buffer (R26), so nothing else sets a minimum.
- (Superseded: `BARTAP_PITCHER_MIN`, about 1.05 KB, set by the largest unsplittable byte record.)

## R51 — Hardware validation before the rewrite counts as done

On the Pico 2 (SD breakout, output over RTT; no panels needed), run from Tavernkeep, which imports
Bartap and has the working SD driver and FatFs. This repository supplies the canonical serialization
and the expected host hashes:

- **Correctness:** for every fixture, a hash of all records produced on the device (through the test
  harness's canonical serialization) equals the host suite's hash. Exact match required.
- **Timings:**
  - opening Pride and Prejudice (65 items), a large single-file book and an image-heavy book, in
    each directory mode (R24), with read counts — target: under 2 s for Pride and Prejudice;
  - producing one page's worth of records (an ordinary page turn), stream and memory mode — target:
    under 50 ms;
  - seeking back within the ring (from a checkpoint) and outside it (full replay), on the large
    single-file book — target within the ring: under 100 ms;
  - inflate throughput alone, and card read throughput with the compressed-input buffer at 512 B,
    2 KiB and 4 KiB (settles Tavernkeep's choice, R33);
  - one verify step (R38), to choose a step size that keeps page turns responsive.
- **Memory:**
  - stack high-water marks for opening, text, TOC and verify, measured by stack painting, not
    estimated;
  - `sizeof` of every state struct on Arm, against Tavernkeep's reader-mode RAM budget.
- Targets are starting points, set against e-ink refresh times of hundreds of milliseconds to a few
  seconds; revise them with measurements.

## Open

(Nothing open.)

## Implementation plan: remaining phases

Phases 0 (reset, these amendments) and 1 (results, warnings, read gate, memory source, CRC-32, name
hash, records, test-side canonical serialization) are done. Each phase needs its own approval, and ends
with its tests passing under ASan and UBSan and any fuzz target it calls for added.

### Layout

```
include/bartap.h  public API, every state struct in full (R28), build-time limits (R33)
src/internal.h    interfaces between layers, including the tokenizer's (R15)
src/zip.c         layer 1 (R3 zip): CRC-32, name hash, inflate, directory, entry streams, verify; no XML
src/xml.c         layer 2 (R3 OCF/package): byte source, UTF-8/CP1252, entities, tokenizer
src/package.c     layer 2 (R3 OCF/package): container.xml, OPF, spine, metadata, href resolver,
                  bartap_open
src/text.c        layer 3 (R3 text): text records, TOC iterator, bookmarks, seek, rolling buffer
tests/            fixtures/ (gen.py, dump.c, check.py, seek.c), unit/, diff/, prop/, fuzz/, arm/,
                  bigendian/
```

Each layer depends only on the one below. The TOC iterator sits in `text.c` even though R3 lists
navigation under the package layer, because R26 has it emit style and text records; the package layer
still locates the nav and NCX entries.

### Key mechanisms

- **Byte source stack (R14, R15):** tokenizer, then source, then optional ring tap, then entry stream
  (stored or deflate), then `read()`. Memory mode replaces everything below the source with a pointer
  and length.
- **Inflate:** decodes into the caller's window and stops when fewer than 258 bytes of space remain, so
  a match never suspends. Output beyond the directory's size is corrupt, which bounds zip bombs. `END`
  requires a CRC and size match (R31), so text streams drain any bytes after `</html>`.
- **One record per call (R20):** a small emission state machine covers the moments that produce several
  records: unwinding container ends, block start plus style (R8), and the context after a seek (R49).
- **Opening (R23, R41):**
  1. Walk the directory for `container.xml`, then again for the OPF.
  2. OPF pass 1: metadata, itemref count, idref hashes written as compact items (R41).
  3. OPF pass 2: manifest entries replace the idref hashes with resolved path hash and length, and
     capture nav, NCX and cover. Two passes are needed because the manifest precedes the spine.
     If the items fit full slots, they are spread into them; otherwise the spine stays compact.
  4. One full directory walk fills the full slots, or marks which compact items were found. A spine
     item missing from the archive gets a warning and fails only when opened.

The whole API is in place (phases 2 to 9 below); `include/bartap.h` is its reference. Phase 10 is
done except the real-book corpus and a sustained fuzz campaign (runs so far are minutes long, and
clean).

### Phases

| Phase | Scope | Done when |
|---|---|---|
| 2. Inflate and entry streams | Stored and deflate, CRC and size checks, verify's bounded step (R31, R38) | Byte-identical with zlib on generated and fuzzed streams, including every block type and invalid tables |
| 3. Zip directory | End record search, ZIP64 where it fits (R40), directory cursor in both modes (R24), first match wins and duplicate warning (R44), ASCII-folded matching (R43) | Agrees with Python `zipfile` on fixtures; raw and none give identical results |
| 4. XML tokenizer | Tags, attributes in 1 KiB storage, entities, CDATA, comments, PI, DOCTYPE, UTF-8 with Windows-1252 fallback (R30), control characters (R21), byte offsets on every token | Tokens do not change across read-ahead sizes (R33); fuzzed standalone |
| 5. Package | Full `bartap_open`: metadata limits (R25), href resolution with percent-decoding and the `%2F` refusal (R43), spine truncation and required count (R41), resource streams, lenient packaging (R30) | Fixtures for each fault; read counts confirm at most three directory walks |
| 6. Text, sequential | Element classes (R46), whitespace (R4), records (R6, R7, R8), containers and ordinals (R45), recovery (R47), image lookup (R9), buffer splitting (R50) | Hand-written expected record sequences for every class and recovery rule; identical at every text-buffer size once merged |
| 7. TOC | Nav and NCX with the label rewind (R26), target resolution, `bartap_toc_resolve` (R13) | NCX and nav versions of a book give identical entries; a long NCX label arrives whole |
| 8. Bookmarks and seek by full replay | Byte-by-byte serialization (R19), every resolution level (R12), records after a seek, landing and replay rules (R49) | Round trip lands exactly; each fallback triggers only when its own check fails; P2 holds (R48) |
| 9. Rolling buffer | Ring tap, two checkpoints, all five invariants, virtual window for memory mode (R48) | P1 holds, with zero `read()` calls for seeks served from the ring, asserted by a counting callback |
| 10. Evidence | Full property matrix over fixtures and corpus, sustained fuzz campaign with findings fixed, big-endian run, symbol check, `sizeof` report, README and API docs | Everything below passes except the R51 row |
| 11. Hardware handoff (R51) | Run from Tavernkeep. This repository supplies the importable library, the canonical serialization and hash as portable C, the expected host hash per fixture and corpus book, and a `tests/README` listing R51's measurements and targets | R51 passes on the device; the rewrite counts as done only then. Results that change a default or target come back as updates to this document |

### Verification

| Property | How it is checked |
|---|---|
| Inflate matches zlib; directory matches `zipfile` (R36) | `tests/diff/`, on generated streams and archives |
| Checkpoint seek equals full replay (P1) | Landing points through every fixture item, across ring sizes and text buffers, plus the unit seek tests |
| Seek output equals a straight read after normalising (P2) | Same, plus random targets |
| Text-buffer sizes from 4 up give the same merged content (R6, R50) | Every small size, then powers of two |
| Directory modes and spine layouts agree (R24, R41, R44) | Every fixture dumped in five modes (none, raw, hashed, compact, compact hashed); `fuzz_open` and the unit tests compare them too |
| Bookmark round trip and each resolution level (R12) | Single-field and random corruption, exhaustive bit flips |
| Input-buffer and read-ahead sizes do not change output (R33) | The fixture dump built at three sizes of each, every line compared; the unit tests and fuzz replays at 1 byte |
| Balanced containers on broken markup (R45, R47) | Asserted inside the text fuzz harness |
| Only `mem*` symbols (R34, R35) | `nm` check on the host build, and on the Cortex-M33 build, where `__aeabi_*` helpers are allowed |
| No undefined behaviour, correct on big-endian (R19, R29, R34) | ASan and UBSan builds; the fixture dump built for big-endian Arm and run under `qemu-armeb` matches the host's |
| Device hash equals host hash, targets met (R51) | Run from Tavernkeep against the hashes this repository publishes |

- **Fuzz targets:** tokenizer (`fuzz_xml`); open (`fuzz_open`, which also covers the directory,
  inflate and resource streams, in every mode); text; TOC; seek with arbitrary bookmark bytes.
- **Fixtures:** `tests/fixtures/gen.py` generates deterministic EPUBs covering each record.
  A corpus fetcher, not yet written, will download public-domain books, uncommitted, including
  R51's Pride and Prejudice, a large single-file book and an image-heavy book.
- **Tools:** `cmake`, Python 3 and zlib; `clang` for libFuzzer; `arm-none-eabi-gcc` and newlib for
  the Cortex-M33 checks. The big-endian build uses the same compiler with `-mbig-endian`,
  freestanding over a minimal libc in `tests/bigendian/` since no big-endian newlib is packaged, and
  runs under `qemu-armeb`. A missing cross tool skips its test.

### Out of scope

- Deferred by these decisions: fast inflate lookup table (R39), grid containers (R46).
- Layout and rendering, the SD driver and FatFs, and the Tavernkeep side of R51 (firmware, timings,
  stack painting).
