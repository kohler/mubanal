// mubanal.hh -- MuPDF-based reimplementation of `banal -json`.
//
// Port of src/banal (Geoffrey M. Voelker, Eddie Kohler) from Perl/pdftohtml to
// C++/MuPDF. Internal units are decipoints, as in banal. See
// devel/mubanal-plan.md for the calibration measurements behind the transfer
// functions and the extraction rules.
//
// Text is UTF-8 throughout, but banal operates on Perl decoded strings, where
// `length` counts codepoints and the word regex applies a two-character minimum
// per codepoint. Each run therefore carries its codepoint count alongside the
// bytes, and the two places that genuinely need codepoints -- word counting and
// the hyphen backscan -- decode on the fly with the helpers below.
#ifndef MUBANAL_HH
#define MUBANAL_HH

#include <cmath>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mubanal {

struct XMap;   // extent map; an implementation detail of analyze.cc

// ------------------------------------------------------------------- units

inline double in2pdf(double x) {
    return x * 720.0;
}

inline double pdf2pt(double x) {
    return x / 10.0;
}

// true points -> banal decipoints, quantized to 1/8 as banal's rpdf() does.
// poppler clamps -zoom at 3 (default 1.5), so banal's explicit -zoom 3 makes
// its units 3 * points; with $p2h_scale = 10/3 that is points * 10.
inline double rpdf(double pts) {
    return std::floor(pts * 80.0 + 0.5) / 8.0;
}

// the pdftohtml fontspec size: runs split only when this changes, not when the
// exact size does (9.9626 and 10.0617 both quantize to 30)
inline int p2h_size(double pts) {
    return int(std::floor(3.0 * pts + 0.5));
}

// true font size in points -> banal decipoints. Reproduces pdftohtml's integer
// quantization at its fixed 3x scale plus banal's +1 compensation
// ($p2h_font_size_compensation == 1 for poppler >= 0.85). Verified against 171
// fontspecs: round(3 * size) matches poppler's for all but one Type3 font.
//
// The +1, amplified by 10/3, is the whole of banal's ~0.33pt upward bias in
// bodyfontsize: a true 12pt body reports as 12.3. It is reproduced
// deliberately. Removing it would report smaller sizes, so against a spec
// minimum -- the branch conferences actually set -- papers that clear the bar
// today would stop clearing it, tightening every deployed sub_banal spec
// overnight. (A spec maximum, rarely configured, would loosen instead.)
// Correcting the bias is a decision about deployed settings, not about this
// function.
inline int rpdffont(double pts) {
    return int(std::floor((p2h_size(pts) + 1) * 10.0 / 3.0 + 0.5));
}

// banal skips text at or above this HSL lightness. 220 keeps visible light
// greys -- #d3d3d3 (211), #cccccc (204), #bfbfbf (191) all carry real
// code-listing text -- while still catching #e5e5e5 (229) watermarks.
constexpr unsigned kLightSkip = 220;
constexpr int kMinNchars = 800;
constexpr int kGrid = 4;

// Counter map keyed by double, standing in for banal's Perl hashes. Ordered,
// which is what makes mode() break ties toward the smaller key as banal's
// modevalkey does -- every document-level number is a mode, so that matters.
using DMap = std::map<double, long>;

std::optional<double> mode(const DMap& m);

// -------------------------------------------------------------------- utf8

// Decode the codepoint starting at `i`, advancing `i` past it.
char32_t u8_next(std::string_view s, size_t& i);
// Start offset of the codepoint ending at `i`.
size_t u8_prev(std::string_view s, size_t i);
void u8_append(std::string& s, char32_t c);
size_t u8_length(std::string_view s);

bool eq_ascii_ci(std::string_view s, std::string_view ascii);
bool in_ascii_ci(std::string_view s, std::initializer_list<std::string_view> asciis);

// ASCII whitespace, for run-edge trimming. See Trim below.
inline bool is_ascii_space(char32_t c) {
    return c == U' ' || (c >= 0x09 && c <= 0x0D);
}

// -------------------------------------------------------------- extraction

struct Run {
    double t = 0, l = 0, r = 0, b = 0;
    double size = 0;   // true font size, points
    double lum = 0;    // HSL lightness 0..255, alpha-composited over white
    std::string text;
    int nchars = 0;    // codepoints in `text`, which is what banal's length() counts
};

struct RawPage {
    double w = 0, h = 0;   // page box, points
    std::vector<Run> runs;
};

// Rotation policy: poppler linearizes text at an arbitrary angle into a small
// unrotated box, so banal never sees a wide diagonal run; MuPDF reports true
// quads. Measured over 300 files, dropping all non-horizontal text agrees with
// banal better than keeping 90-degree text (56.0% vs 48.3% file-level).
enum class Rot { Skip, Skew, Keep };

