// analyze.cc -- banal's geometry and heading heuristics.
//
// A near-transliteration of src/banal, working in decipoints. banal's regexes
// become explicit matchers over the text; that keeps Perl's semantics exact
// without a regex dependency.
//
// Everything banal matches is ASCII, so the matchers index bytes directly. The
// two places that need codepoints are word counting, where Perl's word regex
// applies a two-character minimum per codepoint, and the hyphen backscan; both
// decode on the fly. Character counts come from Run::nchars rather than
// string::size(), because banal's length() counts codepoints.
//
// Take care with backtracking when editing the matchers: banal's optional
// prefix groups retry the empty alternative on failure, so "Acknowledgements"
// must not be read as section "A" followed by "cknowledgements".

#include "mubanal.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>

namespace mubanal {
namespace {

// ------------------------------------------------------------- byte classes

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_upper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool is_roman(char c) {
    return c == 'I' || c == 'V' || c == 'X';
}

bool is_alpha(char c) {
    return is_upper(c) || (c >= 'a' && c <= 'z');
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_hpunct(char c) {
    return c == ',' || c == ';' || c == '(' || c == ')';
}

char lower1(char c) {
    return is_upper(c) ? char(c + 32) : c;
}

// This table helps define word characters, in the sense of Perl's `\w` over
// decoded text: letters, combining marks and digits build words, while
// whitespace, punctuation and symbols separate them.
//
// Defined by exclusion so that a script nobody thought to enumerate still
// counts as text. The table below is deliberately approximate at the edges.
//
// Private-use characters count as word constituents. Subsetted fonts map real
// glyphs there, and in a 150-document sample every private-use character
// adjacent to text stood in for a digit -- "IPv<PUA>" for "IPv6", "P<PUA>LRU"
// for "P4LRU".
//
// The table contents are Unicode (>= 0x80) whitespace, punctuation and symbol
// blocks, in ascending order. Generated from categories Zs/Zl/Zp,
// Pd/Ps/Pe/Pi/Pf/Po, Sm/Sc/Sk/So and Cc, then collapsed to block granularity;
// connector punctuation is deliberately absent, as `\w` includes it.
constexpr char32_t upper_nonword_ranges[][2] = {
    {0x80, 0x9F},
    {0xA0, 0xA9},
    {0xAB, 0xB1},
    {0xB4, 0xB4},
    {0xB6, 0xB8},
    {0xBB, 0xBB},
    {0xBF, 0xBF},
    {0xD7, 0xD7},
    {0xF7, 0xF7},
    {0x2C2, 0x2C5},
    {0x2D2, 0x2DF},
    {0x2E5, 0x2EB},
    {0x2ED, 0x2ED},
    {0x2EF, 0x2FF},
    {0x2000, 0x206F},
    {0x2070, 0x2070},
    {0x2074, 0x207E},
    {0x2080, 0x208E},
    {0x20A0, 0x20CF},
    {0x2100, 0x2101},
    {0x2103, 0x2106},
    {0x2108, 0x2109},
    {0x2114, 0x2114},
    {0x2116, 0x2118},
    {0x211E, 0x2123},
    {0x2125, 0x2125},
    {0x2127, 0x2127},
    {0x2129, 0x2129},
    {0x212E, 0x212E},
    {0x213A, 0x213B},
    {0x2140, 0x2144},
    {0x214A, 0x214D},
    {0x214F, 0x214F},
    {0x2190, 0x2BFF},
    {0x2E00, 0x2E7F},
    {0x3000, 0x3004},
    {0x3008, 0x3020},
    {0x3030, 0x3030},
    {0xFB29, 0xFB29},
    {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},
    {0xFF01, 0xFF0F},
    {0xFF1A, 0xFF20},
    {0xFF3B, 0xFF40},
    {0xFF5B, 0xFF65},
    {0xFFE0, 0xFFEE},
    {0xFFF9, 0xFFFD},
    {0x1F000, 0x1FAFF}
};

// Ranges are ascending and disjoint, so a binary search bounds the work at
// ~6 comparisons.
bool upper_nonword(char32_t c) {
    size_t lo = 0, hi = sizeof(upper_nonword_ranges) / sizeof(*upper_nonword_ranges);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < upper_nonword_ranges[mid][0]) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return lo > 0 && c <= upper_nonword_ranges[lo - 1][1];
}

// ASCII and Latin-1 cover ~98.8% of the characters in a typical submission, so
// they get a direct table: bit 0 answers is_word, bit 1 is_word2. One load
// serves both, where is_word2 otherwise runs is_word and then six more
// comparisons. Built from upper_nonword_ranges above, so there is one source of
// truth for which characters are word constituents.
constexpr uint8_t kWord = 1, kWord2 = 2;

constexpr std::array<uint8_t, 256> word_table = [] {
    std::array<uint8_t, 256> t{};
    for (size_t c = 0; c != 256; ++c) {
        bool w = true, w2 = true;
        if (c < 0x80) {
            w = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_';
            w2 = w || c == '[' || c == ']' || c == '{' || c == '}'
                || c == ',' || c == '.';
        } else {
            for (const auto& r : upper_nonword_ranges) {
                if (c >= r[0] && c <= r[1]) {
                    w = w2 = false;
                    break;
                }
            }
        }
        t[c] = uint8_t((w ? kWord : 0) | (w2 ? kWord2 : 0));
    }
    return t;
} ();

bool is_word(char32_t c) {
    if (c < 0x100) {
        return (word_table[c] & kWord) != 0;
    }
    return !upper_nonword(c);
}

// banal's second word class also admits brackets, braces, comma and period --
// all ASCII, so above U+00FF the two classes coincide.
bool is_word2(char32_t c) {
    if (c < 0x100) {
        return (word_table[c] & kWord2) != 0;
    }
    return !upper_nonword(c);
}

// ---------------------------------------------------------- text utilities

// banal: $content =~ s/\[[\s\d,-]+\]//g
// The bracket class is ASCII, and no ASCII byte occurs inside a multi-byte
// sequence, so this is safe to do byte-wise.
void strip_citations(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[') {
            size_t k = i + 1;
            while (k < s.size()
                   && (is_space(s[k]) || is_digit(s[k]) || s[k] == ',' || s[k] == '-')) {
                ++k;
            }
            if (k > i + 1 && k < s.size() && s[k] == ']') {
                i = k + 1;
                continue;
            }
        }
        out.push_back(s[i]);
        ++i;
    }
    s = std::move(out);
}

// banal: while ($content =~ /[\w\200-\377][\w\[\]\{\},.\200-\377]+(?:\(s\))?/g)
int count_words(std::string_view s) {
    int n = 0;
    size_t i = 0;
    while (i < s.size()) {
        char32_t c = u8_next(s, i);
        if (!is_word(c) || i >= s.size()) {
            continue;
        }
        size_t j = i;
        char32_t c2 = u8_next(s, j);
        if (!is_word2(c2)) {
            continue;
        }
        i = j;
        for (;;) {
            size_t k = i;
            if (k >= s.size()) {
                break;
            }
            char32_t c3 = u8_next(s, k);
            if (!is_word2(c3)) {
                break;
            }
            i = k;
        }
        if (i + 2 < s.size() && s[i] == '(' && lower1(s[i + 1]) == 's'
            && s[i + 2] == ')') {
            i += 3;
        }
        ++n;
    }
    return n;
}

// banal: length($content) <= 5 && $content =~ /\A[- ,.:\/0-9]*\z/
bool is_num(std::string_view s, int nchars) {
    if (nchars > 5) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return c == '-' || c == ' ' || c == ',' || c == '.' || c == ':'
            || c == '/' || is_digit(c);
    });
}

