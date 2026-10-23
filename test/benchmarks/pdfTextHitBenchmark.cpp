#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>

#include <config-test.h>
#include <glib.h>
#include <gtest/gtest.h>
#include <poppler.h>

#include "pdf/popplerapi/PopplerGlibPage.h"

// Compare the pre-cache algorithm against the current helper on identical
// coordinates. This is a microbenchmark, not an end-to-end UI speedup claim.
TEST(PdfTextHitBenchmark, repeatedPointerQueries) {
    const auto filename = GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf");
    gchar* uri = g_filename_to_uri(reinterpret_cast<const gchar*>(filename.c_str()), nullptr, nullptr);
    ASSERT_NE(uri, nullptr);
    auto* document = poppler_document_new_from_file(uri, nullptr, nullptr);
    g_free(uri);
    ASSERT_NE(document, nullptr);
    auto* page = poppler_document_get_page(document, 0);
    ASSERT_NE(page, nullptr);
    {
        PopplerGlibPage cached(page, document, std::make_shared<std::mutex>());
        auto original = [&](double x, double y) {
            PopplerRectangle* rects = nullptr;
            guint count = 0;
            const bool valid = poppler_page_get_text_layout(page, &rects, &count);
            const bool hit = valid && rects && std::any_of(rects, rects + count, [=](const auto& r) {
                                 return std::min(r.x1, r.x2) <= x && x <= std::max(r.x1, r.x2) &&
                                        std::min(r.y1, r.y2) <= y && y <= std::max(r.y1, r.y2);
                             });
            g_free(rects);
            return hit;
        };
        constexpr int queries = 2000;
        auto measure = [&](auto&& query) {
            const auto start = g_get_monotonic_time();
            int hits = 0;
            for (int i = 0; i < queries; ++i) {
                hits += query(i % 2 ? 60.0 : 10.0, i % 2 ? 65.0 : 10.0);
            }
            EXPECT_EQ(hits, queries / 2);
            return static_cast<double>(g_get_monotonic_time() - start) / queries;
        };
        const auto coldStart = g_get_monotonic_time();
        EXPECT_TRUE(cached.hasTextAt(60, 65));
        const auto coldUs = g_get_monotonic_time() - coldStart;
        for (int round = 0; round < 5; ++round) {
            double originalUs, cachedUs;
            auto cachedQuery = [&](double x, double y) { return cached.hasTextAt(x, y); };
            if (round % 2) {
                cachedUs = measure(cachedQuery);
                originalUs = measure(original);
            } else {
                originalUs = measure(original);
                cachedUs = measure(cachedQuery);
            }
            std::cout << "XOPP_PDF_HIT_BENCHMARK {\"round\":" << round + 1 << ",\"queries\":" << queries
                      << ",\"cold_cached_us\":" << coldUs << ",\"original_us_per_query\":" << originalUs
                      << ",\"cached_us_per_query\":" << cachedUs << "}\n";
        }
    }
    g_object_unref(page);
    g_object_unref(document);
}
