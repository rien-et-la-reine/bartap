#!/usr/bin/env python3
"""Generates the deterministic EPUB fixtures (phase 10): together they produce every record kind,
element class, recovery rule, TOC shape and packaging fault the library handles.

The same script gives the same archive bytes for the same zlib. A different zlib (zlib-ng, say) can
deflate differently and move directory offsets, so host hashes (R51) are taken from the generated
files themselves, and the device reads those same files, never a regenerated copy.

Usage: gen.py OUTDIR [NAME...]   (all fixtures if no names; --list prints the names)
"""
import sys
import warnings
import zipfile
from pathlib import Path

# ---- Archive writing ----

# Fixed fields for every entry, so the archive does not depend on the host, the clock or umask.
DATE = (1980, 1, 1, 0, 0, 0)
STORED = zipfile.ZIP_STORED
DEFLATED = zipfile.ZIP_DEFLATED

XHTML_NS = "http://www.w3.org/1999/xhtml"
EPUB_NS = "http://www.idpf.org/2007/ops"


class Book:
    """Entries in the order they are written. Each is (name, bytes, method, force_zip64)."""

    def __init__(self):
        self.entries = []

    def add(self, name, data, method=DEFLATED, zip64=False):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.entries.append((name, data, method, zip64))

    def write(self, path):
        # a duplicate name is deliberate (R44)
        with warnings.catch_warnings(), zipfile.ZipFile(path, "w") as zf:
            warnings.simplefilter("ignore")
            for name, data, method, zip64 in self.entries:
                info = zipfile.ZipInfo(name, DATE)
                info.compress_type = method
                # zipfile picks these from the host otherwise
                info.create_system = 0
                info.external_attr = 0
                if method == DEFLATED:
                    info.compress_level = 9
                with zf.open(info, "w", force_zip64=zip64) as out:
                    out.write(data)


# ---- Documents ----

def chapter(body, title="t"):
    """A chapter document around a body snippet, as R46's element classes expect."""
    return (f'<?xml version="1.0" encoding="utf-8"?>\n'
            f'<html xmlns="{XHTML_NS}" xmlns:epub="{EPUB_NS}"><head><title>{title}</title>'
            f'<style>p {{ margin: 0 }}</style></head>\n<body>\n{body}\n</body></html>\n')


def container(opf_path):
    return ('<?xml version="1.0"?>\n'
            '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
            f'<rootfiles><rootfile full-path="{opf_path}" media-type="application/oebps-package+xml"/>'
            '</rootfiles></container>\n')


def opf(manifest, spine, metadata=None, toc_id=None, unique_id="uid", version="3.0"):
    """manifest is (id, href, media type, properties or None); spine is (idref, linear) pairs."""
    if metadata is None:
        metadata = (f'<dc:identifier id="uid">urn:bartap:fixture</dc:identifier>'
                    f'<dc:title>Fixture</dc:title><dc:language>en</dc:language>')
    items = "".join(
        f'<item id="{i}" href="{h}" media-type="{t}"' + (f' properties="{p}"' if p else "") + "/>"
        for i, h, t, p in manifest)
    refs = "".join(f'<itemref idref="{i}"' + ("" if linear else ' linear="no"') + "/>" for i, linear in spine)
    toc = f' toc="{toc_id}"' if toc_id else ""
    return (f'<?xml version="1.0"?>\n<package xmlns="http://www.idpf.org/2007/opf" version="{version}" '
            f'unique-identifier="{unique_id}"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
            f'{metadata}</metadata><manifest>{items}</manifest><spine{toc}>{refs}</spine></package>\n')


def nav(entries):
    """entries is a tree of (label markup, href or None, children)."""
    def ol(nodes):
        out = "<ol>"
        for label, href, children in nodes:
            link = f'<a href="{href}">{label}</a>' if href is not None else f"<span>{label}</span>"
            out += f"<li>{link}{ol(children) if children else ''}</li>"
        return out + "</ol>"
    return chapter(f'<nav epub:type="toc"><h1>Contents</h1>{ol(entries)}</nav>'
                   f'<nav epub:type="landmarks"><ol><li><a href="text/c0.xhtml">Start</a></li></ol></nav>',
                   title="Contents")


