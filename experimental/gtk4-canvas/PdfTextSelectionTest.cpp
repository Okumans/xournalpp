#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include <glib.h>
#include <poppler.h>

#include "PdfTextSelection.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const auto path = std::filesystem::path(XOURNALPP_REPO_ROOT) / "test/files/cjk/测试.pdf";
        gchar* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        require(uri != nullptr, "Could not make a PDF URI");
        GError* error = nullptr;
        PopplerDocument* document = poppler_document_new_from_file(uri, nullptr, &error);
        g_free(uri);
        if (!document) {
            const std::string detail = error ? error->message : "unknown Poppler error";
            g_clear_error(&error);
            throw std::runtime_error("Could not open PDF fixture: " + detail);
        }

        PopplerPage* page = poppler_document_get_page(document, 0);
        require(page != nullptr, "PDF fixture has no first page");
        double width = 0;
        double height = 0;
        poppler_page_get_size(page, &width, &height);
        const PopplerRectangle wholePage{0, 0, width, height};
        auto linear = prototype::selectPdfText(page, wholePage, prototype::PdfTextSelectionStyle::Linear);
        require(!linear.text.empty(), "Linear PDF selection returned no text");
        require(!linear.textRects.empty(), "Linear PDF selection returned no glyph rectangles");
        require(cairo_region_num_rectangles(linear.region.get()) > 0, "Linear PDF selection returned no region");

        PopplerRectangle* glyphs = nullptr;
        guint glyphCount = 0;
        require(poppler_page_get_text_layout(page, &glyphs, &glyphCount) && glyphCount > 0,
                "PDF fixture has no text layout");
        const guint selectedCount = std::min(glyphCount, 8u);
        const auto firstGlyph = glyphs[0];
        PopplerRectangle area{glyphs[0].x1 - 1, glyphs[0].y1 - 1, glyphs[selectedCount - 1].x2 + 1,
                             glyphs[selectedCount - 1].y2 + 1};
        g_free(glyphs);

        auto rectangular = prototype::selectPdfText(page, area, prototype::PdfTextSelectionStyle::Rectangle);
        require(!rectangular.text.empty(), "Rectangular PDF selection returned no text");
        require(!rectangular.textRects.empty(), "Rectangular PDF selection returned no glyph rectangles");
        require(cairo_region_num_rectangles(rectangular.region.get()) > 0,
                "Rectangular PDF selection returned no region");

        const auto preview = prototype::pdfTextSelectionPreview(page, area,
                                                               prototype::PdfTextSelectionStyle::Rectangle);
        require(cairo_region_num_rectangles(preview.get()) > 0, "Rectangular drag preview is empty");

        PopplerRectangle wordBounds{firstGlyph.x1 - 1, firstGlyph.y1 - 1, firstGlyph.x2 + 1, firstGlyph.y2 + 1};
        auto word = prototype::selectPdfText(page, wordBounds, prototype::PdfTextSelectionStyle::Word);
        require(!word.text.empty() && !word.textRects.empty() && cairo_region_num_rectangles(word.region.get()) > 0,
                "Double-click word selection returned no text or geometry");
        auto line = prototype::selectPdfText(page, wordBounds, prototype::PdfTextSelectionStyle::Line);
        require(line.text.find("Test PDF for Xournal++") != std::string::npos && !line.textRects.empty() &&
                        cairo_region_num_rectangles(line.region.get()) > 0,
                "Triple-click line selection did not expand to the whole PDF line");
        g_object_unref(page);
        g_object_unref(document);
        std::cout << "PASS: linear, rectangular, word and line PDF text extraction, glyph rectangles, and preview regions\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
