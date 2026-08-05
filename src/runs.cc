// runs.cc -- read and write a run list as JSON.
//
// Together these split the tool in half. Dump the run list from one build and
// feed it to another with `--runs=FILE`: if the lists are byte-identical, a
// disagreement is in the analysis rather than in text extraction.
//
// The format is also what an external extractor would emit, which is how the
// port was originally checked against banal's own pdftohtml input.
//
// Format, matching --dump-runs:
//   {"pages":[{"w":N,"h":N,"runs":[[top,left,w,h,size,"font",lum,"text",base],...]}]}

#include "mubanal.hh"

#include <json/json.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace mubanal {

namespace {

// The whole file is parsed up front: it is a diagnostic path, the input is a
// run list we produced, and there is no streaming JSON reader to hand.
class RunsSource : public PageSource {
public:
    explicit RunsSource(const std::string& path) {
        Json::Value j;
        Json::CharReaderBuilder rb;
        std::string errs;
        bool ok;
        if (path == "-") {
            ok = Json::parseFromStream(rb, std::cin, &j, &errs);
        } else {
            std::ifstream in(path);
            if (!in) {
                throw std::runtime_error("cannot open " + path);
            }
            ok = Json::parseFromStream(rb, in, &j, &errs);
        }
        if (!ok) {
            throw std::runtime_error("cannot parse " + path + ": " + errs);
        }
        const Json::Value& jpages = j["pages"];
        if (!jpages.isArray()) {
            throw std::runtime_error("no run pages in " + path);
        }
        for (const Json::Value& jp : jpages) {
            RawPage page;
            page.w = jp.get("w", 0.0).asDouble();
            page.h = jp.get("h", 0.0).asDouble();
            for (const Json::Value& jr : jp["runs"]) {
                if (jr.size() < 8) {
                    throw std::runtime_error("malformed run in " + path);
                }
                Run run;
                run.t = jr[0].asDouble();
                run.l = jr[1].asDouble();
                run.r = run.l + jr[2].asDouble();
                run.b = run.t + jr[3].asDouble();
                run.size = jr[4].asDouble();
                // Older run lists have no baseline; 0 falls back to the top.
                run.base = jr.size() > 8 ? jr[8].asDouble() : 0.0;
                run.lum = jr[6].asDouble();
                run.text = jr[7].asString();
                run.nchars = int(u8_length(run.text));
                page.runs.push_back(std::move(run));
            }
            pages_.push_back(std::move(page));
        }
    }

    size_t npages() const override {
        return int(pages_.size());
    }

    RawPage page(size_t index) override {
        return pages_[index];
    }

private:
    std::vector<RawPage> pages_;
};

}  // namespace

std::unique_ptr<PageSource> open_runs(const std::string& path) {
    return std::make_unique<RunsSource>(path);
}

void dump_runs(PageSource& src) {
    Json::Value j;
    j["pages"] = Json::Value(Json::arrayValue);
    for (size_t i = 0; i < src.npages(); ++i) {
        RawPage p = src.page(i);
        Json::Value jp;
        jp["w"] = p.w;
        jp["h"] = p.h;
        Json::Value jruns(Json::arrayValue);
        for (const auto& run : p.runs) {
            Json::Value jr(Json::arrayValue);
            jr.append(run.t);
            jr.append(run.l);
            jr.append(run.r - run.l);
            jr.append(run.b - run.t);
            jr.append(run.size);
            jr.append("");
            jr.append(run.lum);
            jr.append(run.text);
            jr.append(run.base);
            jruns.append(std::move(jr));
        }
        jp["runs"] = std::move(jruns);
        j["pages"].append(std::move(jp));
    }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";        // one line, as before
    wb["emitUTF8"] = true;         // run text is UTF-8; do not escape it
    std::cout << Json::writeString(wb, j) << "\n";
}

}  // namespace mubanal