void rtrim(std::string& s) {
    while (!s.empty() && is_space(s.back())) {
        s.pop_back();
    }
}

// -------------------------------------------------------- heading matchers

struct HeadingNumber {
    bool ok = false;
    unsigned num;
    std::string key;
    bool numeric;
};

// banal:663, case-sensitive alternatives
bool head_match_cs(std::string_view s, HeadingNumber* hn = nullptr) {
    if (hn) {
        hn->ok = false;
    }
    // A Roman number, \d{1,3}(?:\.\d+)*, or [A-M](?:\.\d+)*
    size_t p = 0, n = s.size(), t, num = 0;
    if (p == n) {
        return false;
    } else if (is_roman(s[p])) {
        while (p < n && s[p] == 'X') {
            num += 10;
            ++p;
        }
        if (n - p >= 2 && s[p] == 'I' && (s[p + 1] == 'X' || s[p + 1] == 'V')) {
            num += s[p + 1] == 'X' ? 9 : 4;
            p += 2;
        } else {
            if (p < n && s[p] == 'V') {
                num += 5;
                ++p;
            }
            while (p < n && s[p] == 'I') {
                num += 1;
                ++p;
            }
        }
        t = 0;
    } else if (is_digit(s[p])) {
        while (p < n && p < 3 && is_digit(s[p])) {
            num = (num * 10) + s[p] - '0';
            ++p;
        }
        t = 1;
    } else if (s[p] >= 'A' && s[p] <= 'M') {
        ++p;
        t = 2;
    } else {
        return false;
    }
    while (t != 0 && n - p >= 2 && s[p] == '.' && is_digit(s[p + 1])) {
        for (p += 2; p != n && is_digit(s[p]); ++p) {
        }
    }
    size_t e = p;
    if (p < n && s[p] == '.') {
        ++p;
    }
    if (p == n || s[p] != ' ') {
        return false;
    }
    while (p < n && s[p] == ' ') {
        ++p;
    }
    // Only the numbered forms require a character after the space; the Roman
    // one ends in `[^,;()]*`, which matches empty.
    if ((t == 1 && (p == n || (!is_alpha(s[p]) && s[p] != '_')))
        || (t == 2 && (p == n || (!is_upper(s[p]) && s[p] != '_')))) {
        return false;
    }
    while (p < n && !is_hpunct(s[p])) {
        ++p;
    }
    if (hn && p == n) {
        hn->ok = true;
        hn->num = unsigned(num);
        hn->key = s.substr(0, e);
        hn->numeric = t == 1;
    }
    return p == n;
}

// banal:663, the case-insensitive keyword alternative
bool head_match_ci(std::string_view s) {
    if (in_ascii_ci(s, {
            "Abstract", "Acknowledgment", "Acknowledgement", "Acknowledgments",
            "Acknowledgements", "Contents", "Reference", "References",
            "Bibliography", "Introduction", "Appendices"
        })) {
        return true;
    }
    // Appendix(?:es)?[^,;()]*
    size_t n = s.size();
    if (n < 8 || !eq_ascii_ci(s.substr(0, 8), "appendix")) {
        return false;
    }
    size_t p = 8;
    while (p < n && !is_hpunct(s[p])) {
        ++p;
    }
    return p == n;
}

}  // namespace