// Which characters to strip from a run's edges. pdftohtml excludes leading and
// trailing whitespace from an element's text and bbox, and matching that is
// what makes run left edges line up: it lifts (top,left) agreement from 70.2%
// to 90.1%.
//
// ASCII whitespace is enough, and Unicode whitespace deliberately is not
// handled. Without FZ_STEXT_PRESERVE_WHITESPACE, MuPDF replaces every kind of
// horizontal whitespace with U+0020, and a scan of 179025 runs found exactly
// two characters at run edges: U+0020 (34999) and U+000A (27). No U+00A0, no
// thin/en/em spaces, and none anywhere in run interiors either. Matching
// U+2000..U+200A and friends would be code that never executes -- which is how
// banal's luminance branch came to sit dead and wrong for years.
//
// None exists for diagnostics; it is how the scan above was taken.
enum class Trim { Ascii, None };

// Pages are pulled one at a time and analysed immediately, so a document's full
// text is never resident. Holding every run of a long submission at once costs
// tens of megabytes for no benefit -- nothing outside the per-page pass looks at
// a run again.
struct PageSource {
    virtual ~PageSource() = default;
    virtual size_t npages() const = 0;
    // Returns page `index`. A page that will not render yields an empty
    // RawPage rather than throwing, which is what banal does when pdftohtml
    // emits a partial page.
    virtual RawPage page(size_t index) = 0;
};

// Both throw std::runtime_error if the input cannot be opened.
// stext_flags is a raw FZ_STEXT_* mask, exposed for calibration.
std::unique_ptr<PageSource> open_pdf(const std::string& filename, Rot rot,
                                     Trim trim = Trim::Ascii, int stext_flags = 0);
std::unique_ptr<PageSource> open_runs(const std::string& path);

// ---------------------------------------------------------------- analysis

struct Text {
    double l = 0, t = 0, r = 0, b = 0;   // decipoints
    int sz = 0;                          // decipoints
    unsigned nchars = 0;
    std::string text;
    bool num = false;            // short numeric run, excluded from the extent maps
    bool is_references = false;  // start of the references; future words excluded
    bool heading1 = false;       // heading near top of page

    void account_sizes(DMap& alen, DMap& tlen) const;
};

struct HeadingText : public Text {
    double coll = 0, colr = 0;
};

struct AppendixStatus {
    bool acks_refs = false;
    bool conclusion = false;
    std::optional<std::string> appendix;
};

struct Page {
    size_t index_;                         // index in pages[]
    double pw_ = 0, ph_ = 0;               // page box, decipoints
    int ncols_ = 0;
    std::vector<double> colpos_;
    bool has_textbb_ = false;
    double top_, left_, width_, height_;
    size_t nchars_;
    std::optional<size_t> nwords_;
    int bodyfontsize_ = 0, reffontsize_ = 0;   // decipoints; 0 = unset
    std::optional<double> lead_;
    int max_heading_fontsize_ = 0;
    double lowest_number_ = 0;
    std::optional<unsigned> first_section_;
    bool heading1_ = false;
    std::vector<std::string> headings_;
    std::string type_ = "body";

    void calc_columns(const XMap& xmap);
    void calc_leading(const std::vector<Text>& texts, int bfs);
    void calc_nwords(const std::vector<Text>& texts, int bfs);
    AppendixStatus calc_page_type(AppendixStatus status);
};

struct Doc {
    std::vector<Page> pages_;             // Page::num is 1-based; this is not
    double pw_ = 0, ph_ = 0;               // doc page box, decipoints
    int ncols_ = 0;
    std::optional<int> bodyfontsize_;
    int max_bodyfontsize_ = 0;
    std::optional<double> lead_;
    DMap bodyfont_counts_, lead_counts_;
    std::set<std::string> headings_;
    unsigned max_numeric_heading_ = 0, max_section_ = 0;
    bool in_references_ = false;

    // banal's @recent_reasonable_colpos / @alternate_colpos: column positions
    // carried between pages_, so a page whose own columns look implausible can
    // fall back on the last sane ones. Document state, not global state.
    std::vector<double> recent_colpos_, alt_colpos_;

    size_t npages() const {
        return pages_.size();
    }

    void analyze_page(Page& page, const RawPage& rp);
    void merge_page(Page& page);
    void calc_page_columns(Page& page, const XMap& xmap);
    // Returns the heading runs; they point into `texts` and so must not outlive
    // it, which is why they are not kept on Page.
    std::vector<HeadingText> calc_page_headings(Page& page, std::vector<Text>& texts,
                                                int bfs);
    void calc_page_types();
    void calc_columns();
};

Doc analyze(PageSource& src);

// ------------------------------------------------------------------ report

struct ReportOpts {
    bool pagenum = false;
    bool no_time = false;
    bool colpos = false;
};

void report_json(const Doc& doc, const ReportOpts& opt);
void report_error(const ReportOpts& opt);
void dump_runs(PageSource& src);

}  // namespace mubanal

#endif
