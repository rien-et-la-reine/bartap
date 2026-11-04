# Bartap rewrite — decisions

Decisions for the manual rewrite of Bartap, settled 2026-11-04. The pre-rewrite code is `bartap` at
`ccdc61d`; its D1–D22 (`lib/bartap/DECISIONS.md`) are all open to reconsideration except R1.

## R1 — Fixed constraints

- No heap. Bartap never allocates; the caller owns all memory.
- The only thing written to the card is book state (essentially bookmarks).
- Everything else, including every old decision, is open.

## R2 — Plan

- The manual rewrite is the next step. The stream-mode seek buffer (old D16's tail buffer, now R48's rolling buffer) is implemented as part of the
  rewrite, against the updated contract, rather than retrofitted onto the old code.
- Bartap is split into its own repository after the rewrite.

## R3 — Three layers

1. **Zip** — the archive and its entries, streamed out inflated.
2. **OCF / package** — container, package and navigation; XML handling sits with this layer.
3. **Text** — sends out text blocks, in a form a layout engine consumes, into a caller-given buffer.

## R4 — Whitespace

Whitespace collapsing happens in the text layer, so every caller gets the same words.

## R5 — Images

Images may sit inline inside a block wherever the EPUB places them; Bartap does not assume they are
block-level.

## R6 — Output is a sequence of records

- Everything the text layer writes is a record: `[type][len][payload]`. Text, style, image and
  anything added later use the same format.
- `len` is 2 bytes. A text record is split only where a block is cut at the buffer's capacity, never
  for the format's sake.
- Because records are length-delimited, nothing is in-band: no marker byte is reserved in book text.
- Consequence: a consumer must treat consecutive text records as one continuous run, with no implied
  word boundary between them (buffer-edge splits, style changes mid-word).

## R7 — Style records carry the complete style

- A style record holds the whole current style, not a toggle of one attribute.
- Unknown style bits are ignored; the known bits are still applied.
- Style bits are independent and additive. A change that alters what existing bits mean bumps the
  format version instead.

## R8 — Every block part starts with its style

Every block, and every continued part of a block cut across pitchers, starts with a style record
after its start or continue record, so each part can be read without the one before it.

## R9 — Image payload

An image record carries the offset of the image entry's central-directory record (4 bytes), not its
path. (Revised from the local header offset to follow R22.)

- The text layer looks the image's path up when it emits the record. With a directory buffer
  (R24) this is a RAM walk; without one, it is a card walk per image, paid even if the caller never
  draws the image. A caller reading heavily illustrated books can avoid that by passing a buffer.
- Opening the image later starts from the directory record, the source of truth for its sizes,
  CRC-32 and local header offset.

## R10 — Bookmarks count text bytes only

Character offsets in bookmarks count text-record payload bytes only, never record headers or other
records.

## R11 — Stream mode is a first-class path

- Context: Rev F's U206 footprint takes either PSRAM or a second NOR flash; the part is chosen just
  before fab, so the firmware cannot assume PSRAM. Other users of Bartap may have none either.
- Decision: Stream mode (inflating from the card as text is read) and memory mode (reading an
  already-inflated chapter) are both fully supported, and seeking in stream mode must be acceptably
  fast. A stream-mode seek buffer therefore stays in scope (R48).
- Bartap does not know or care which part is fitted; the caller chooses the mode per chapter.

## R12 — Bookmark contents

- Fields, serialized one by one (never a struct dump):
  - version, 1 byte
  - spine index, 2 bytes
  - block offset, 4 bytes: raw XHTML offset where the block starts
  - character offset, 4 bytes: text-record payload bytes into the block (R10); 4 bytes because a
    single block can exceed 64 KiB
  - chapter CRC-32, 4 bytes, taken from the central directory
- If the chapter's CRC-32 no longer matches, the bookmark falls back to the start of that chapter.
- A bookmark does not identify its book; which book it belongs to is the caller's to record.
- 2 bytes of spine index are enough: a spine's length is bounded by the number of archive entries,
  and the caller's spine array runs out of RAM far below 65,535 items anyway.

## R13 — TOC links resolve to bookmarks

- A TOC target (`chapter.xhtml#id`) is resolved when it is followed, by streaming that chapter from
  its start until the element with that `id` is reached.
- The result is an exact landing in bookmark form (R12): the block offset of the block containing the
  element, plus the character offset of the element within that block's text. Following a TOC entry
  then uses the same seek as a bookmark.
- An `id` not found in the chapter lands at the chapter start, the same fallback as R12, and raises
  a warning so the caller can log the broken link. The warning format is not settled (see Open).