// -------------------------------------------------------------------- xmap

struct XMap {
    double rowh = 0, snap = 0;
    std::vector<std::vector<double>> rows;   // interval pairs

    XMap() = default;

    XMap(double h, double rowh_, double snap_)
        : rowh(rowh_), snap(snap_) {
        int n = int(std::ceil(h / rowh_));
        rows.resize(size_t(n < 0 ? 0 : n));
    }

    size_t nrows() const {
        return rows.size();
    }

    void add(double top, double left, double right, double bottom) {
        long t = std::max<long>(0, long(std::floor(top / rowh)));
        long b = std::min<long>(long(nrows()), long(std::ceil(bottom / rowh)));
        for (; t < b; ++t) {
            auto& z = rows[size_t(t)];
            size_t i = 0, j;
            while (i < z.size() && z[i + 1] < left - snap) {
                i += 2;
            }
            for (j = i; j < z.size() && z[j] <= right + snap; j += 2) {
            }
            if (j > i + 2) {
                z.erase(z.begin() + long(i + 1), z.begin() + long(j - 1));
                j = i + 2;
            }
            if (j == i && (i == z.size() || right < z[i])) {
                z.insert(z.begin() + long(i), {left, right});
            } else {
                z[i] = std::min(z[i], left);
                z[i + 1] = std::max(z[i + 1], right);
            }
        }
    }

    XMap close_gaps() const {
        XMap out;
        out.rowh = rowh;
        out.snap = snap;
        out.rows = rows;
        for (size_t t = 0; t < out.rows.size(); ++t) {
            auto& z = out.rows[t];
            static const std::vector<double> kEmpty;
            const auto& pz = t == 0 ? kEmpty : rows[t - 1];
            const auto& nz = t + 1 == rows.size() ? kEmpty : rows[t + 1];
            size_t pi = 0, ni = 0;
            if (z.empty()) {
                continue;
            }
            double ll = z[0], lr = z[1];
            size_t i = 2;
            while (i < z.size()) {
                double rl = z[i], rr = z[i + 1];
                while (pi < pz.size() && pz[pi + 1] < rl) {
                    pi += 2;
                }
                while (ni < nz.size() && nz[ni + 1] < rl) {
                    ni += 2;
                }
                if (((pi < pz.size() || ni < nz.size())
                     && (pi == pz.size() || pz[pi] < lr)
                     && (ni == nz.size() || nz[ni] < lr))
                    || (rl - rr) * 2.5 > std::min(lr - ll, rr - rl)) {
                    z.erase(z.begin() + long(i - 1), z.begin() + long(i + 1));
                    lr = rr;
                } else {
                    i += 2;
                    ll = rl;
                    lr = rr;
                }
            }
        }
        return out;
    }

    XMap strip_overlap(double left, double right) const {
        XMap out;
        out.rowh = rowh;
        out.snap = snap;
        out.rows.resize(rows.size());
        for (size_t y = 0; y < rows.size(); ++y) {
            const auto& z = rows[y];
            size_t i = 0;
            for (; i < z.size() && z[i + 1] < left; i += 2) {
                out.rows[y].insert(out.rows[y].end(), {z[i], z[i + 1]});
            }
            for (; i < z.size() && z[i] < right; i += 2) {
            }
            for (; i < z.size(); i += 2) {
                out.rows[y].insert(out.rows[y].end(), {z[i], z[i + 1]});
            }
        }
        return out;
    }

    size_t count_nonempty() const {
        size_t n = 0;
        for (const auto& z : rows) {
            if (!z.empty()) {
                ++n;
            }
        }
        return n;
    }

    std::pair<double, double> find_bounds(double frac) const {
        std::vector<double> lows, highs;
        for (const auto& z : rows) {
            if (!z.empty()) {
                lows.push_back(z.front());
                highs.push_back(z.back());
            }
        }
        std::sort(lows.begin(), lows.end());
        std::sort(highs.begin(), highs.end());
        size_t n = lows.size();
        long f = frac > 0 ? long(std::floor(frac * double(n))) : 0;
        if (n == 0 || size_t(f) >= n) {
            return {0, 0};
        }
        if (f <= 0) {
            return {lows.front(), highs.back()};
        }
        return {lows[size_t(f)], highs[n - 1 - size_t(f)]};
    }
};

// ------------------------------------------------------------- page passes

namespace {

int body_font_size(const DMap& alen, const DMap& tlen) {
    double asize = 0, tsize = 0;
    if (auto m = mode(alen)) {
        asize = *m;
    }
    if (auto m = mode(tlen)) {
        tsize = *m;
    }
    if (tsize > 0 && (asize <= 0 || alen.at(asize) < 0.6 * tlen.at(tsize))) {
        asize = tsize;
    }
    return int(asize);
}

}  // namespace

void Text::account_sizes(DMap& alen, DMap& tlen) const {
    long len = nchars + 1;
    alen[sz] += len;
    if (!num) {
        tlen[sz] += len;
    }
}

namespace {

bool interval_overlap(double a0, double a1, double b0, double b1) {
    return (a1 >= b0 && a1 <= b1) || (b1 >= a0 && b1 <= a1);
}

}  // namespace

