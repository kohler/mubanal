// extract.cc -- MuPDF structured text -> banal-style run list.
//
// Uses MuPDF's C++ bindings rather than the C API. The C API signals errors by
// longjmp, which does not run destructors, so cleanup has to live in fz_always
// and every local read there must be registered with fz_var -- a discipline no
// test can check, because the error paths that would expose a mistake are
// exactly the ones a corpus rarely exercises. The C++ layer throws
// FzErrorBase : std::exception instead, so handles are RAII and that hazard
// cannot recur. Measured identical in output and in runtime; see the README.
//
// A run corresponds to a pdftohtml `<text>` element, not to a MuPDF font run.
// Two rules, both calibrated against pdftohtml over the corpus:
//
//  - A run breaks when the font name or the *quantized* fontspec size changes.
//    Comparing exact sizes splits runs poppler does not (9.9626 and 10.0617
//    both quantize to 30), which made every bibliography read as an extra
//    column: "[1]" separated from its entry by a 4.98pt gap, just over the
//    extent map's 4.5pt snap, on every reference row.
//
//  - Leading and trailing whitespace glyphs are excluded from both the text and
//    the bbox, as pdftohtml does. This is what makes run left edges line up:
//    90.1% of elements become bit-identical in (top, left), versus 70.2%
//    without it.

#include "mubanal.hh"

#include <mupdf/classes.h>
#include <mupdf/classes2.h>
#include <mupdf/exceptions.h>
#include <mupdf/functions.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mubanal {

bool verbose = false;

namespace {

// Set by the MuPDF callbacks installed in PdfSource's constructor.
bool mupdf_warning = false;
bool mupdf_error = false;

struct Glyph {
    char32_t c;
    double x0, x1, y0, y1;
    double base;
};

// banal composites a translucent colour over white before testing lightness.
unsigned lightness(unsigned argb) {
    unsigned alpha = (argb >> 24) & 0xFF,
        r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    if (alpha < 255) {
        double scale = 1.0 - alpha / 255.0;
        r += (255 - r) * scale;
        g += (255 - g) * scale;
        b += (255 - b) * scale;
    }
    unsigned mn = std::min({r, g, b}), mx = std::max({r, g, b});
    return (mn + mx) / 2;
}

bool horizontal(const ::fz_stext_line* line) {
    return std::fabs(line->dir.x - 1) < 0.02 && std::fabs(line->dir.y) < 0.02;
}

bool axial(const ::fz_stext_line* line) {
    double ax = std::fabs(line->dir.x), ay = std::fabs(line->dir.y);
    return horizontal(line) || (ax < 0.02 && std::fabs(ay - 1) < 0.02);
}

struct RunBuilder {
    Trim trim = Trim::Ascii;
    std::vector<Glyph> glyphs;
    std::string font;
    double size = 0;
    unsigned lum = 0;
    int psize = 0;
    bool open = false;

    // A page that throws part way through must not leak a half-built run into
    // the next page's first flush.
    void reset() {
        glyphs.clear();
        open = false;
    }