def ncx(entries):
    """The same tree as nav(), as an NCX. Labels are plain text there."""
    counter = [0]

    def points(nodes):
        out = ""
        for label, href, children in nodes:
            counter[0] += 1
            src = f'<content src="{href}"/>' if href is not None else ""
            out += (f'<navPoint id="n{counter[0]}" playOrder="{counter[0]}"><navLabel><text>{label}</text>'
                    f'</navLabel>{src}{points(children)}</navPoint>')
        return out
    return ('<?xml version="1.0"?>\n<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
            '<head><meta name="dtb:uid" content="urn:bartap:fixture"/></head>'
            f'<docTitle><text>Fixture</text></docTitle><navMap>{points(entries)}</navMap></ncx>\n')


# Stand-ins for images: only their names and bytes matter, never their contents (R9).
PNG = bytes.fromhex("89504e470d0a1a0a") + b"fixture image"
XHTML = "application/xhtml+xml"


def standard(book, chapters, opf_path="OEBPS/content.opf", toc=None, toc_kinds=("nav", "ncx"),
             images=("a.png",), metadata=None, linear=None, method=DEFLATED, extra_manifest=()):
    """The usual layout: chapters at OEBPS/text/c<i>.xhtml, images at OEBPS/images/, nav and NCX
    next to the OPF. Leaves out any TOC kind not in toc_kinds."""
    base = opf_path.rsplit("/", 1)[0] + "/" if "/" in opf_path else ""
    book.add("mimetype", "application/epub+zip", STORED)
    book.add("META-INF/container.xml", container(opf_path), method)
    manifest, spine = [], []
    for i, doc in enumerate(chapters):
        book.add(f"{base}text/c{i}.xhtml", doc, method)
        manifest.append((f"c{i}", f"text/c{i}.xhtml", XHTML, None))
        spine.append((f"c{i}", linear is None or linear[i]))
    for name in images:
        book.add(f"{base}images/{name}", PNG, STORED)
        manifest.append((f"img{len(manifest)}", "images/" + name.replace(" ", "%20"), "image/png", None))
    toc_id = None
    if toc is not None and "nav" in toc_kinds:
        book.add(f"{base}nav.xhtml", nav(toc), method)
        manifest.append(("nav", "nav.xhtml", XHTML, "nav"))
    if toc is not None and "ncx" in toc_kinds:
        book.add(f"{base}toc.ncx", ncx(toc), method)
        manifest.append(("ncx", "toc.ncx", "application/x-dtbncx+xml", None))
        toc_id = "ncx"
    manifest.extend(extra_manifest)
    book.add(opf_path, opf(manifest, spine, metadata, toc_id), method)


# ---- Fixtures ----