void Page::calc_columns(const XMap& in) {
    XMap cur = in.close_gaps();

    while (true) {
        DMap counts;
        for (const auto& z : cur.rows) {
            if (!z.empty()) {
                counts[double(z.size())] += 1;
            }
        }
        auto m = mode(counts);
        if (!m) {
            break;
        }
        size_t nc2 = size_t(*m);
        if (nc2 == 0) {
            break;
        }

        std::vector<DMap> edge(nc2);
        for (const auto& z : cur.rows) {
            if (z.size() == nc2) {
                for (size_t k = 0; k < nc2; k += 2) {
                    edge[k][z[k]] += 1;
                }
            }
        }
        std::vector<double> modes(nc2, 0);
        for (size_t i = 0; i < nc2; i += 2) {
            modes[i] = mode(edge[i]).value_or(0);
        }
        for (const auto& z : cur.rows) {
            if (z.size() == nc2) {
                for (size_t k = 0; k < nc2; k += 2) {
                    if (z[k + 1] > modes[k]) {
                        edge[k + 1][z[k + 1]] += 1;
                    }
                }
            }
        }
        for (size_t i = 1; i < nc2; i += 2) {
            modes[i] = mode(edge[i]).value_or(0);
        }

        std::vector<double> found;
        for (size_t i = 0; i < nc2; i += 2) {
            double l1 = modes[i], r1 = modes[i + 1];
            double overlap = std::min(in2pdf(0.75), r1 - l1 - in2pdf(0.125));
            double l = l1, r = r1;
            int n = 0;
            for (const auto& z : cur.rows) {
                if (z.size() != nc2) {
                    continue;
                }
                double l2 = z[i], r2 = z[i + 1];
                if (std::min(r1, r2) - std::max(l1, l2) >= overlap) {
                    auto il = edge[i].find(l2);
                    if (il != edge[i].end() && il->second > 3) {
                        l = std::min(l, l2);
                    }
                    auto ir = edge[i + 1].find(r2);
                    if (ir != edge[i + 1].end() && ir->second > 3) {
                        r = std::max(r, r2);
                    }
                    ++n;
                }
            }
            // A column has to be wide enough to hold text. Without this,
            // any three rows sharing an x -- a stack of equation labels in
            // the right margin, tick labels on a plot -- becomes a "column",
            // which is how a one-column page of inference rules came out as
            // two columns 1.5pt and 0.5pt wide. Half an inch is far below
            // any real column (the narrowest genuine ones here are the ~91pt
            // columns of a three-column layout) and far above the slivers.
            if (n >= 3 && r - l >= in2pdf(0.5)) {
                found.insert(found.end(), {l, r});
            }
        }
        if (found.empty()) {
            break;
        }

        size_t i = 0, j = 0;
        while (j < found.size()) {
            if (i == colpos_.size() || found[j] < colpos_[i]) {
                colpos_.insert(colpos_.begin() + i, {found[j], found[j + 1]});
                cur = cur.strip_overlap(found[j], found[j + 1]);
                j += 2;
            } else {
                i += 2;
            }
        }
        if (double(cur.count_nonempty()) < double(cur.nrows()) / 16.0) {
            break;
        }
    }

    ncols_ = int(colpos_.size() / 2);
    if (colpos_.empty()) {
        colpos_.insert(colpos_.end(), {left_, left_ + width_});
    }
}

void Page::calc_leading(const std::vector<Text>& texts, int bfs) {
    double minlead = in2pdf(1.0 / 16.0);
    DMap leads, rleads;
    for (size_t col = 0; col + 1 < colpos_.size(); col += 2) {
        double x = std::floor(0.8 * colpos_[col] + 0.2 * colpos_[col + 1]);
        std::vector<double> ypos;
        for (const auto& t : texts) {
            if (t.sz >= bfs && t.l <= x && t.r >= x) {
                ypos.push_back(t.t);
            }
        }
        std::sort(ypos.begin(), ypos.end());
        for (size_t j = 1; j < ypos.size(); ++j) {
            double lead = ypos[j] - ypos[j - 1];
            if (lead >= minlead) {
                leads[lead] += 1;
                rleads[std::floor(lead + 0.5)] += 1;
            }
        }
    }
    if (leads.empty()) {
        leads[0] = 1;
    }
    if (rleads.empty()) {
        rleads[0] = 1;
    }
    double best = *mode(leads), rbest = *mode(rleads);
    if (rleads[rbest] > leads[best] * 1.75) {
        best = rbest;
    }
    lead_ = std::floor(pdf2pt(best) * 10 + 0.5) / 10.0;
}

namespace {

int column_gap_overlap(const Text& t, double coll, double colr,
                       double tt, double tb, int bfs) {
    int gap = 0;
    if (t.sz <= bfs && interval_overlap(coll, colr, t.l, t.r)) {
        double slack = t.sz * 0.5;
        if (t.t >= tb && t.t - tb < slack) {
            gap |= 1;
        }
        if (tt >= t.b && tt - t.b < slack) {
            gap |= 2;
        }
    }
    return gap;
}

bool line_combine(const Text& t, double tt, double tb, double tl, double tr,
                  double slack, bool isright) {
    return interval_overlap(tt, tb, t.t, t.b)
        && (isright ? t.l >= tr - slack : tl >= t.r - slack)
        && (isright ? t.l < tr + 3 * slack : tl < t.r + 3 * slack);
}

bool check_column_fit(double tl, double tr, double slack, std::string_view content,
                      bool small, const std::vector<double>& colpos,
                      double& coll_out, double& colr_out) {
    size_t n = colpos.size(), colli = 0;
    while (colli < n && colpos[colli] < tl - slack && colpos[colli + 1] < tr
           && (colli + 2 == n || colpos[colli + 2] <= tr)) {
        colli += 2;
    }
    if (colli == n) {
        coll_out = colr_out = 0;
        return false;
    }
    size_t colri = colli;
    while (colri + 3 < n && colpos[colri + 2] <= tr
           && colpos[colri + 1] < colpos[colli] + in2pdf(1)) {
        colri += 2;
    }
    coll_out = colpos[colli];
    colr_out = colpos[colri + 1];
    if ((coll_out < tl - 2 * slack
         && std::fabs((tl - coll_out) - (colr_out - tr)) > in2pdf(0.25)
         && !eq_ascii_ci(content, "references"))
        || (tr >= colr_out - slack && small)) {
        return false;
    }
    return true;
}

}  // namespace