    void flush(RawPage& page) {
        if (!open) {
            return;
        }
        open = false;
        auto trimmable = [this](char32_t c) {
            return trim != Trim::None && is_ascii_space(c);
        };
        auto a = glyphs.begin(), z = glyphs.end();
        while (a != z && trimmable(a->c)) {
            ++a;
        }
        while (z != a && trimmable((z - 1)->c)) {
            --z;
        }
        if (a == z) {
            return;
        }
        Run run;
        run.l = a->x0;
        run.r = a->x1;
        run.t = a->y0;
        run.b = a->y1;
        // Runs split whenever the font size changes, so a superscript is
        // already a run of its own and the glyphs here share one baseline.
        run.base = a->base;
        run.size = size;
        run.lum = lum;
        run.text.reserve(size_t(z - a));
        auto it = a;
        goto loop_middle;
        while (it != z) {
            run.l = std::min(run.l, it->x0);
            run.r = std::max(run.r, it->x1);
            run.t = std::min(run.t, it->y0);
            run.b = std::max(run.b, it->y1);
        loop_middle:
            u8_append(run.text, it->c);
            ++run.nchars;
            ++it;
        }
        page.runs.push_back(std::move(run));
    }
};

// ------------------------------------------------- dangerous PDF features
//
// See UnsafeMap in mubanal.hh for the categories and the threat model. The
// scan sweeps every xref object rather than following references from the
// catalog: reachability analysis fails open if it misses one referencing
// spelling, while the sweep's worst error is flagging an unreachable object
// no real pipeline emits. Each object recurses into direct children only --
// an indirect reference is some other xref entry, visited there -- so
// nothing is scanned twice and reference cycles cannot loop.
//
// Names are compared as strings because SubmitForm, GoToE, Rendition and
// OpenAction are missing from this MuPDF's PDF_NAME table.

bool name_is(::pdf_obj* obj, const char* name) {
    return obj && mupdf::ll_pdf_is_name(obj)
        && std::strcmp(mupdf::ll_pdf_to_name(obj), name) == 0;
}

bool name_in(::pdf_obj* obj, std::initializer_list<const char*> names) {
    if (!obj || !mupdf::ll_pdf_is_name(obj)) {
        return false;
    }
    const char* n = mupdf::ll_pdf_to_name(obj);
    for (const char* name : names) {
        if (std::strcmp(n, name) == 0) {
            return true;
        }
    }
    return false;
}

// Whether a media file specification names data outside the file. A dict
// filespec with /EF embeds its data; a stream is a form XObject standing in
// for the clip, also embedded; a bare string is always a path.
bool external_filespec(::pdf_obj* fs) {
    if (mupdf::ll_pdf_is_stream(fs)) {
        return false;
    }
    if (mupdf::ll_pdf_is_dict(fs)) {
        return !mupdf::ll_pdf_dict_gets(fs, "EF");
    }
    return mupdf::ll_pdf_is_string(fs);
}

// ll_pdf_load_object is the file's one kept reference, and an accessor that
// throws mid-object must not leak it past the document.
struct ScopedPdfObj {
    ::pdf_obj* obj;
    explicit ScopedPdfObj(::pdf_obj* o)
        : obj(o) {
    }
    ~ScopedPdfObj() {
        mupdf::ll_pdf_drop_obj(obj);
    }
    ScopedPdfObj(const ScopedPdfObj&) = delete;
    ScopedPdfObj& operator=(const ScopedPdfObj&) = delete;
};

class UnsafeScanner {
public:
    explicit UnsafeScanner(::pdf_document* doc)
        : doc_(doc) {
    }

    UnsafeMap scan() {
        try {
            nobj_ = mupdf::ll_pdf_xref_len(doc_);
        } catch (const std::exception&) {
            return {};
        }
        for (cur_ = 1; cur_ < nobj_; ++cur_) {
            try {
                ScopedPdfObj obj(mupdf::ll_pdf_load_object(doc_, cur_));
                if (mupdf::ll_pdf_obj_num_is_stream(doc_, cur_)
                    && mupdf::ll_pdf_dict_gets(obj.obj, "F")) {
                    // stream data in an external file; /F means other
                    // things on non-streams, e.g. annotation flags
                    flag("externalref");
                }
                visit(obj.obj, 0);
            } catch (const std::exception&) {
            }
        }
        return attribute();
    }

private:
    ::pdf_document* doc_;
    int nobj_ = 0;
    int cur_ = 0;                  // object number being swept, for flag()
    std::vector<std::pair<const char*, int>> found_;   // (category, objnum)
    std::vector<int> owner_;       // object number -> 1-based page, 0 unknown
    std::vector<::pdf_obj*> claim_;   // worklist of resolved objects to claim
    // Prevent infinite nesting and infinite /Next chain walking
    // (Note that /Next chains can be circular.)
    static constexpr int kMaxDepth = 64;

    void flag(const char* category) {
        found_.emplace_back(category, cur_);
    }