# Every element class and record kind, in a well-formed book (R6, R7, R9, R45, R46).
def fx_records():
    c0 = chapter("""
<h1 id="top">Heading one</h1><h2>Two</h2><h3>Three</h3><h4>Four</h4><h5>Five</h5><h6>Six</h6>
<p>Plain, <i>italic</i>, <em>em</em>, <cite>cite</cite>, <b>bold</b>, <strong>strong</strong>,
<code>code</code>, <kbd>kbd</kbd>, <samp>samp</samp>, <tt>tt</tt>, H<sub>2</sub>O, x<sup>2</sup>,
<u>u</u>, <ins>ins</ins>, <s>s</s>, <del>del</del>, <strike>strike</strike>,
<b><i><u>nested</u> styles</i> carried</b> back to plain.</p>
<p>Entities: &amp; &lt; &gt; &quot; &apos; &nbsp;&mdash;&hellip; &#233;&#x1F600; and <![CDATA[a <cdata> section]]>.</p>
<p>Line one<br/>line two<br/>line three</p>
<pre>  preformatted
    text   keeps   spaces
</pre>
<hr/>
<blockquote><p>Quoted.</p><blockquote><p>Nested quote.</p></blockquote></blockquote>
<ol><li>One</li><li>Two<ul><li>Bullet</li><li>Bullet</li></ul></li><li value="10">Ten</li><li>Eleven</li></ol>
<ol start="5"><li>Five</li><li>Six</li></ol>
<ol reversed="" start="3"><li>Three</li><li>Two</li><li>One</li></ol>
<dl><dt>Term</dt><dd>Definition, <i>styled</i>.</dd><dt>Another</dt><dd><p>Block in a definition.</p></dd></dl>
<p id="note-ref">A note follows<a href="#n1"><sup>1</sup></a>.</p>
<aside epub:type="footnote" id="n1"><p>The note.</p></aside>
<aside><p>An aside that is not a note is a boundary.</p></aside>
<div>Text straight in a div.<div>Nested div text.</div>After it.</div>
<section><header>Header text</header><p>Section paragraph.</p><footer>Footer</footer></section>
<table><caption>Caption</caption><tr><th>Head</th><th>Head</th></tr><tr><td>Cell</td><td>Cell</td></tr></table>
<figure><img src="../images/a.png" alt="a"/><p>Figure text.</p></figure>
<p>Inline <img src="../images/a.png" alt=""/> image, and one with a space:
<img src="../images/b%20c.png" alt=""/>.</p>
<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"><desc>skipped</desc>
<image xlink:href="../images/a.png"/></svg>
<script>var skipped = "<p>no</p>";</script>
<p id="end">The end.</p>
""")
    c1 = chapter('<h1 id="two">Chapter two</h1><p>Short.</p><p id="last">Last paragraph.</p>')
    toc = [("Heading <i>one</i>", "text/c0.xhtml#top", [("The note", "text/c0.xhtml#n1", []),
                                                         ("The end", "text/c0.xhtml#end", [])]),
           ("Chapter two", "text/c1.xhtml", [("Last", "text/c1.xhtml#last", [])])]
    book = Book()
    standard(book, [c0, c1], toc=toc, images=("a.png", "b c.png"))
    return book


# One TOC tree written as nav and as NCX, each alone: their entries must match (phase 7). Every entry
# has a target, since a valid NCX always has one; toc_faults has the entry with none (R26).
TOC_TREE = [
    ("Part one", "text/c0.xhtml", [("Chapter 1", "text/c0.xhtml", [("Section 1.1", "text/c0.xhtml#s1", []),
                                                        ("Section 1.2", "text/c0.xhtml#s2", [])]),
                        ("Chapter 2", "text/c1.xhtml#top", [])]),
    ("Part two", "text/c2.xhtml", [("Deep", "text/c2.xhtml#a", [("Deeper", "text/c2.xhtml#b", [
        ("Deepest", "text/c2.xhtml#c", [])])])]),
    ("Percent %C3%A9", "text/c%33.xhtml", []),
]
TOC_CHAPTERS = [
    chapter('<p>Intro.</p><h2 id="s1">One</h2><p>Text.</p><h2 id="s2">Two</h2><p>Text.</p>'),
    chapter('<h1 id="top">Two</h1><p>Text.</p>'),
    chapter('<p id="a">A</p><div id="b"><p>B</p></div><p>Before <span id="c">C</span> after.</p>'),
    chapter('<p>Reached through a percent-encoded href.</p>'),
]


def fx_toc_nav():
    book = Book()
    standard(book, TOC_CHAPTERS, toc=TOC_TREE, toc_kinds=("nav",))
    return book


def fx_toc_ncx():
    book = Book()
    standard(book, TOC_CHAPTERS, toc=TOC_TREE, toc_kinds=("ncx",))
    return book


