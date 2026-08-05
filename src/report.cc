// report.cc -- banal's `-json` output.
//
// banal decides whether to emit a per-page field by comparing its *formatted*
// string against the document default, so the same comparison is done here.
// The output is hand-formatted rather than built with a JSON library because
// it must match banal's layout and its %g/%.0f number rendering.

#include "mubanal.hh"

#include <cstdio>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

namespace mubanal {
namespace {

// Perl sprintf "%.0f", applied to values that are already integral
std::string f0(double x) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", x);
    return buf;
}

// Perl sprintf "%g" over banal's value range
std::string fg(double x) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", x);
    return buf;
}

std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '\n':
            o += "\\n";
            break;
        case '\r':
            o += "\\r";
            break;
        case '\f':
            o += "\\f";
            break;
        case '\t':
            o += "\\t";
            break;
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '/':
            o += "\\/";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                o += buf;
            } else {
                o += char(c);
            }
        }
    }
    return o + "\"";
}

struct PageDim {
    double pw = 0, ph = 0, tw = 0, th = 0, mt = 0, ml = 0, mr = 0, mb = 0;
};

}  // namespace

void report_error(const ReportOpts& opt) {
    std::cout << "{\n";
    if (!opt.no_time) {
        std::cout << "  \"at\": " << long(std::time(nullptr)) << ",\n";
    }
    std::cout << "  \"error\": true,\n  \"pages\": []\n}\n";
}