    // Findings carry object numbers; the report wants pages. Ownership is
    // computed only when something was found -- one document in thousands --
    // so clean documents pay nothing. Pages claim their object subgraphs in
    // order, so a shared object reports its first page. Unclaimed objects
    // (the catalog's OpenAction, the JavaScript name tree, XFA) report as
    // page 1, the page showing when they fire.
    UnsafeMap attribute() {
        if (found_.empty()) {
            return {};
        }
        // The one allocation proportional to the xref: up to ~32MB at
        // MuPDF's 8,388,607 object-number cap. On failure own nothing --
        // findings all report page 1 -- rather than lose the report to an
        // escaped bad_alloc.
        try {
            claim_pages();
        } catch (const std::exception&) {
            owner_ = {};
        }
        // The report prints 10 pages plus a trailing 0 for more, so the 11
        // smallest per category suffice; the cap keeps these sets O(1) for
        // a document with millions of pages.
        std::map<std::string, std::set<long>> pages;
        for (const auto& [category, num] : found_) {
            std::set<long>& pgset = pages[category];
            long page = num < int(owner_.size()) && owner_[num] ? owner_[num] : 1;
            pgset.insert(page);
            if (pgset.size() > 11) {
                pgset.erase(std::prev(pgset.end()));
            }
        }
        UnsafeMap out;
        for (const auto& [category, pgset] : pages) {
            out[category].assign(pgset.begin(), pgset.end());
        }
        return out;
    }

    void claim_pages() {
        owner_.assign(nobj_, 0);
        int np = 0;
        try {
            np = mupdf::ll_pdf_count_pages(doc_);
        } catch (const std::exception&) {
        }
        for (int p = 0; p < np; ++p) {
            try {
                ::pdf_obj* pg = mupdf::ll_pdf_lookup_page_obj(doc_, p);
                int num = mupdf::ll_pdf_obj_parent_num(pg);
                if (num > 0 && num < nobj_ && !owner_[num]) {
                    owner_[num] = p + 1;
                }
                claim_.clear();
                claim_.push_back(pg);
                while (!claim_.empty()) {
                    ::pdf_obj* obj = claim_.back();
                    claim_.pop_back();
                    claim(obj, p + 1, 0);
                }
            } catch (const std::exception&) {
            }
        }
    }

    // Direct children recurse; indirect targets go through claim_, so a
    // reference chain cannot deepen the C++ stack and the owner check in
    // claim_child stops revisits and cycles.
    void claim(::pdf_obj* obj, int page, int depth) {
        if (depth >= kMaxDepth) {
            return;
        }
        int n;
        if (mupdf::ll_pdf_is_dict(obj)) {
            n = mupdf::ll_pdf_dict_len(obj);
            for (int i = 0; i < n; ++i) {
                // /Parent leads back through the page tree to the catalog,
                // which would hand the whole document to page 1.
                if (name_is(mupdf::ll_pdf_dict_get_key(obj, i), "Parent")) {
                    continue;
                }
                claim_child(mupdf::ll_pdf_dict_get_val(obj, i), page, depth);
            }
        } else if (mupdf::ll_pdf_is_array(obj)) {
            n = mupdf::ll_pdf_array_len(obj);
            for (int i = 0; i < n; ++i) {
                claim_child(mupdf::ll_pdf_array_get(obj, i), page, depth);
            }
        }
    }

    void claim_child(::pdf_obj* val, int page, int depth) {
        if (!mupdf::ll_pdf_is_indirect(val)) {
            claim(val, page, depth + 1);
            return;
        }
        int num = mupdf::ll_pdf_to_num(val);
        if (num > 0 && num < nobj_ && !owner_[num]) {
            owner_[num] = page;
            claim_.push_back(mupdf::ll_pdf_resolve_indirect(val));
        }
    }

