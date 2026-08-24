/*
 * Xournal++
 *
 * Tests for Poppler-backed PDF page helpers.
 */

#include <memory>
#include <mutex>

#include <config-test.h>
#include <gtest/gtest.h>
#include <poppler.h>

#include "pdf/popplerapi/PopplerGlibPage.h"

TEST(PopplerGlibPageTest, detectsGlyphsButNotPageMargins) {
    const auto filename = GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf");
    GError* error = nullptr;
    gchar* uri = g_filename_to_uri(reinterpret_cast<const gchar*>(filename.c_str()), nullptr, &error);
    ASSERT_NE(uri, nullptr) << (error ? error->message : "could not create PDF URI");

    PopplerDocument* document = poppler_document_new_from_file(uri, nullptr, &error);
    g_free(uri);
    ASSERT_NE(document, nullptr) << (error ? error->message : "could not open PDF fixture");

    PopplerPage* page = poppler_document_get_page(document, 0);
    ASSERT_NE(page, nullptr);

    {
        PopplerGlibPage pdfPage(page, document, std::make_shared<std::mutex>());
        EXPECT_TRUE(pdfPage.hasTextAt(60.0, 65.0));
        EXPECT_FALSE(pdfPage.hasTextAt(10.0, 10.0));
    }

    g_object_unref(page);
    g_object_unref(document);
    if (error) {
        g_error_free(error);
    }
}