void report_json(const Doc& doc, const ReportOpts& opt) {
    size_t np = doc.npages();
    std::vector<PageDim> px(static_cast<size_t>(np));
    DMap dpw, dph, dmt, dml, dmr, dmb;
    double nummargin = 10000;
    long nwords = 0;

    for (size_t i = 0; i < np; ++i) {
        const Page& page = doc.pages_[i];
        if (page.nwords_) {
            nwords += *page.nwords_;
        }
        PageDim& pd = px[size_t(i)];
        pd.pw = std::floor(pdf2pt(page.pw_) + 0.5);
        pd.ph = std::floor(pdf2pt(page.ph_) + 0.5);
        dpw[pd.pw] += 1;
        dph[pd.ph] += 1;
        if (page.has_textbb_) {
            // Outward to the grid, as banal does, so the reported block always
            // contains the real one -- a compliance check wants to err toward
            // noticing, and conferences' tolerances are set against that bias.
            //
            // But round to the nearest point on the way, which is what
            // subtracting half a point before the ceiling amounts to. These
            // numbers are reported in whole points, so a fraction of one is
            // below the resolution of the answer; without this a paper typeset
            // to exactly 72pt of bottom margin, measuring 720.015 here against
            // banal's 720.000, lost a whole 4pt grid step to fifteen
            // thousandths of a point -- and that step was enough to cross the
            // textblock check.
            double tl = std::floor((pdf2pt(page.left_) + kGridSlack) / kGrid) * kGrid;
            double tt = std::floor((pdf2pt(page.top_) + kGridSlack) / kGrid) * kGrid;
            double tr = std::ceil((pdf2pt(page.left_ + page.width_) - kGridSlack) / kGrid) * kGrid;
            double tb = std::ceil((pdf2pt(page.top_ + page.height_) - kGridSlack) / kGrid) * kGrid;
            pd.mt = tt;
            pd.ml = tl;
            pd.mb = pd.ph - tb;
            pd.mr = pd.pw - tr;
            pd.tw = tr - tl;
            pd.th = tb - tt;
            dmt[pd.mt] += 1;
            dml[pd.ml] += 1;
            dmr[pd.mr] += 1;
            dmb[pd.mb] += 1;
            double pnum = std::floor(pd.ph - pdf2pt(page.lowest_number_));
            if (pnum < pd.ph - tb) {
                nummargin = std::min(nummargin, pnum);
            }
        }
    }

    auto mt = mode(dmt), mr = mode(dmr);
    std::string doc_ps = "\"papersize\": [" + f0(mode(dph).value_or(0)) + ","
        + f0(mode(dpw).value_or(0)) + "]";
    std::string doc_margin = mt && mr
        ? "\"margin\": [" + f0(*mt) + "," + f0(*mr) + ","
              + f0(mode(dmb).value_or(0)) + "," + f0(mode(dml).value_or(0)) + "]"
        : std::string("\"margin\": [0,0,0,0]");
    std::string doc_bfs, doc_l;
    if (!doc.bodyfontsize_ || *doc.bodyfontsize_ <= 0) {
        doc_bfs = "\"bodyfontsize\": null";
        doc_l = "\"leading\": null";
    } else {
        doc_bfs = "\"bodyfontsize\": " + fg(pdf2pt(*doc.bodyfontsize_));
        doc_l = "\"leading\": " + fg(doc.lead_.value_or(0));
    }
    std::string doc_c = "\"columns\": " + std::to_string(doc.ncols_);

    std::ostringstream o;
    o << "{\n";
    if (!opt.no_time) {
        o << "  \"at\": " << long(std::time(nullptr)) << ",\n";
    }
    o << "  " << doc_ps << ",\n  " << doc_margin << ",\n  " << doc_bfs
      << ",\n  " << doc_l << ",\n  " << doc_c << ",\n";
    if (nummargin < 10000) {
        o << "  \"nummargin\": " << f0(nummargin) << ",\n";
    }
    o << "  \"w\": " << nwords << ",\n  \"pages\": [";

    const char* sep = "\n";
    for (size_t i = 0; i < np; ++i) {
        const Page& page = doc.pages_[i];
        const PageDim& pd = px[i];
        std::vector<std::string> val;

        if (opt.pagenum) {
            val.push_back("\"pageno\": \"" + std::to_string(page.index_ + 1) + "\"");
        }
        std::string page_ps = "\"papersize\": [" + f0(pd.ph) + "," + f0(pd.pw) + "]";
        if (page_ps != doc_ps) {
            val.push_back(page_ps);
        }
        if (page.type_ != "body") {
            val.push_back("\"type\": " + quote(page.type_));
        }
        if (page.type_ != "blank") {
            std::string page_margin = "\"margin\": [" + f0(pd.mt) + ","
                + f0(pd.pw - (pd.ml + pd.tw)) + "," + f0(pd.ph - (pd.mt + pd.th))
                + "," + f0(pd.ml) + "]";
            if (page_margin != doc_margin) {
                val.push_back(page_margin);
            }
            if (page.bodyfontsize_) {
                std::string s = "\"bodyfontsize\": " + fg(pdf2pt(page.bodyfontsize_));
                if (s != doc_bfs) {
                    val.push_back(s);
                }
            }
            if (page.reffontsize_
                && !(doc.bodyfontsize_ && page.reffontsize_ == *doc.bodyfontsize_)) {
                val.push_back("\"reffontsize\": " + fg(pdf2pt(page.reffontsize_)));
            }
            if (page.lead_ && *page.lead_) {
                std::string s = "\"leading\": " + fg(*page.lead_);
                if (s != doc_l) {
                    val.push_back(s);
                }
            }
            std::string page_c = "\"columns\": " + std::to_string(page.ncols_);
            // banal withholds this from appendix pages too. We do not: an
            // appendix has a column count worth reporting, even though it does
            // not get to decide the document's -- see page_votes in analyze.cc.
            if (page_c != doc_c && page.type_ != "figure") {
                val.push_back(page_c);
            }
            // The column edges behind that count, in points, as left/right
            // pairs. Printed for every page rather than only where the count
            // differs: the point of asking is to check the geometry, including
            // on the pages where the two implementations agree.
            if (opt.colpos && !page.colpos_.empty()) {
                std::string s = "\"colpos\": [";
                for (size_t i = 0; i != page.colpos_.size(); ++i) {
                    s += (i ? "," : "") + fg(pdf2pt(page.colpos_[i]));
                }
                val.push_back(s + "]");
            }
            val.push_back("\"c\": " + std::to_string(page.nchars_));
            if (page.nwords_ && *page.nwords_ > 0) {
                val.push_back("\"w\": " + std::to_string(*page.nwords_));
            }
            if (opt.pagenum && !page.headings_.empty()) {
                std::string h = "\"headings\": [";
                for (size_t k = 0; k < page.headings_.size(); ++k) {
                    if (k) {
                        h += ",";
                    }
                    h += quote(page.headings_[k]);
                }
                val.push_back(h + "]");
            }
            if (page.first_section_) {
                val.push_back("\"fs\": " + std::to_string(*page.first_section_));
            }
        }

        o << sep << "    {";
        for (size_t k = 0; k < val.size(); ++k) {
            if (k) {
                o << ", ";
            }
            o << val[k];
        }
        o << "}";
        sep = ",\n";
    }
    o << "\n  ]\n}\n";
    std::cout << o.str();
}

}  // namespace mubanal