std::vector<HeadingText> Doc::calc_page_headings(Page& page, std::vector<Text>& texts,
                                                 int bfs) {
    double slack = in2pdf(0.125);
    double top = page.top_ + 0.075 * page.height_;
    bool near_top = true, in_refs_top = in_references_;
    unsigned min_section = max_section_, section_hi = min_section;
    size_t nskipped = 0;
    std::vector<HeadingText> heading_texts;
    HeadingNumber hn;

    for (size_t tpos = 0; tpos < texts.size(); /* increment in loop */) {
        size_t tpos1 = tpos;
        Text& text = texts[tpos];
        if (text.sz < bfs || double(text.sz) < slack) {
            if (in_refs_top && !text.num && text.t > top) {
                near_top = in_refs_top = false;
            }
            ++tpos;
            continue;
        }

        double tt = text.t, tb = text.b;
        bool was_near_top = near_top;
        if (near_top && !text.num && tb > top) {
            near_top = false;
        }

        char c0 = text.text.empty() ? '\0' : text.text[0];
        if (!(is_digit(c0)
              || (c0 && std::string_view("ABCDEFGHIRVX").find(c0) != std::string_view::npos))) {
            ++tpos;
            ++nskipped;
            continue;
        }

        double tl = text.l, tr = text.r;
        if (nskipped > 0 && line_combine(texts[tpos - 1], tt, tb, tl, tr, slack, false)) {
            ++tpos;
            ++nskipped;
            continue;
        }

        nskipped = 0;
        std::string content = text.text;
        ++tpos;
        while (tpos < texts.size()
               && line_combine(texts[tpos], tt, tb, tl, tr, slack, true)) {
            double textl = texts[tpos].l;
            if (textl - tr > text.sz / 6.0) {
                content.push_back(' ');
            }
            content += texts[tpos].text;
            tr = texts[tpos].r;
            ++tpos;
        }

        bool small = text.sz <= bfs;
        double coll = 0, colr = 0;
        bool fit = false;
        if (!alt_colpos_.empty()) {
            fit = check_column_fit(tl, tr, slack, content, small, alt_colpos_, coll, colr);
        }
        if (!fit) {
            fit = check_column_fit(tl, tr, slack, content, small, page.colpos_, coll, colr);
        }
        if (!fit) {
            continue;
        }

        rtrim(content);
        char last = content.empty() ? '\0' : content.back();
        if (last == '.'
            || last == ','
            || last == ':'
            || (!head_match_cs(content, &hn) && !head_match_ci(content))) {
            continue;
        }

        if (small) {
            int gaps = 0;
            long tposl = long(tpos1) - 1;
            size_t tposr = tpos;
            bool killed = false;
            while (tposl >= 0 || tposr < texts.size()) {
                if (tposl >= 0) {
                    gaps |= column_gap_overlap(texts[size_t(tposl)], coll, colr, tt, tb, bfs);
                    --tposl;
                }
                if (tposr < texts.size()) {
                    gaps |= column_gap_overlap(texts[tposr], coll, colr, tt, tb, bfs);
                    ++tposr;
                }
                if (gaps & 2) {
                    killed = true;
                    break;
                }
            }
            if (killed) {
                continue;
            }
        }

        if (hn.ok && hn.numeric) {
            if (hn.num > 99
                && max_numeric_heading_ > 0
                && max_numeric_heading_ < 20) {
                continue;
            }
            if (headings_.count(hn.key)) {
                continue;
            }
            max_numeric_heading_ = std::max(max_numeric_heading_, hn.num);
            headings_.insert(hn.key);
        }

        page.headings_.push_back(content);
        if (in_ascii_ci(content, {"references", "reference", "bibliography"})) {
            text.is_references = true;
        }
        if (was_near_top) {
            page.heading1_ = text.heading1 = true;
        }
        heading_texts.push_back({
            text,
            coll, colr
        });
        page.max_heading_fontsize_ = std::max(page.max_heading_fontsize_, text.sz);
        if (hn.ok && hn.num > 0 && hn.num > min_section) {
            if (!page.first_section_ || *page.first_section_ > hn.num) {
                page.first_section_ = hn.num;
            }
            section_hi = std::max(section_hi, hn.num);
        }

        if (page.headings_.size() == 1 && eq_ascii_ci(content, "contents")) {
            break;
        }
    }
    max_section_ = section_hi;
    return heading_texts;
}