    void visit(::pdf_obj* obj, int depth) {
        if (depth >= kMaxDepth) {
            return;
        }
        if (mupdf::ll_pdf_is_dict(obj)) {
            check_dict(obj, depth);
            for (int i = 0, n = mupdf::ll_pdf_dict_len(obj); i < n; ++i) {
                ::pdf_obj* val = mupdf::ll_pdf_dict_get_val(obj, i);
                if (!mupdf::ll_pdf_is_indirect(val)) {
                    visit(val, depth + 1);
                }
            }
        } else if (mupdf::ll_pdf_is_array(obj)) {
            for (int i = 0, n = mupdf::ll_pdf_array_len(obj); i < n; ++i) {
                ::pdf_obj* val = mupdf::ll_pdf_array_get(obj, i);
                if (!mupdf::ll_pdf_is_indirect(val)) {
                    visit(val, depth + 1);
                }
            }
        }
    }

    void check_dict(::pdf_obj* d, int depth) {
        ::pdf_obj* s = mupdf::ll_pdf_dict_gets(d, "S");
        if (name_is(s, "JavaScript")) {
            flag("javascript");
        } else if (name_is(s, "Launch")) {
            flag("launch");
        } else if (name_in(s, {"SubmitForm", "ImportData"})) {
            flag("submitform");
        } else if (name_is(s, "RichMediaExecute")) {
            flag("richmedia");
        } else if (name_is(s, "MCD")
                   && external_filespec(mupdf::ll_pdf_dict_gets(d, "D"))) {
            // media clip data played from outside the file; an embedded
            // clip is just content
            flag("multimedia");
        } else if (mupdf::ll_pdf_dict_gets(d, "JS")) {
            // a script outside a well-formed JavaScript action: a Rendition
            // action's /JS, or an action missing its /S
            flag("javascript");
        }

        ::pdf_obj* subtype = mupdf::ll_pdf_dict_gets(d, "Subtype");
        if (name_is(subtype, "RichMedia")) {
            flag("richmedia");
        } else if (name_is(subtype, "Movie")) {
            // the legacy Movie annotation's clip, same embedded-or-not test
            ::pdf_obj* movie = mupdf::ll_pdf_dict_gets(d, "Movie");
            if (mupdf::ll_pdf_is_dict(movie)
                && external_filespec(mupdf::ll_pdf_dict_gets(movie, "F"))) {
                flag("multimedia");
            }
        } else if (name_is(subtype, "Form")
                   && mupdf::ll_pdf_dict_gets(d, "Ref")) {
            // reference XObject: renders a page of a different document
            flag("externalref");
        }

        if (name_is(mupdf::ll_pdf_dict_gets(d, "FS"), "URL")) {
            // URL file specification: whatever consumes it hits the network
            flag("externalref");
        }
        if (mupdf::ll_pdf_dict_gets(d, "XFA")) {
            flag("xfa");
        }

        // Auto-triggered actions: /OpenAction at document open, /AA entries
        // on open/close/visibility events. Scripts inside them are flagged
        // above wherever they live; what is left is the URI or remote goto
        // that is harmless as a clicked link but leaks unprompted here.
        check_trigger(mupdf::ll_pdf_dict_gets(d, "OpenAction"), depth);
        ::pdf_obj* aa = mupdf::ll_pdf_dict_gets(d, "AA");
        if (mupdf::ll_pdf_is_dict(aa)) {
            for (int i = 0, n = mupdf::ll_pdf_dict_len(aa); i < n; ++i) {
                check_trigger(mupdf::ll_pdf_dict_get_val(aa, i), depth);
            }
        }
    }

    // Destination arrays and plain GoTo actions pass; is_dict and dict_gets
    // resolve an indirect action.
    void check_trigger(::pdf_obj* action, int depth) {
        if (depth >= kMaxDepth || !mupdf::ll_pdf_is_dict(action)) {
            return;
        }
        if (name_in(mupdf::ll_pdf_dict_gets(action, "S"),
                    {"URI", "GoToR", "GoToE"})) {
            flag("autoaction");
        }
        // /Next chains successor actions (one or an array), so a benign
        // GoTo can head a queue that ends in a URI.
        ::pdf_obj* next = mupdf::ll_pdf_dict_gets(action, "Next");
        if (mupdf::ll_pdf_is_array(next)) {
            for (int i = 0, n = mupdf::ll_pdf_array_len(next); i < n; ++i) {
                check_trigger(mupdf::ll_pdf_array_get(next, i), depth + 1);
            }
        } else {
            check_trigger(next, depth + 1);
        }
    }
};

}  // namespace

