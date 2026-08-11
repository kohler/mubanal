// main.cc -- mubanal command line.
//
// Deliberately a subset of banal's interface: only `-json` matters, because
// that is the only invocation in the HotCRP tree (src/checkformat.php:92).
// `-zoom` is gone; it was a pdftohtml artifact.

#include "mubanal.hh"

#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char* kVersion = "1.2";

void usage(std::ostream& out) {
    out << "usage: mubanal [-json] [-p] [-no-time] [--dump-runs] FILE.pdf\n"
           "       mubanal --grep=FEATURE FILE.pdf...\n\n"
           "  -json         print JSON output (default; the only supported mode)\n"
           "  -p, -pagenum  include page numbers and headings\n"
           "  -C, -colpos   add per-page \"colpos\", the column edges\n"
           "  -no-time      omit the \"at\" timestamp\n"
           "  -no-unsafe    skip the dangerous-feature scan (no \"unsafe\" output)\n"
           "  --grep=FEATURE  print each matching FILE's name instead of JSON;\n"
           "                FEATURE: unsafe, error, problem\n"
           "  --dump-runs   print the extracted run list instead of analysing\n"
           "  --runs=FILE   analyse a stored run list instead of a PDF\n"
           "  --columns=ALG  column detection: gutter (default), mode\n"
           "  --rot=MODE    rotated text: skip (default), skew, keep\n"
           "  --debug-columns trace column detection on stderr\n"
           "  --trim=MODE   trim run edges: ascii (default), none\n"
           "  --stext=OPTS  MuPDF stext options, comma separated (calibration);\n"
           "                accurate-bboxes is on by default, no-accurate-bboxes\n"
           "                turns it off\n"
           "  -V, --verbose  print detailed errors and warnings to stderr\n"
           "  -version      print version\n";
}