void Page::calc_nwords(const std::vector<Text>& texts, int bfs_in) {
    double slack = in2pdf(0.125);
    double bfs = bfs_in * 0.95;
    int nw = 0;
    std::string prehyphen;
    bool have_pre = false;

    for (size_t tpos = 0; tpos < texts.size(); ++tpos) {
        const Text& text = texts[tpos];
        if (text.is_references) {
            break;
        }
        if (text.sz < bfs) {
            continue;
        }
        std::string content;
        if (have_pre) {
            content = std::move(prehyphen);
            prehyphen.clear();
            have_pre = false;
        }
        content += text.text;

        double tt = text.t, tb = text.b, tr = text.r;
        while (tpos + 1 < texts.size()) {
            const Text& nt = texts[tpos + 1];
            if (nt.l >= tr - slack && nt.l < tr + 3 * slack
                && interval_overlap(tt, tb, nt.t, nt.b)) {
                if (nt.l - tr > text.sz / 6.0) {
                    content.push_back(' ');
                }
                content += nt.text;
                tr = nt.r;
                ++tpos;
            } else {
                break;
            }
        }

        strip_citations(content);

        if (!content.empty() && content.back() == '-') {
            size_t i = 1;
            while (i < colpos_.size() && colpos_[i] + slack < text.r) {
                i += 2;
            }
            if (i < colpos_.size() && std::fabs(text.r - colpos_[i]) < 2 * slack) {
                // s/([\w()\[\]\{\}\200-\377]+)-\z//
                size_t e = content.size() - 1, b = e;
                while (b > 0) {
                    size_t p = u8_prev(content, b);
                    size_t q = p;
                    char32_t c = u8_next(content, q);
                    if (is_word(c) || c == U'(' || c == U')' || c == U'['
                        || c == U']' || c == U'{' || c == U'}') {
                        b = p;
                    } else {
                        break;
                    }
                }
                if (b < e) {
                    prehyphen = content.substr(b, e - b);
                    have_pre = true;
                    content.resize(b);
                }
            }
        }
        nw += count_words(content);
    }
    if (have_pre) {
        nw += count_words(prehyphen);
    }
    nwords_ = nw;
}

// ------------------------------------------------------- document passes

namespace {

int version_compare(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        size_t p = 0;
        while (true) {
            size_t q = s.find('.', p);
            v.push_back(s.substr(p, q == std::string::npos ? q : q - p));
            if (q == std::string::npos) {
                break;
            }
            p = q + 1;
        }
        return v;
    };
    std::vector<std::string> as = split(a), bs = split(b);
    size_t i = 0;
    for (; i < as.size() && i < bs.size(); ++i) {
        bool al = as[i].size() == 1 && as[i][0] >= 'A' && as[i][0] <= 'Z';
        bool bl = bs[i].size() == 1 && bs[i][0] >= 'A' && bs[i][0] <= 'Z';
        if (al != bl) {
            if (i == 0) {
                if (!a.empty() && a[0] == 'A') {
                    return 1;
                }
                if (!b.empty() && b[0] == 'A') {
                    return -1;
                }
            }
            return -100;
        }
        if (al ? as[i] < bs[i] : atoi(as[i].c_str()) < atoi(bs[i].c_str())) {
            return -1;
        }
        if (al ? as[i] > bs[i] : atoi(as[i].c_str()) > atoi(bs[i].c_str())) {
            return 1;
        }
    }
    if (i == 0) {
        return -100;
    }
    if (i >= as.size() && i < bs.size()) {
        return -1;
    }
    if (i < as.size() && i >= bs.size()) {
        return 1;
    }
    return 0;
}

// banal:825. The prefix group `(?:[\dIVX]+\.? *|[ABCDE]\.? *|)` is optional, so
// the regex backtracks to the empty alternative when a prefix leads nowhere --
// "Acknowledgements" must not be read as section "A" followed by
// "cknowledgements". Each alternative is therefore tried independently. The
// `\AAppendices` / `\AAppendix` alternatives are anchored, so they only apply
// when no prefix was consumed.
bool is_acks_refs(std::string_view p, bool had_num) {
    return in_ascii_ci(p, {"acknowledgment", "acknowledgement", "acknowledgments",
                           "acknowledgements", "reference", "references",
                           "bibliography"})
        || (!had_num && in_ascii_ci(p, {"appendices", "appendix", "appendixes"}));
}

bool is_conclusion(std::string_view h) {
    return in_ascii_ci(h, {"conclusion", "conclusions", "conclusion and future work", "conclusions and future work"});
}

struct HeadingAnalysis {
    std::string num;
    bool num_digit = false;
    bool num_alpha = false;
    bool num_roman = false;
    bool appendix_prefix = false;
    bool appendix_heading = false;
    bool appendix_ok = false;
    bool acks_refs_ok = false;
    bool conclusion_ok = false;

    HeadingAnalysis(const std::string& h) {
        acks_refs_ok = is_acks_refs(h, false);
        size_t p = 0;
        if (h.size() >= 8
            && eq_ascii_ci(std::string_view(h).substr(0, 8), "appendix")) {
            size_t q = 8;
            while (q < h.size() && (h[q] == ' ' || h[q] == '\t')) {
                ++q;
            }
            if (q > 8) {
                p = q;
                appendix_prefix = true;
            }
        }
        size_t s = p;
        if (p == h.size()) {
            return;
        } else if (is_roman(h[p])) {
            for (++p; p < h.size() && is_roman(h[p]); ++p) {
            }
            num_roman = true;
        } else if (h[p] >= 'A' && h[p] <= 'M') {
            ++p;
            num_alpha = true;
        } else if (is_digit(h[p])) {
            for (++p; p < h.size() && is_digit(h[p]); ++p) {
            }
            num_digit = true;
        } else {
            return;
        }
        bool subsection = false;
        while (p + 1 < h.size() && h[p] == '.' && is_digit(h[p + 1])) {
            subsection = true;
            for (p += 2; p < h.size() && is_digit(h[p]); ++p) {
            }
        }
        size_t e = p;
        if (p < h.size() && h[p] == '.') {
            ++p;
        }
        if (p < h.size() && !is_space(h[p])) {
            return;
        }
        num = h.substr(s, e - s);
        while (p < h.size() && is_space(h[p])) {
            ++p;
        }
        std::string_view rest = std::string_view(h).substr(p);
        appendix_heading = !appendix_prefix
            && in_ascii_ci(rest, {"appendices", "appendixes", "appendix"});
        appendix_ok = appendix_prefix
            || !num_roman
            || num == "I";
        acks_refs_ok = acks_refs_ok
            || (!subsection
                && (!num_alpha || num <= "E")
                && is_acks_refs(rest, true));
        conclusion_ok = !subsection
            && is_conclusion(rest);
    }
};

}  // namespace