// Holds the open document and yields one page at a time, so the caller never
// has to keep every page's runs alive at once.
class PdfSource : public PageSource {
public:
    PdfSource(const std::string& filename, Rot rot, Trim trim, int stext_flags)
        : rot_(rot), stext_flags_(stext_flags) {
        static std::once_flag handlers;
        std::call_once(handlers, [] {
            mupdf::ll_fz_register_document_handlers();
            // MuPDF's default callbacks narrate broken files to stderr
            // ("repairing PDF document", "cannot create appearance
            // stream"). The contract is JSON on stdout, so the messages are
            // dropped and only the fact of them kept, for "error" and
            // "warning" in the report.
            mupdf::ll_fz_set_warning_callback([](void*, const char* e) {
                if (mubanal::verbose) {
                    fprintf(stderr, "Warning: %s\n", e);
                }
                mupdf_warning = true;
            }, nullptr);
            mupdf::ll_fz_set_error_callback([](void*, const char* e) {
                if (mubanal::verbose) {
                    fprintf(stderr, "Error: %s\n", e);
                }
                mupdf_error = true;
            }, nullptr);
        });
        // One document per process, except --grep, whose documents do not
        // overlap; resetting here keeps one file's diagnostics off the next.
        mupdf_warning = mupdf_error = false;
        // FzErrorBase::what() prefixes "code=N: "; m_text is the bare message,
        // and rethrowing as a standard exception keeps mupdf out of main.
        try {
            doc_.emplace(filename.c_str());
            npages_ = doc_->fz_count_pages();
        } catch (const mupdf::FzErrorBase& e) {
            throw std::runtime_error(e.m_text);
        }
        rb_.trim = trim;
    }

    size_t npages() const override {
        return npages_;
    }

    // Runs after page extraction (see analyze) so the sweep mostly hits
    // MuPDF's object cache. ll_pdf_specifics is null for non-PDF formats,
    // which have no object graph to scan.
    UnsafeMap unsafe() const override {
        if (::pdf_document* pdoc = mupdf::ll_pdf_specifics(doc_->m_internal)) {
            return UnsafeScanner(pdoc).scan();
        }
        return {};
    }

    bool error() const override {
        return mupdf_error;
    }

    bool warning() const override {
        return mupdf_warning;
    }