bool opt_is(std::string_view a, std::string_view name, std::string_view* arg = nullptr) {
    if (a.empty() || a[0] != '-') {
        return false;
    }
    size_t i = 1;
    if (a.size() > 1 && a[1] == '-' && name.size() > 1) {
        ++i;
    }
    if (a.compare(i, name.size(), name) != 0) {
        return false;
    }
    if (arg) {
        if (a.size() == i + name.size() || a[i + name.size()] != '=') {
            return false;
        }
        *arg = a.substr(i + name.size() + 1);
    } else if (a.size() != i + name.size()) {
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace mubanal;
    ReportOpts ropt;
    Rot rot = Rot::Skip;
    Trim trim = Trim::Ascii;
    bool dump = false;
    bool unsafe = true;
    std::string runs_file, grep;
    std::vector<std::string> files;
    // Accurate bboxes by default. Without them MuPDF reports each glyph's
    // declared font box, which for a math symbol set inline in a text line can
    // be half again as tall as the line and hang well below it -- 17.3pt
    // against 12.0pt at the same nominal size, on the same baseline. That
    // inflates the text block downward and raises `textblock` complaints
    // against papers whose text block is fine, which is the worst direction
    // for a format check to be wrong in.
    int stext_flags = mubanal::stext_default_flags();

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i], arg;
        if (opt_is(a, "json")) {
            // ignore (banal compatibility)
        } else if (opt_is(a, "p") || opt_is(a, "pagenum") || opt_is(a, "pageno")
                   || opt_is(a, "headings") || opt_is(a, "headers")) {
            ropt.pagenum = true;
        } else if (opt_is(a, "colpos") || opt_is(a, "C")) {
            ropt.colpos = true;
        } else if (opt_is(a, "no-time") || opt_is(a, "notime")
                   || opt_is(a, "no-timestamp")) {
            ropt.no_time = true;
        } else if (opt_is(a, "no-unsafe")) {
            unsafe = false;
        } else if (opt_is(a, "zoom", &arg)) {
            // ignore (banal compatibility)
        } else if (opt_is(a, "zoom")) {
            // ignore (banal compatibility)
            i += i + 1 < argc;
        } else if (opt_is(a, "V") || opt_is(a, "verbose")) {
            mubanal::verbose = true;
        } else if (opt_is(a, "dump-runs")) {
            dump = true;
        } else if (opt_is(a, "grep", &arg)) {
            grep = arg;
        } else if (opt_is(a, "grep")) {
            if (i + 1 == argc) {
                std::cerr << "mubanal: --grep requires an argument\n";
                return 2;
            }
            grep = argv[++i];
        } else if (opt_is(a, "runs", &arg)) {
            runs_file = arg;
        } else if (opt_is(a, "stext", &arg)) {
            // calibration hook: comma-separated MuPDF stext option names
            for (size_t p = 0; p < arg.size(); ) {
                size_t q = arg.find(',', p);
                auto name = arg.substr(p, q == std::string::npos ? q : q - p);
                // A leading "no-" clears the option instead of setting it,
                // which is the only way to switch off one that is on by
                // default.
                bool off = name.substr(0, 3) == "no-";
                int flag = stext_flag(off ? name.substr(3) : name);
                if (flag >= 0) {
                    stext_flags = off ? (stext_flags & ~flag) : (stext_flags | flag);
                } else if (!name.empty()) {
                    std::cerr << "mubanal: unknown stext option " << name << "\n";
                    return 1;
                }
                if (q == std::string::npos) {
                    break;
                }
                p = q + 1;
            }
        } else if (opt_is(a, "trim", &arg)) {
            trim = arg == "none" ? Trim::None : Trim::Ascii;
        } else if (opt_is(a, "columns", &arg)) {
            if (arg == "gutter") {
                column_algo = ColumnAlgo::Gutter;
            } else if (arg == "mode") {
                column_algo = ColumnAlgo::Mode;
            } else {
                std::cerr << "mubanal: unknown column algorithm " << arg << "\n";
                return 1;
            }
        } else if (opt_is(a, "debug-columns")) {
            debug_columns = true;
        } else if (opt_is(a, "rot", &arg)) {
            rot = arg == "keep" ? Rot::Keep : (arg == "skew" ? Rot::Skew : Rot::Skip);
        } else if (opt_is(a, "version") || opt_is(a, "v")) {
            std::cout << "Banal version " << kVersion << ".\n";
            return 0;
        } else if (opt_is(a, "help") || opt_is(a, "h")) {
            usage(std::cout);
            return 0;
        } else if (a.size() > 1 && a[0] == '-') {
            std::cerr << "mubanal: bad option " << a << "\n";
            usage(std::cerr);
            return 1;
        } else if (files.empty() || !grep.empty()) {
            files.emplace_back(a);
        } else {
            std::cerr << "mubanal: only one input file is supported\n";
            return 1;
        }
    }

    if (files.empty() && runs_file.empty()) {
        usage(std::cerr);
        return 1;
    }

    // Grep mode prints matching filenames and exits like grep: 0 match,
    // 1 none, 2 error. `unsafe` needs only the object-graph scan, so the
    // pages are never extracted.
    if (!grep.empty()) {
        if (grep != "unsafe" && grep != "problem" && grep != "error") {
            std::cerr << "mubanal: unknown --grep argument " << grep << "\n";
            return 2;
        }
        if (!runs_file.empty() || dump) {
            std::cerr << "mubanal: --grep is incompatible with --runs and --dump-runs\n";
            return 2;
        }
        if (!unsafe && grep == "unsafe") {
            std::cerr << "mubanal: --grep unsafe conflicts with -no-unsafe\n";
            return 2;
        }
        int status = 1;
        for (const std::string& f : files) {
            bool match = false;
            try {
                auto src = open_pdf(f, rot, trim, stext_flags);
                if (grep == "unsafe") {
                    // needs only the object-graph scan
                    match = !src->unsafe().empty();
                } else {
                    // Diagnostics surface during the work -json would do:
                    // pull every page, then scan.
                    for (size_t p = 0; p != src->npages(); ++p) {
                        src->page(p);
                    }
                    if (unsafe) {
                        src->unsafe();
                    }
                    match = src->error() || (grep == "problem" && src->warning());
                }
            } catch (const std::exception& e) {
                // An unopenable document reports {"error": true}, which the
                // warning and error greps match. For unsafe it is a failure:
                // whether it has unsafe features was not determined.
                if (grep == "unsafe") {
                    std::cerr << f << ": Error: " << e.what() << "\n";
                    status = 2;
                    continue;
                }
                match = true;
            }
            if (match) {
                std::cout << f << "\n";
                status = status == 1 ? 0 : status;
            }
        }
        return status;
    }

    const std::string& file = runs_file.empty() ? files[0] : runs_file;
    std::unique_ptr<PageSource> src;
    try {
        src = runs_file.empty() ? open_pdf(file, rot, trim, stext_flags)
            : open_runs(runs_file);
    } catch (const std::exception& e) {
        std::cerr << file << ": Error: " << e.what() << "\n";
        report_error(ropt);
        return 1;
    }

    if (dump) {
        dump_runs(*src);
        return 0;
    }
    report_json(analyze(*src, unsafe), ropt);
    return 0;
}
