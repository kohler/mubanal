# Mubanal

A [MuPDF](https://mupdf.com)-based reimplementation of `src/banal -json`,
[HotCRP](https://hotcrp.com)’s PDF format checker. Requires C++17.

To build from source and run on `paper.pdf`:

```sh
cmake -S . -B build
cmake --build build -j
./build/mubanal paper.pdf
```

This downloads a MuPDF source release and builds it locally. To use a
preexisting MuPDF library, give the first `cmake` the `-DMUPDF_BUILD=OFF`
option. To build MuPDF from source including its bundled dependencies (freetype,
harfbuzz, openjpeg, libjpeg, jbig2dec, zlib, gumbo, brotli), give
`-DMUPDF_SYSTEM_LIBS=OFF`. (On MacOS, `-DMUPDF_SYSTEM_LIBS=ON` fails; the build
always uses bundled dependencies.)

Nothing is installed: `mubanal` links against the libraries where they are
built. The MuPDF build also produces a `mutool`, which is linked as
`build/mutool` and, like `mubanal`, can be run from any directory.

**`NDEBUG` must match how MuPDF was built.** The generated C++ headers declare
some destructors only `#ifndef NDEBUG`, so a mismatch fails to link with missing
destructors. Release MuPDF builds define it, so the build defines it too; pass
`-DMUPDF_NDEBUG=OFF` if you built MuPDF with debugging enabled. Only symbols are
affected, not object layout, so the failure is a loud link error rather than
silent corruption.

## Why

`src/banal` drives poppler’s `pdftohtml` and parses its XML. That stack has a
pathological case (documents with heavily replicated tiling patterns), and the
Perl is hard to extend. Mean time per document over 50 randomly chosen corpus
documents:

| Tool      | Analysis time   |
|:----------|:----------------|
| `perl src/banal` (including its pdftohtml subprocess) | 0.203s |
| `mubanal` | **0.070s (2.9x faster)** |

MuPDF’s text-extraction floor—`mutool draw -F text` on the same documents—is
53ms; most of the remaining 17ms is the unsafe scan’s sweep of every xref
object (see below), and the analysis itself is close to free.

## Options

| Option           | Meaning                                           |
|:-----------------|:--------------------------------------------------|
| `-p`, `-pagenum` | Add `"pageno"` and per-page `"headings"`          |
| `-C`, `-colpos`  | Add `"colpos"` column positions                   |
| `-no-time`       | omit the `"at"` timestamp (use when diffing)      |
| `-no-unsafe`     | skip the dangerous-feature scan                   |
| `--grep=FEATURE` | print matching FILEs' names instead of JSON       |
| `--dump-runs`    | print the extracted run list instead of analyzing |
| `--runs=FILE`    | analyze a stored run list instead of a PDF        |
| `--rot=MODE`     | rotated text: `skip` (default), `skew`, `keep`    |
| `--stext=OPTS`   | MuPDF stext options, comma separated (calibration) |

The `-json` and `-zoom` options from banal are ignored.

## Dangerous PDF features

A submitted paper is downloaded and opened by anonymous reviewers, so
rendering it must not run code or touch the network—a document that fetches an
external resource on open reveals the reviewer's IP address to whoever
controls the server. Every analysis therefore scans the PDF object graph and,
when it finds anything, adds a top-level `"unsafe"` key mapping category names
to the 1-based pages involved—at most the first 10, with a trailing `0`
standing for any pages beyond them:

```json
"unsafe": {"javascript": [27,28]},
```

An *embedded* movie or sound clip is just content and does not flag;
`multimedia` means the clip's file specification points outside the document.

Findings that belong to no page (an `/OpenAction` on the catalog, the
JavaScript name tree, XFA) report as page 1, the page showing when they fire;
an object shared between pages reports the first page using it. Pages are
attributed only after something is found, so clean documents never pay for
the ownership pass.

| Category      | Meaning                                                     |
|:--------------|:------------------------------------------------------------|
| `javascript`  | JavaScript actions or `/JS` scripts                         |
| `launch`      | Launch that can run a program: command form, absolute or URL path, executable target |
| `rellaunch`   | Launch of a bare relative document path (`run:./proof.v` artifact links) |
| `submitform`  | SubmitForm/ImportData actions (network form submission)     |
| `autoaction`  | `/OpenAction` or `/AA` auto-triggering a URI or remote goto |
| `richmedia`   | RichMedia annotations (embedded Flash/ActionScript)         |
| `multimedia`  | movie/sound clips played from outside the file              |
| `xfa`         | XFA forms (may script and open web connections)             |
| `externalref` | URL file specs, external streams, reference XObjects        |

Clickable URI link annotations are deliberately not flagged: a link the reader
chooses to follow is fine; the point is what happens unprompted. `rellaunch`
is advisory—worth surfacing, not grounds for rejection—and so is `multimedia`
when it appears without `externalref`; every other category warrants a human
look. The key is omitted entirely when nothing is found, so output for clean
documents—7137 of 7146 in the calibration corpus—is unchanged. The exceptions
split cleanly along that severity line: `javascript` catches embedded-movie
player controls, ACM/JSTOR reference-linking scripts, and form-field
formatters, while `rellaunch` catches papers linking their verification
artifacts (`run:./proof.v:164`) and proceedings-CD navigation
(`../TOC.pdf`).

The scan walks every xref object rather than chasing references from the
catalog. Parsing every object dictionary is the main cost: 13–17ms on a typical
corpus document, ~25% on top of the rest of the analysis. `-no-unsafe` skips the
scan, for timing and calibration; `--runs=` input carries no PDF structure and
is never scanned.

## Grep

`mubanal --grep=FEATURE FILE.pdf...` sweeps a corpus, printing the names of the
files whose analysis matches the FEATURE, and exits like grep—0 for a match, 1
for none, 2 on failure. Supported features: `unsafe`, `error`, `problem` (error
or warning).

## Layout

| File             | Description                                                |
|:-----------------|:-----------------------------------------------------------|
| `src/extract.cc` | `PdfSource`: MuPDF structured text → banal-style run list  |
| `src/analyze.cc` | Extent maps, columns, leading, headings, page types, words |
| `src/report.cc`  | `-json` output                                             |
| `src/runs.cc`    | `RunsSource` and `--dump-runs`, both JSON                  |
| `src/util.cc`    | UTF-8 helpers                                              |
| `src/main.cc`    | Command line                                               |

Pages are pulled through a `PageSource` one at a time and analysed immediately,
so a document’s full text is never resident.

MuPDF’s stext defaults are used deliberately. All three plausible alternatives
were measured and are worse: `accurate-bboxes` drops margin agreement from
93.7% to 75.8% (its ink-accurate boxes diverge from poppler’s advance-based
widths), `inhibit-spaces` destroys word counts (78% median error), and
`ligatures` changes nothing. `--stext=` remains as a calibration hook.

`extract.cc` is the interface between the analysis and MuPDF itself. It uses C++
bindings so error paths throw exceptions. Only the owning handles (`FzDocument`,
`FzPage`, `FzStextPage`) are wrapped. Blocks, lines and chars are owned by the
stext page and free nothing, so their wrappers buy no safety and cost time in
per-element construction; those lists are walked directly.

## Testing

HotCRP’s `batch/testmubanal.php` can compare a mubanal run against `src/banal`
over documents drawn from the docstore, named on the command line, or listed in
a file.

```sh
php batch/testmubanal.php -c 150 -s 3          # 150 random docstore documents
php batch/testmubanal.php --files=list.txt     # a fixed list
php batch/testmubanal.php a.pdf b.pdf          # specific files
```

`testmubanal.php` ignores character and word counts by default and allows 4pt of
margin slack, both of which differ constantly without meaning much;
`--include=c,w` and `--margin-tolerance` override that. Useful modes:

```
--summary        tally which fields disagree, instead of listing every one
--verdict        compare HotCRP's format-check verdicts rather than JSON
--html=FILE      write a report with an image of each disagreeing page
```

`--verdict` is the mode that matters for a switchover decision: field-level
drift only counts when it changes what HotCRP tells an author. It drives the
real `Default_FormatChecker` with each JSON blob injected, so the rules and
tolerances are HotCRP's own rather than a copy that would rot. `--spec` sets
the spec to check against.

`--html` renders each disagreeing page with its complaints underneath; clicking
a page cycles bare → margins → columns, drawn from mubanal’s own output.

If a problem is found, `mubanal`’s `--dump-runs` and `--runs` separate the two
halves of the tool:

```sh
./build/mubanal --dump-runs f.pdf > runs.json   # extraction only
./build/mubanal --runs=runs.json                # analysis only
```

Dump the runs from one build and feed them to another; if the lists are
byte-identical the difference is in the analysis, not in extraction.

### Known differences

Three differences from banal are understood, and none is a bug in either
program.

**Columns** are found differently. banal reads them off the modal number of
x-intervals per row band; mubanal looks for the vertical whitespace between
columns, pools the candidates across the document -- a paper has one layout, so
a gutter recurring on many pages is a column boundary and a gutter on one page
is a table -- and lets each page take the document's gutters where its own ink
leaves them clear. This disagrees with banal often, and deliberately: it is
right on the pages a wide table or a spanning figure used to fool. Pass
`--columns=mode` for banal's algorithm, and see `--debug-columns` for a trace.

**Margins** are the weakest field. poppler and MuPDF accumulate glyph advances
with a ~0.01% scale difference, and the 4pt output grid amplifies that to
exactly one grid step; a page is typically out by 4pt or not at all. That is why
`--margin-tolerance` defaults to 4; HotCRP's own textblock check allows 9pt, so
these do not reach a verdict.

**Word counts** differ slightly, and this is the one residual that can change a
verdict on its own. HotCRP’s `wordlimit` check, unlike `textblock` and
`bodyfontsize`, has no tolerance, so a document within a percent of the limit
can fall on either side of it.

There are no unit tests; correctness is established differentially against
`src/banal` over a corpus. Agreement figures are deliberately not recorded here
as both programs keep changing. Run `testmubanal.php` with `--verdict` and
`--summary` to measure the current state.

Because the column algorithms differ on purpose, agreement with banal is no
longer the measure for that field. `testmubanal.php --corpus` scores column
counts against hand-labelled documents and pages instead.

## License

Mubanal is available under the GNU Affero General Public License, Version 3.0,
as required by MuPDF.

## Authors

Eddie Kohler working with Claude Code. HotCRP’s banal derives from a script
originally by Geoff Voelker.
