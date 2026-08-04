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
#include <mutex>
#include <stdexcept>

namespace mubanal {
namespace {

struct Glyph {
    char32_t c;
    double x0, x1, y0, y1;
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
        });
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
                        rb_.glyphs.push_back(Glyph{
                            char32_t(c->c),
                            std::min(c->quad.ul.x, c->quad.ll.x),
                            std::max(c->quad.ur.x, c->quad.lr.x),
                            std::min(c->quad.ul.y, c->quad.ur.y),
                            std::max(c->quad.ll.y, c->quad.lr.y)
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

std::unique_ptr<PageSource> open_pdf(const std::string& filename, Rot rot,
                                     Trim trim, int stext_flags) {
    return std::make_unique<PdfSource>(filename, rot, trim, stext_flags);
}

}  // namespace mubanal