# TOC faults: a label longer than any buffer (R26's rewind), a broken fragment (R13), a target
# outside the spine (R26), an entry with no target, and markup in a nav label.
def toc_faults(kinds):
    long_label = "".join(f"word{i % 97} " for i in range(8000)).strip()
    toc = [(long_label, "text/c0.xhtml", []),
           ("Broken fragment", "text/c0.xhtml#missing", []),
           ("Not in the spine", "extra.xhtml", []),
           ("No target", None, [("Child of no target", "text/c0.xhtml#here", [])])]
    book = Book()
    standard(book, [chapter('<p>Body.</p><p id="here">Here.</p>')], toc=toc, toc_kinds=kinds,
             extra_manifest=[("extra", "extra.xhtml", XHTML, None)])
    book.add("OEBPS/extra.xhtml", chapter("<p>Outside the spine.</p>"))
    return book


def fx_toc_faults():
    return toc_faults(("nav", "ncx"))


# The same faults with only the NCX to read: there the entry with no target is skipped (R26).
def fx_toc_faults_ncx():
    return toc_faults(("ncx",))


# Broken markup and text faults: each R47 recovery rule, the depth limit, entities, control
# characters, Windows-1252, a BOM, list numbering faults, a missing image, a UTF-16 item that fails
# alone, and an item cut off mid-document (R9, R21, R30, R45, R47).
def fx_recovery():
    deep = "".join("<blockquote>" if i % 2 else "<i>" for i in range(40))
    c0 = chapter(f"""
<p>Block in a block: <p>inner</p> after, still in the outer's style? <i>yes <p>split</p> carried</i></p>
<p><b>Unwound <i>by a close</b> further down.</p>
<p>Unmatched close</span> tag.</p>
<p>Unknown &bogus; entity and a bare & ampersand.</p>
<p>Control\x01char\x7f and tab\there.</p>
<div>Boundary <hr> splits <br> and keeps.</div>
<ul><li>Unclosed item<li>Next item</ul>
<p>{deep}Too deep to frame.</p>
<ol reversed=""><li>Reversed with no start counts up.</li><li>Two</li></ol>
<ol><li value="99999999999">Clamped ordinal.</li></ol>
<p>A missing image: <img src="../images/nope.png" alt=""/>.</p>
<p>Never closed, <b>nor this
""")
    c1 = b"\xef\xbb\xbf" + chapter("<p>After a BOM.</p>").encode()
    c2 = chapter("<p>Caf\x00 na\x00ve \x00</p>").encode().replace(b"\x00", b"\xe9")  # Windows-1252
    c3 = "\ufeff" + chapter("<p>UTF-16 is unsupported.</p>")
    book = Book()
    # cut off mid-document: no </body> to close what is open, so the end does (R47)
    c4 = chapter("<div><p>Cut <i>off").split("\n</body>")[0]
    standard(book, [c0, c1, c2, c3.encode("utf-16-le"), c4])
    return book


def words(seed, count):
    """Deterministic filler text, so the fixture is the same everywhere."""
    vocab = ("the of and to a in that it was he for on are as with his they at be this from have or "
             "by one had not but what all were when we there can an your which their said if do will "
             "each about how up out them then she many some so these would other into has more her two "
             "like him see time could no make than first been its who now people my made over did down "
             "only way find use may water long little very after words called just where most know").split()
    x = seed or 1
    out = []
    for _ in range(count):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append(vocab[x % len(vocab)])
    return out


# One long chapter, several windows deflated, for checkpoints and seeks (R48, R51's large book).
def fx_large():
    parts = []
    for i in range(2500):
        w = words(i + 1, 40 + i % 60)
        if i % 7 == 3:
            w[5] = f"<i>{w[5]}</i>"
        if i % 11 == 5:
            w[9] = f"<b>{w[9]} <u>{w[10]}</u></b>"
            w[10] = ""
        text = " ".join(x for x in w if x)
        if i % 100 == 0:
            parts.append(f'<h2 id="h{i // 100}">Section {i // 100}</h2>')
        if i % 37 == 0:
            parts.append(f"<ul><li>{text}</li><li>{' '.join(words(i + 9000, 6))}</li></ul>")
        elif i % 53 == 0:
            parts.append(f'<blockquote><p>{text}</p></blockquote><p><img src="../images/a.png" alt=""/></p>')
        else:
            parts.append(f"<p>{text}</p>")
    book = Book()
    standard(book, [chapter("\n".join(parts))], toc=[("Start", "text/c0.xhtml", []),
                                                    ("Middle", "text/c0.xhtml#h12", []),
                                                    ("End", "text/c0.xhtml#h24", [])])
    return book