AppendixStatus Page::calc_page_type(AppendixStatus cur) {
    AppendixStatus next = cur;

    bool is_first = heading1_;
    for (const std::string& h : headings_) {
        HeadingAnalysis ap(h);
        if (ap.acks_refs_ok) {
            next.acks_refs = true;
            if (is_first) {
                cur.acks_refs = true;
            }
            next.appendix.reset();
        } else if (ap.conclusion_ok) {
            cur.conclusion = next.conclusion = true;
        } else if (ap.appendix_ok
                   && (next.acks_refs || next.conclusion)
                   && (ap.appendix_prefix
                       || (next.appendix && *next.appendix != "yes"
                           ? version_compare(ap.num, *next.appendix) == 1
                           : !ap.num_digit || ap.appendix_heading))) {
            next.appendix = ap.num;
            if (cur.acks_refs
                || (is_first
                    && (ap.appendix_prefix || ap.appendix_heading || cur.conclusion))) {
                cur.appendix = ap.num;
            }
        } else if (h.size() >= 8
                   && eq_ascii_ci(std::string_view(h).substr(0, 8), "appendix")) {
            next.appendix = "yes";
            if (cur.acks_refs || is_first) {
                cur.appendix = "yes";
            }
        } else {
            if ((!next.appendix
                 || ap.num_digit
                 || ap.num_roman)
                && (!cur.appendix
                    || (bodyfontsize_
                        && max_heading_fontsize_ > bodyfontsize_))) {
                cur = AppendixStatus{};
                next.acks_refs = next.conclusion = false;
            }
        }
        is_first = false;
    }

    if (nchars_ == 0) {
        type_ = "blank";
    } else if (index_ == 0 && nchars_ < 800) {
        type_ = "cover";
    } else if (cur.appendix) {
        type_ = "appendix";
    } else if (cur.acks_refs) {
        type_ = "bib";
    } else if (nchars_ < kMinNchars || ncols_ == 0) {
        type_ = "figure";
    } else {
        type_ = "body";
    }
    return next;
}

void Doc::calc_page_types() {
    AppendixStatus s;
    for (size_t i = 0; i < pages_.size(); ++i) {
        s = pages_[i].calc_page_type(s);
    }
}

void Doc::calc_columns() {
    DMap counts;
    std::map<int, int> firstpage;
    for (size_t i = 0; i < pages_.size(); ++i) {
        int nc = pages_[i].ncols_;
        if (nc > 0) {
            counts[nc] += 1;
            firstpage.emplace(nc, int(i));
        }
    }
    int best_ncols = 0;
    long best = 0;
    for (const auto& [k, v] : counts) {
        int key = int(k);
        if (v > best
            || (v == best && best_ncols && firstpage[key] < firstpage[best_ncols])) {
            best_ncols = key;
            best = v;
        }
    }
    ncols_ = best_ncols;
}

void Doc::merge_page(Page& page) {
    if (page.index_ == 0) {
        pw_ = page.pw_;
        ph_ = page.ph_;
        ncols_ = page.ncols_;
    }
    if (page.bodyfontsize_) {
        bodyfont_counts_[page.bodyfontsize_] += 1;
        if (!bodyfontsize_ || *bodyfontsize_ != page.bodyfontsize_) {
            bodyfontsize_ = int(*mode(bodyfont_counts_));
        }
        max_bodyfontsize_ = std::max(max_bodyfontsize_, page.bodyfontsize_);
    }
    if (page.lead_) {
        lead_counts_[*page.lead_] += 1;
        if (!lead_ || *lead_ != *page.lead_) {
            lead_ = *mode(lead_counts_);
        }
    }
}

namespace {

std::pair<int, int> body_and_ref_font_size(const std::vector<Text>& texts,
                                           const HeadingText* refhead,
                                           const HeadingText* unrefhead) {
    double rl = 0, rr = 0, rt = 0, ul = 0, ur = 0, ut = 0;
    if (refhead) {
        rl = refhead->coll;
        rr = refhead->colr;
        rt = refhead->t;
    }
    if (unrefhead) {
        ul = unrefhead->coll;
        ur = unrefhead->colr;
        ut = unrefhead->t;
    }
    DMap ref_a, ref_t, text_a, text_t;
    for (const auto& t : texts) {
        if ((refhead && (t.l < rl || t.r <= rl || (t.l <= rr && t.b <= rt)))
            || (unrefhead && (t.l >= ur || (t.r >= ul && t.t >= ut)))) {
            t.account_sizes(text_a, text_t);
        } else {
            t.account_sizes(ref_a, ref_t);
        }
    }
    return {body_font_size(text_a, text_t), body_font_size(ref_a, ref_t)};
}

}  // namespace