## R14 — Pull, not push

- The caller pulls the next block; the text layer pulls tokens from the tokenizer, which pulls bytes
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

## R16 — Block headers are records

- A block's header is a block-start record (kind, level, source location, starting character
  offset) in the same `[type][len][payload]` format as everything else (R6). The text layer's output
  is only ever records.

## R17 — A text stream covers one spine item

- A text stream reads one spine item (one XHTML content document, one archive entry) from its start
  to `END`. "Chapter" elsewhere in these notes means this unit; a book that is one large XHTML file is
  one stream.
- Moving across spine items, and the policy for non-linear items, is the caller's. Continuous
  reading can be built on top by the caller.
- Rationale: one entry, one CRC-32, one replay scope; a corrupt item fails on its own.
- Blocks are found by walking records; there is no directory record.

## R18 — Blocks in a pitcher: start records back-patched with length and a continues flag

- One call fills the caller's buffer with as many blocks as fit (a pitcher, R20). A block that does
  not fit is cut at the buffer's end and continues in the next call's pitcher.
- Each block's part in a pitcher starts with a block-start record, or a continue record when it
  carries on a block cut in the previous pitcher. A continue record carries the same kind, level and
  source location, plus its own starting character offset.
- Each start or continue record holds that part's byte length in the buffer and a flag saying
  whether the block continues in the next pitcher. Both are filled in by going back once the part is
  finished, before the call returns. No block-end record.
- Walking from block to block reads only start and continue records. Layout knows whether a block
  has finished when it reaches the end of its part.
- Output granularity is at least one block (or the part of one that fits) per call, so
  back-patching adds no visible latency.

## R19 — Integer widths and byte order

- The record type field is 1 byte.
- The byte length in a block-start or continue record is 4 bytes, so only the caller's buffer size
  decides where blocks are cut.
- Every multi-byte field, in records and in bookmarks (R12), is little-endian and written and read
  byte by byte: no struct overlays, no unaligned accesses, correct on big-endian hosts.

## R20 — Pitchers

- A **pitcher** is everything one text call returns in the caller's buffer: one block, part of a
  block, or several blocks with possibly a partial one at either end. The name is Bartap's own
  unit, bar-themed like the library; the documentation and header comments must define it where it
  is first used.
- A pitcher starts with a record telling the caller what it holds (R49).
- Generic words were avoided: "section" is an XHTML element (`<section>`), and "piece"/"segment"
  say nothing specific to Bartap.

## R21 — Control characters are dropped from text

- C0 controls and DEL (U+007F), and C1 controls (U+0080–U+009F) arriving through UTF-8 or character
  references, are removed from text records, with a warning. Tab, LF and CR are handled by
  whitespace collapsing (R4) and line-break records in `pre`.
- With the Windows-1252 fallback, raw bytes 0x80–0x9F decode to punctuation, never to C1 controls.

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

## R24 — Optional directory buffer, kept with the book

- The caller may pass a buffer at open. The book keeps a pointer to it, and the caller keeps it
  alive until the book is finished with (the same rule as the spine array). Passing none is always
  valid.
- The central directory's size is known from the end record, so the mode is chosen up front:
  1. **Raw**: the whole central directory fits. It is read in one go, and every walk and lookup
     (at open and later) runs over it in RAM.
  2. **None**: it does not fit. The buffer is ignored and every walk reads the card.
- A partial directory is never used.
- Testing: every lookup has two paths (raw, none) that must give identical results. Open every
  fixture in both modes and compare the results, and run the fuzzers in both.
- Deferred: a **hashed** mode for directories too large for the buffer (an index of name hash,
  length and directory-record offset, built in the first walk, every hit confirmed by reading the
  directory record per R22). It changes no format or output, so it can be added later if real books
  need it. Left out of the rewrite to keep the number of lookup paths down.

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

## R26 — The TOC uses the same record format

- The TOC iterator fills the caller's buffer with records, in pitchers, like the text layer: one
  output structure across the library, one parser for callers.
- Each TOC entry is a block of its own kind: its start record carries the depth and the target
  (spine index and fragment `id`); its label follows as style and text records, cut across pitchers
  with continue records like any block (R18). Labels are not truncated.
- Only text records are split across pitchers; every other record, including an entry's start record
  with its target, must fit whole.

## R27 — Every inflating call takes its window explicitly

