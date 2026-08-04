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

namespace {

constexpr const char* kVersion = "1.2";

void usage(std::ostream& out) {
    out << "usage: mubanal [-json] [-p] [-no-time] [--dump-runs] FILE.pdf\n\n"
           "  -json         print JSON output (default; the only supported mode)\n"
           "  -p, -pagenum  include page numbers and headings\n"
           "  -C, -colpos   add per-page \"colpos\", the column edges\n"
           "  -no-time      omit the \"at\" timestamp\n"
           "  --dump-runs   print the extracted run list instead of analysing\n"
           "  --runs=FILE   analyse a stored run list instead of a PDF\n"
           "  --rot=MODE    rotated text: skip (default), skew, keep\n"
           "  --trim=MODE   trim run edges: ascii (default), none\n"
           "  --stext=OPTS  MuPDF stext options, comma separated (calibration)\n"
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
    std::string runs_file, file;
    int stext_flags = 0;

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
        } else if (opt_is(a, "zoom", &arg)) {
            // ignore (banal compatibility)
        } else if (opt_is(a, "zoom")) {
            // ignore (banal compatibility)
            i += i + 1 < argc;
        } else if (opt_is(a, "dump-runs")) {
            dump = true;
        } else if (opt_is(a, "runs", &arg)) {
            runs_file = arg;
        } else if (opt_is(a, "stext", &arg)) {
            // calibration hook: comma-separated MuPDF stext option names
            for (size_t p = 0; p < arg.size(); ) {
                size_t q = arg.find(',', p);
                auto name = arg.substr(p, q == std::string::npos ? q : q - p);
                if (name == "ligatures") {
                    stext_flags |= 1;
                } else if (name == "whitespace") {
                    stext_flags |= 2;
                } else if (name == "inhibit-spaces") {
                    stext_flags |= 8;
                } else if (name == "dehyphenate") {
                    stext_flags |= 16;
                } else if (name == "spans") {
                    stext_flags |= 32;
                } else if (name == "accurate-bboxes") {
                    stext_flags |= 512;
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
        } else if (file.empty()) {
            file = a;
        } else {
            std::cerr << "mubanal: only one input file is supported\n";
            return 1;
        }
    }

    if (file.empty() && runs_file.empty()) {
        usage(std::cerr);
        return 1;
    }

    std::unique_ptr<PageSource> src;
    try {
        src = runs_file.empty() ? open_pdf(file, rot, trim, stext_flags)
            : open_runs(runs_file);
    } catch (const std::exception& e) {
        std::cerr << (runs_file.empty() ? file : runs_file)
                  << ": Error: " << e.what() << "\n";
        report_error(ropt);
        return 1;
    }

    if (dump) {
        dump_runs(*src);
        return 0;
    }
    report_json(analyze(*src), ropt);
    return 0;
}