# Many short items, like R51's Pride and Prejudice: 65 items, one non-linear, one missing (R17, R41).
def fx_many_items():
    count = 65
    chapters = [chapter(f'<h1>Chapter {i}</h1><p>{" ".join(words(i + 7, 120))}</p>') for i in range(count)]
    linear = [i != 1 for i in range(count)]
    book = Book()
    standard(book, chapters, linear=linear, toc=[(f"Chapter {i}", f"text/c{i}.xhtml", []) for i in range(count)])
    # drop chapter 40's entry: the spine still names it
    book.entries = [e for e in book.entries if e[0] != "OEBPS/text/c40.xhtml"]
    return book


# Packaging faults R30 forgives and R43/R44 settle: mimetype deflated and not first, the OPF at
# the root under a differently cased path, percent-encoded hrefs, a duplicate name, an EPUB 2 cover,
# and an identifier that is not the unique one.
def fx_packaging():
    book = Book()
    book.add("META-INF/container.xml", container("Content.OPF"))
    book.add("mimetype", "application/epub+zip", DEFLATED)
    book.add("Text/Chapter One.xhtml", chapter('<p>Spaces in the name. <img src="../Images/cover.png"/></p>'))
    book.add("Text/Chapter One.xhtml", chapter("<p>The duplicate: never read.</p>"))
    book.add("Text/c2.xhtml", chapter("<p>Second.</p>"))
    book.add("Images/cover.png", PNG, STORED)
    book.add("content.opf", opf(
        [("c1", "Text/Chapter%20One.xhtml", XHTML, None), ("c2", "text/C2.xhtml", XHTML, None),
         ("cover", "Images/cover.png", "image/png", None)],
        [("c1", True), ("c2", True)],
        metadata=('<dc:identifier>urn:bartap:not-unique</dc:identifier><dc:title>Packaging</dc:title>'
                  '<meta name="cover" content="cover"/>'),
        unique_id="missing", version="2.0"))
    return book


# Every entry stored, so text streams take the stored path end to end.
def fx_stored():
    book = Book()
    standard(book, [chapter("<p>Stored chapter.</p><p><i>Styled</i> and <img src=\"../images/a.png\"/>.</p>"),
                    chapter("<h1>Two</h1><p>Also stored.</p>")],
             toc=[("One", "text/c0.xhtml", []), ("Two", "text/c1.xhtml", [])], method=STORED)
    return book


# ZIP64 records on every entry (R40).
def fx_zip64():
    book = Book()
    standard(book, [chapter("<p>ZIP64 local and directory records.</p>")])
    book.entries = [(n, d, m, True) for n, d, m, _ in book.entries]
    return book


FIXTURES = {name[3:]: fn for name, fn in sorted(globals().items()) if name.startswith("fx_")}


def main():
    args = sys.argv[1:]
    if args == ["--list"]:
        print("\n".join(FIXTURES))
        return 0
    if not args:
        print(__doc__.strip().splitlines()[-1])
        return 2
    out = Path(args[0])
    names = args[1:] or list(FIXTURES)
    unknown = [n for n in names if n not in FIXTURES]
    if unknown:
        print(f"unknown fixtures: {', '.join(unknown)}")
        return 2
    out.mkdir(parents=True, exist_ok=True)
    for name in names:
        FIXTURES[name]().write(out / f"{name}.epub")
    return 0


if __name__ == "__main__":
    sys.exit(main())