- `bartap_open` takes the 32 KiB inflate window as its own argument, separate from its work struct,
  like every other call that inflates. The work struct holds only opening's parsing state (about
  5 KB, still too big for a small stack, so still caller-owned).
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
  file can hold any value. Seeking range-checks every field and falls back to the chapter start on
  anything invalid, as on a CRC mismatch.

## R30 — Lenient only (old D19 kept)

- One mode. Packaging faults (mimetype missing, compressed or not first), malformed markup and
  invalid UTF-8 are recovered from and reported as warnings; only what makes the book unreadable is
  an error. Invalid UTF-8 is read as Windows-1252. The R29 safety checks always apply.
- No strict mode: validation is a different tool (EPUBCheck). A caller wanting strictness rejects
  books on warnings.
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
- Warnings are not content, so they are outside the record format; records carry only content.

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
- Gone with the redesign: TOC label limit (R26), word limit (text is records), anchor size (R12).
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
  - seeks served from a checkpoint and from a full replay give byte-identical pitchers (R48 P1);
  - a seek's output matches a straight read after normalising pitcher boundaries (R48 P2);
  - every buffer size from the minimum up gives the same concatenated content (R18);
  - raw and no directory buffer give identical results (R24);
  - a bookmark round trip lands on the same block and character offset (R12);
  - XML read-ahead and compressed-input sizes do not change the output (R33);
  - a corrupted bookmark lands at the chapter start, never out of bounds (R29).
- The pre-rewrite code is not used as a reference: it was never validated on hardware, so agreeing
  with it proves little.

## R37 — One read callback (old D4 kept)

- All I/O goes through `read(user, offset, buf, len)` (0 on success) plus the archive's size. Reads
  are all-or-nothing; Bartap never asks for bytes past the size. A memory source is included.
- Any callback failure is one I/O error; the adapter logs the platform's cause.

## R38 — Synchronous calls, verify incremental (old D5 kept)

- Opening a book and finding things in it run to completion. Text and TOC streams return one pitcher
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

- The caller supplies an array of spine items and its capacity, which must outlive the book. Each
  item (about 32 bytes) holds its path hash and length, local header and directory-record offsets,
  sizes, CRC-32, method and flags, so a chapter opens with no lookup.
- If the book has more spine items than the array holds, the first `capacity` are kept, a truncation
  warning is set, and later chapters are unreachable.
- The required item count is always reported (the first OPF pass counts itemrefs before the directory
  walk), so a caller with spare memory can reopen with a larger array.
- Deferred: a **compact** mode for spines too long for full items (about 8-byte items holding path
  hash, length and linear flag, with each chapter's entry looked up when it is opened; slots become
  a union of one full item or several compact ones, declared by real type per R28). It changes no
  format or output, so it can be added later if real books need it.
- A rolling spine (refilling a window by re-parsing the OPF) was rejected: OPF re-parses, TOC
  targets outside the window, and refill logic, for books no compact spine fails to hold.

## R42 — One result enum (old D18 kept)

- Every call returns one `bartap_err_t`. Non-error statuses: `OK`, `END` (a stream or iterator is
  finished), and verify's "more to do" (R38). Errors include I/O, corrupt, unsupported (R40),
  argument, and buffer too small. Warnings are separate (R32).
- `bartap_strerror` returns code names for logs (e.g. "BARTAP_ERR_CORRUPT"), not user-facing text;
  firmware maps codes to its own messages (consistent with R25: no display strings).
- Values are not stable before a release; after 1.0 they are append-only.

## R43 — Names compared ASCII case-insensitively (old D20 revised)

- Entry names and resolved hrefs are compared byte for byte with ASCII letters folded to lower
  case, always. All name hashes (spine items, R22's lookups, and any later index) use the same folding.
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

- Blocks are flat (R18: no block-end record). Nesting (quotes, lists, list items) is carried by
  container-start and container-end records, emitted from the text layer's frame stack as it pushes
  and pops, so they balance by construction even on broken markup.
- Pitchers read in sequence carry container state forward: layout keeps a container stack across
  calls (at most the frame depth, R33). After a seek or opening at a bookmark, layout starts with an
  empty stack and the text layer first emits start records for every container open at that point.
- Siblings stay distinct (an end, then a new start), and a container split across pitchers is one
  container (no end between its parts).
- Every container-start record has the same payload: kind and ordinal. The ordinal is a signed
  4-byte value (HTML allows `start="-3"`, and `reversed` lists count past zero; out-of-range markup
  values are clamped with a warning). It is a list item's number, computed by the text layer (honouring `start` and `value`; `reversed` is
  honoured when the list has a `start`) so layout
  never counts and a seek into a list re-emits the right number; it is 0 for other kinds.
