/*
 * Xournal++
 *
 * Fixed input benchmark test of the file loading process
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include <config-test.h>
#include <glib-2.0/glib.h>
#include <gtest/gtest.h>

#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/PageRef.h"
#include "model/XojPage.h"
#include "util/PathUtil.h"

#include "filesystem.h"


namespace {

struct BenchmarkSummary {
    double firstMs;
    double medianMs;
    double p95Ms;
    double meanMs;
    double minMs;
    double maxMs;
};

auto getIterationCount(int defaultIterations) -> int {
    const char* value = g_getenv("XOPP_BENCHMARK_ITERATIONS");
    if (value == nullptr) {
        return defaultIterations;
    }

    char* end = nullptr;
    const gint64 parsed = g_ascii_strtoll(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > G_MAXINT) {
        throw std::invalid_argument{"XOPP_BENCHMARK_ITERATIONS must be a positive integer"};
    }
    return static_cast<int>(parsed);
}

auto summarize(std::vector<gint64> samplesUs) -> BenchmarkSummary {
    const double firstMs = static_cast<double>(samplesUs.front()) / 1000.0;
    const double meanMs = static_cast<double>(std::accumulate(samplesUs.begin(), samplesUs.end(), gint64{0})) /
                          static_cast<double>(samplesUs.size()) / 1000.0;

    std::ranges::sort(samplesUs);
    const auto medianIndex = samplesUs.size() / 2;
    const double medianUs = samplesUs.size() % 2 == 0 ?
                                    static_cast<double>(samplesUs[medianIndex - 1] + samplesUs[medianIndex]) / 2.0 :
                                    static_cast<double>(samplesUs[medianIndex]);
    const auto p95Index = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(samplesUs.size()))) - 1;

    return {
            firstMs,
            medianUs / 1000.0,
            static_cast<double>(samplesUs[p95Index]) / 1000.0,
            meanMs,
            static_cast<double>(samplesUs.front()) / 1000.0,
            static_cast<double>(samplesUs.back()) / 1000.0,
    };
}

auto getPeakRssKiB() -> std::optional<long> {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return std::nullopt;
    }
#if defined(__APPLE__)
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
#else
    return std::nullopt;
#endif
}

void benchLoadFile(std::string_view name, const fs::path& filename, int defaultIterations) {
    const int iterations = getIterationCount(defaultIterations);
    std::vector<gint64> samplesUs;
    samplesUs.reserve(static_cast<size_t>(iterations));
    size_t loadedPages = 0;

    for (int i = 0; i < iterations; ++i) {
        const auto start = g_get_monotonic_time();
        const auto doc = LoadHandler{}.loadDocument(filename);
        const auto stop = g_get_monotonic_time();
        samplesUs.emplace_back(stop - start);
        loadedPages += doc->getPageCount();
    }

    const auto summary = summarize(std::move(samplesUs));
    const auto peakRssKiB = getPeakRssKiB();
    std::cout << std::fixed << std::setprecision(3) << "XOPP_BENCHMARK_RESULT {\"name\":\"" << name
              << "\",\"file_bytes\":" << fs::file_size(filename) << ",\"iterations\":" << iterations
              << ",\"first_ms\":" << summary.firstMs << ",\"median_ms\":" << summary.medianMs
              << ",\"p95_ms\":" << summary.p95Ms << ",\"mean_ms\":" << summary.meanMs << ",\"min_ms\":" << summary.minMs
              << ",\"max_ms\":" << summary.maxMs << ",\"peak_rss_kib\":";
    if (peakRssKiB) {
        std::cout << *peakRssKiB;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"loaded_pages\":" << loadedPages << "}\n";
}

}  // namespace

TEST(FileLoadBenchmark, benchmarkHandwrittenText) {
    benchLoadFile("handwritten-text", GET_TESTFILE(u8"benchmark/handwritten-text.xopp"), 25);
}

TEST(FileLoadBenchmark, benchmarkTypedText) {
    benchLoadFile("typed-text", GET_TESTFILE(u8"benchmark/typed-text.xopp"), 5'000);
}

TEST(FileLoadBenchmark, benchmarkLatex) { benchLoadFile("latex", GET_TESTFILE(u8"benchmark/latex.xopp"), 50); }

static auto createTemporaryFile(void (*buildDoc)(Document&), const fs::path& filename) -> fs::path {
    // Build file
    DocumentHandler dh;
    Document doc{&dh};
    buildDoc(doc);

    // Save it to a temporary path
    SaveHandler sh;
    auto tmp_path = Util::getTmpDirSubfolder() / filename;
    sh.prepareSave(&doc, tmp_path);
    sh.saveTo(tmp_path);

    return tmp_path;
}

TEST(FileLoadBenchmark, benchmarkEmpty) {
    // Create empty file (containing only one obligatory page)
    const auto tmp_path = createTemporaryFile(
            [](Document& doc) -> void {
                const PageRef page = std::make_shared<XojPage>(50, 50);
                doc.addPage(page);
            },
            u8"empty.xopp");

    // Benchmark loading time
    benchLoadFile("empty", tmp_path, 100'000);

    // Clean up test file
    fs::remove(tmp_path);
}

TEST(FileLoadBenchmark, benchmarkManyPages) {
    // Create a 500-page file
    const auto tmp_path = createTemporaryFile(
            [](Document& doc) -> void {
                for (int i = 0; i < 500; ++i) {
                    const PageRef page = std::make_shared<XojPage>(50, 50);
                    doc.addPage(page);
                }
            },
            u8"many-pages.xopp");

    // Benchmark loading time
    benchLoadFile("many-pages", tmp_path, 1000);

    // Clean up test file
    fs::remove(tmp_path);
}