    RawPage page(size_t index) override {
        RawPage out;
        try {
            mupdf::FzPage pg = doc_->fz_load_page(index);
            mupdf::FzRect box = pg.fz_bound_page();
            out.w = box.x1 - box.x0;
            out.h = box.y1 - box.y0;

            mupdf::FzStextOptions opts(stext_flags_);
            mupdf::FzStextPage stext(pg, opts);

            // FzDocument, FzPage and FzStextPage own resources, so they are
            // wrapped for RAII. Blocks, lines and chars are owned by the stext
            // page and free nothing, so wrappers buy no safety there and cost
            // 8% in per-element construction; walk them directly.
            for (::fz_stext_block* block = stext.m_internal->first_block;
                 block;
                 block = block->next) {
                if (block->type != FZ_STEXT_BLOCK_TEXT) {
                    continue;
                }
                // Each ll_ wrapper looks up the thread-local context, so
                // resolving the font name per character costs real time at ~68k
                // characters a document -- 1.36x slower when measured. The
                // pointer only changes at run boundaries, so cache on it.
                const ::fz_font* last_font = nullptr;
                const char* last_name = "";
                for (::fz_stext_line* line = block->u.t.first_line;
                     line; line = line->next) {
                    rb_.flush(out);
                    if ((rot_ == Rot::Skip && !horizontal(line))
                        || (rot_ == Rot::Skew && !axial(line))) {
                        continue;
                    }
                    for (::fz_stext_char* c = line->first_char; c; c = c->next) {
                        if (c->font != last_font) {
                            last_font = c->font;
                            last_name = c->font ? mupdf::ll_fz_font_name(c->font) : "";
                        }
                        int ps = p2h_size(c->size);
                        if (!rb_.open || rb_.psize != ps || rb_.font != last_name) {
                            rb_.flush(out);
                            rb_.glyphs.clear();
                            rb_.font = last_name;
                            rb_.size = c->size;
                            rb_.psize = ps;
                            rb_.lum = lightness(unsigned(c->argb));
                            rb_.open = true;
                        }
                        // A glyph box taller than kGlyphMaxHeight times the
                        // glyph's own size is a font defect, not a measurement.
                        // MuPDF reads these from the font program rather than
                        // from the outline, so a subsetted font carrying a bad
                        // per-glyph box -- or its whole FontBBox, sized for
                        // large delimiters -- is reported verbatim, and
                        // FZ_STEXT_ACCURATE_BBOXES does not help: one 7.4pt
                        // beta came back 41.5pt tall either way and pulled a
                        // page's text block 12pt above where any ink was.
                        //
                        // Over 3M glyphs, including equation-heavy papers whose
                        // large delimiters are the obvious thing to break, no
                        // legitimate box exceeds 3x its size; 99.99% are under
                        // 1.8. So clamp back to the baseline, which is measured
                        // independently of the box and stays right.
                        double gy0 = std::min(c->quad.ul.y, c->quad.ur.y);
                        double gy1 = std::max(c->quad.ll.y, c->quad.lr.y);
                        if (c->size > 0
                            && gy1 - gy0 > kGlyphMaxHeight * c->size) {
                            gy0 = std::max(gy0, c->origin.y - kGlyphAscent * c->size);
                            gy1 = std::min(gy1, c->origin.y + kGlyphDescent * c->size);
                        }
                        rb_.glyphs.push_back(Glyph{
                            char32_t(c->c),
                            std::min(c->quad.ul.x, c->quad.ll.x),
                            std::max(c->quad.ur.x, c->quad.lr.x),
                            gy0, gy1,
                            c->origin.y
                        });
                    }
                }
            }
            rb_.flush(out);
        } catch (const std::exception&) {
            // A page that will not render still leaves the others usable, which
            // is what banal does when pdftohtml emits a partial page. Discard
            // any half-built run so it cannot leak into the next page.
            rb_.reset();
            out.runs.clear();
        }
        return out;
    }

private:
    std::optional<mupdf::FzDocument> doc_;
    RunBuilder rb_;
    Rot rot_;
    int stext_flags_;
    size_t npages_ = 0;
};

int stext_flag(std::string_view name) {
    static const struct { const char* name; int flag; } flags[] = {
        {"ligatures", FZ_STEXT_PRESERVE_LIGATURES},
        {"whitespace", FZ_STEXT_PRESERVE_WHITESPACE},
        {"images", FZ_STEXT_PRESERVE_IMAGES},
        {"inhibit-spaces", FZ_STEXT_INHIBIT_SPACES},
        {"dehyphenate", FZ_STEXT_DEHYPHENATE},
        {"spans", FZ_STEXT_PRESERVE_SPANS},
        {"clip", FZ_STEXT_CLIP},
        {"structure", FZ_STEXT_COLLECT_STRUCTURE},
        {"accurate-bboxes", FZ_STEXT_ACCURATE_BBOXES},
        {"accurate-ascenders", FZ_STEXT_ACCURATE_ASCENDERS},
        {"accurate-side-bearings", FZ_STEXT_ACCURATE_SIDE_BEARINGS},
        {"segment", FZ_STEXT_SEGMENT},
        {"table-hunt", FZ_STEXT_TABLE_HUNT}
    };
    for (const auto& f : flags) {
        if (name == f.name) {
            return f.flag;
        }
    }
    return -1;
}

int stext_default_flags() {
    return FZ_STEXT_ACCURATE_BBOXES;
}

std::unique_ptr<PageSource> open_pdf(const std::string& filename, Rot rot,
                                     Trim trim, int stext_flags) {
    return std::make_unique<PdfSource>(filename, rot, trim, stext_flags);
}

}  // namespace mubanal