std::optional<double> mode(const DMap& m) {
    // DMap is ordered, so scanning with a strict > keeps the smallest key among
    // ties -- banal's modevalkey tie-break.
    std::optional<double> best;
    long bestn = 0;
    for (const auto& [k, v] : m) {
        if (!best || v > bestn) {
            best = k;
            bestn = v;
        }
    }
    return best;
}

void Doc::analyze_page(Page& page, const RawPage& rp) {
    page.pw_ = rpdf(rp.w);
    page.ph_ = rpdf(rp.h);

    std::vector<Text> texts;
    texts.reserve(rp.runs.size());
    DMap alen, tlen;

    for (const Run& r : rp.runs) {
        if (r.lum >= kLightSkip) {
            continue;
        }
        double top = rpdf(r.t), left = rpdf(r.l);
        // banal rounds the width and height, then adds them to the rounded
        // origin. rpdf() quantizes to 1/8, so rpdf(t) + rpdf(b - t) is not
        // rpdf(b): rounding the far edge directly shifts it by up to 1/8pt.
        double bottom = top + rpdf(r.b - r.t), right = left + rpdf(r.r - r.l);
        left = std::max(0.0, left);
        top = std::max(0.0, top);
        right = std::min(right, page.pw_);
        bottom = std::min(bottom, page.ph_);
        if (top >= bottom || left >= right) {
            continue;
        }
        Text t;
        t.t = top;
        t.l = left;
        t.r = right;
        t.b = bottom;
        t.sz = rpdffont(r.size);
        t.text = r.text;
        t.nchars = r.nchars;
        t.num = is_num(t.text, t.nchars);
        t.account_sizes(alen, tlen);
        if (t.num
            && bottom > page.lowest_number_
            && std::any_of(t.text.begin(), t.text.end(), is_digit)) {
            page.lowest_number_ = bottom;
        }
        texts.push_back(std::move(t));
    }

    if (texts.empty()) {
        return;
    }

    int bfs = body_font_size(alen, tlen);
    int numfontsize = bfs;
    if (bodyfontsize_ && *bodyfontsize_ > numfontsize) {
        numfontsize = *bodyfontsize_;
    }

    XMap xm(page.ph_, in2pdf(0.25), in2pdf(1.0 / 16.0));
    XMap ym(page.pw_, in2pdf(0.25), in2pdf(1.0 / 16.0));
    for (const Text& t : texts) {
        if (!t.num || t.sz > numfontsize) {
            xm.add(t.t, t.l, t.r, t.b);
            ym.add(t.l, t.t, t.b, t.r);
        }
    }
    auto [l0, r0] = xm.find_bounds(0.1);
    auto [t0, b0] = ym.find_bounds(0.1);
    page.top_ = t0;
    page.left_ = l0;
    page.width_ = r0 - l0;
    page.height_ = b0 - t0;
    page.has_textbb_ = true;

    page.calc_columns(xm);
    if (page.colpos_.size() == 4
        && page.colpos_[2] - page.colpos_[1] <= in2pdf(1)) {
        recent_colpos_ = page.colpos_;
        alt_colpos_.clear();
    } else {
        alt_colpos_ = recent_colpos_;
    }

    // The heading runs point into `texts`, so they stay local to this page.
    std::vector<HeadingText> heading_texts = calc_page_headings(page, texts, bfs);

    int reffontsize = 0;
    if (!heading_texts.empty()) {
        bool was_inref = in_references_, inref = in_references_;
        HeadingText* refhead = nullptr;
        HeadingText* unrefhead = nullptr;
        for (HeadingText& ht : heading_texts) {
            if (ht.is_references) {
                inref = true;
                refhead = &ht;
            } else if (inref) {
                inref = false;
                unrefhead = &ht;
            }
        }
        in_references_ = inref;
        if (refhead && refhead->heading1) {
            was_inref = true;
        }
        if (unrefhead && unrefhead->heading1) {
            was_inref = false;
        }
        if ((refhead && !was_inref) || (unrefhead && was_inref)) {
            std::tie(bfs, reffontsize) =
                body_and_ref_font_size(texts, refhead, unrefhead);
        } else if (was_inref) {
            reffontsize = bfs;
            bfs = 0;
        }
    } else if (in_references_) {
        reffontsize = bfs;
        bfs = 0;
    }
    page.bodyfontsize_ = bfs;
    page.reffontsize_ = reffontsize;
    int fontsize = bfs ? bfs : reffontsize;

    page.calc_leading(texts, fontsize);

    page.nchars_ = 0;
    for (const auto& [size, count] : alen) {
        if (size >= fontsize && size <= 1.5 * fontsize) {
            page.nchars_ += count;
        }
    }

    if (bfs) {
        page.calc_nwords(texts, bfs);
    }
}

Doc analyze(PageSource& src) {
    Doc doc;
    size_t np = src.npages();
    doc.pages_.resize(np);

    for (size_t pi = 0; pi < np; ++pi) {
        // The RawPage dies at the end of this iteration, so a document's full
        // text is never resident at once.
        Page& page = doc.pages_[pi];
        page.index_ = pi;
        doc.analyze_page(page, src.page(pi));
        doc.merge_page(page);
    }

    doc.calc_page_types();
    doc.calc_columns();
    return doc;
}

}  // namespace mubanal