- Lists (`ol`, `ul`) and list items (`li`) are containers, not blocks: one item can hold several
  paragraphs.
- `reversed` without `start` would number the first item with the list's item count, which a
  streaming text layer does not know until the list ends. Such a list counts up from 1 instead, with
  a warning. Rare in books; a pre-scan of the list was rejected as not worth it.
- Rejected: a container path on every block start record (needs container identities and adds
  information to every block for no benefit in sequential reading or seeking). Cost accepted: a
  pitcher cannot be laid out alone, out of order, without the container stack from before it.

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
  later as new kinds (unknown records are skippable, R6).
- `div` is a boundary, not a block: books that use `div` for paragraphs get implicit paragraphs, and
  blocks never nest.
- Footnotes: an `aside` whose `epub:type` is `footnote`, `endnote` or `rearnote` is a `note`
  container, so firmware decides whether to show, skip or collect notes. The tokenizer keeps
  `epub:type` for this, within its bounded attribute storage.
- `ins`/`del` are HTML's inserted/deleted-text markup; they map to underline/strikethrough as
  browsers draw them by default.

## R47 — Recovery from broken markup

1. A block opened while another is open ends the current one (blocks are flat). No warning.
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
  checkpoints let a seek skip re-parsing everything before them.
- Invariants:
  1. Checkpoints are taken only at block starts, just after the start tag: no text run, partial
     entity, partial UTF-8 sequence or CDATA is pending there.
  2. A checkpoint stores the source offset of that tag, not the tokenizer's read-ahead; restoring
     resets the read-ahead and resumes at that offset.
  3. After a seek into the ring, reading comes from the ring up to its newest byte, then switches to
     the live stream, which has not moved. Bytes re-read from the ring are never appended to it again.
  4. A seek before both checkpoints rewinds the stream and replays from the chapter start; the ring
     refills and checkpoints restart from that replay.
  5. Bytes are folded into the CRC-32 once, when the stream first delivers them; replays from the
     ring do not re-fold them (R31 verifies the entry as a whole).
- A seek's first pitcher: container starts for everything open at the target (R45); a continue record
  if the bookmark's character offset is inside a block (R18), otherwise a block-start record; a style
  record (R8); then text from the target. No new record type for resuming mid-block.
- Properties (R36):
  - P1: the same target and buffer size give byte-identical pitchers whether the seek is served from a
    checkpoint and the ring, or from a full replay from the chapter start.
  - P2: from the target onward, a seek's output matches a straight read of the chapter after
    normalising pitcher boundaries (continued parts merged, restated context dropped). Pitchers
    themselves are not compared, since their boundaries depend on where the call started.

## R49 — The pitcher-start record

- Every pitcher begins with a pitcher-start record:
  - total byte length of the pitcher, 4 bytes, filled in by going back once the pitcher is finished;
  - a flags byte:
    - **context reset**: layout must clear its container stack before applying what follows. Set
      on the first pitcher after a seek or opening at a bookmark, which begins with re-emitted
      container starts (R45);
    - **chapter finished**: the chapter's last block is in this pitcher; the next call returns `END`;
    - other bits reserved; readers ignore unknown bits (as with style, R7).
- Not included: a block count (walking start records is cheap, R18) or a "begins mid-block" flag
  (the first part's record type already says so).

## R50 — Minimum buffer size

- Only text records are split across pitchers. Re-emitted container starts after a seek (R45) are
  ordinary records and may continue into following pitchers; only the first carries the context-reset
  flag (R49). Nesting depth therefore does not affect the minimum.
- One published constant, `BARTAP_PITCHER_MIN`, covers text and TOC alike: a pitcher-start record,
  the largest record that is never split (a TOC entry's start record with the longest fragment the
  tokenizer's attribute storage allows, R33), a style record, and a text record holding one full
  UTF-8 character. Roughly 1.05 KB; computed exactly once record layouts are fixed.
- A smaller buffer is a "buffer too small" error (R42).

## R51 — Hardware validation before the rewrite counts as done

On the Pico 2 (SD breakout, output over RTT; no panels needed):

- **Correctness:** for every fixture, a hash of all pitchers produced on the device equals the host
  suite's hash. Exact match required.
- **Timings:**
  - opening Pride and Prejudice (65 items), a large single-file book and an image-heavy book, in
    each directory mode (R24), with read counts — target: under 2 s for Pride and Prejudice;
  - next pitcher (an ordinary page turn), stream and memory mode — target: under 50 ms;
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
