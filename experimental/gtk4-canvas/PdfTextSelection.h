#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <cairo.h>
#include <poppler.h>

namespace prototype {

enum class PdfTextSelectionStyle { Linear, Rectangle };

struct CairoRegionDeleter {
    void operator()(cairo_region_t* region) const {
        if (region) {
            cairo_region_destroy(region);
        }
    }
};

using CairoRegion = std::unique_ptr<cairo_region_t, CairoRegionDeleter>;

struct PdfTextSelectionResult {
    std::string text;
    std::vector<PopplerRectangle> textRects;
    CairoRegion region;
};

namespace pdf_text_selection_detail {

inline cairo_rectangle_int_t cairoRect(const PopplerRectangle& rect) {
    const int x = static_cast<int>(std::floor(std::min(rect.x1, rect.x2)));
    const int y = static_cast<int>(std::floor(std::min(rect.y1, rect.y2)));
    const int right = static_cast<int>(std::ceil(std::max(rect.x1, rect.x2)));
    const int bottom = static_cast<int>(std::ceil(std::max(rect.y1, rect.y2)));
    return {x, y, std::max(0, right - x), std::max(0, bottom - y)};
}

inline bool sameLine(const PopplerRectangle& first, const PopplerRectangle& second) {
    constexpr double tolerance = 1e-5;
    return std::abs(first.y1 - second.y1) < tolerance && std::abs(first.y2 - second.y2) < tolerance;
}

inline void appendMerged(std::vector<PopplerRectangle>& rects, const PopplerRectangle& next,
                         const cairo_region_t* selectionRegion) {
    const auto nextCairo = cairoRect(next);
    if (!rects.empty() && sameLine(rects.back(), next)) {
        auto joined = rects.back();
        joined.x1 = std::min(joined.x1, next.x1);
        joined.x2 = std::max(joined.x2, next.x2);
        const auto joinedCairo = cairoRect(joined);
        if (cairo_region_contains_rectangle(selectionRegion, &joinedCairo) == CAIRO_REGION_OVERLAP_IN) {
            rects.back() = joined;
            return;
        }
    }
    if (cairo_region_contains_rectangle(selectionRegion, &nextCairo) == CAIRO_REGION_OVERLAP_IN) {
        rects.push_back(next);
    }
}

inline bool intersects(const PopplerRectangle& first, const PopplerRectangle& second) {
    return std::min(first.x2, second.x2) > std::max(first.x1, second.x1) &&
           std::min(first.y2, second.y2) > std::max(first.y1, second.y1);
}

}  // namespace pdf_text_selection_detail

inline CairoRegion pdfTextSelectionPreview(PopplerPage* page, const PopplerRectangle& bounds,
                                           PdfTextSelectionStyle style) {
    if (!page) {
        return CairoRegion(cairo_region_create());
    }
    if (style == PdfTextSelectionStyle::Linear) {
        CairoRegion region(poppler_page_get_selected_region(page, 1.0, POPPLER_SELECTION_GLYPH,
                                                            const_cast<PopplerRectangle*>(&bounds)));
        return region ? std::move(region) : CairoRegion(cairo_region_create());
    }

    PopplerRectangle area{std::min(bounds.x1, bounds.x2), std::min(bounds.y1, bounds.y2),
                          std::max(bounds.x1, bounds.x2), std::max(bounds.y1, bounds.y2)};
    const auto rectangle = pdf_text_selection_detail::cairoRect(area);
    return CairoRegion(cairo_region_create_rectangle(&rectangle));
}

inline PdfTextSelectionResult selectPdfText(PopplerPage* page, const PopplerRectangle& bounds,
                                            PdfTextSelectionStyle style) {
    PdfTextSelectionResult result;
    result.region.reset(cairo_region_create());
    if (!page) {
        return result;
    }

    PopplerRectangle* layout = nullptr;
    guint layoutCount = 0;
    if (style == PdfTextSelectionStyle::Linear) {
        result.region.reset(poppler_page_get_selected_region(page, 1.0, POPPLER_SELECTION_GLYPH,
                                                             const_cast<PopplerRectangle*>(&bounds)));
        if (!result.region) {
            result.region.reset(cairo_region_create());
        }
        gchar* selected = poppler_page_get_selected_text(page, POPPLER_SELECTION_GLYPH,
                                                          const_cast<PopplerRectangle*>(&bounds));
        if (selected) {
            result.text = selected;
            g_free(selected);
        }
        if (!poppler_page_get_text_layout(page, &layout, &layoutCount) || !layout || layoutCount == 0) {
            g_free(layout);
            return result;
        }

        for (guint i = 0; i < layoutCount; ++i) {
            pdf_text_selection_detail::appendMerged(result.textRects, layout[i], result.region.get());
        }
        g_free(layout);
        return result;
    }

    PopplerRectangle area{std::min(bounds.x1, bounds.x2), std::min(bounds.y1, bounds.y2),
                          std::max(bounds.x1, bounds.x2), std::max(bounds.y1, bounds.y2)};
    if (!poppler_page_get_text_layout_for_area(page, &area, &layout, &layoutCount) || !layout || layoutCount == 0) {
        g_free(layout);
        return result;
    }

    gchar* text = poppler_page_get_text_for_area(page, &area);
    if (!text) {
        g_free(layout);
        return result;
    }
    const glong textLength = g_utf8_strlen(text, -1);
    if (textLength < 0) {
        g_free(text);
        g_free(layout);
        return result;
    }

    double previousLineY = layout[0].y2;
    bool hasSelectedGlyph = false;
    for (guint i = 0; i < layoutCount; ++i) {
        const auto& rect = layout[i];
        if (!pdf_text_selection_detail::intersects(area, rect)) {
            continue;
        }

        if (hasSelectedGlyph && std::abs(previousLineY - rect.y2) > 1e-5) {
            result.text.push_back('\n');
        }
        if (static_cast<glong>(i) < textLength) {
            const char* start = g_utf8_offset_to_pointer(text, static_cast<glong>(i));
            const char* end = g_utf8_offset_to_pointer(text, static_cast<glong>(i + 1));
            result.text.append(start, static_cast<size_t>(end - start));
        }
        previousLineY = rect.y2;
        hasSelectedGlyph = true;

        bool merged = false;
        if (!result.textRects.empty() && pdf_text_selection_detail::sameLine(result.textRects.back(), rect)) {
            const auto previous = result.textRects.back();
            const auto joinedLeft = std::min(std::min(previous.x1, previous.x2), std::min(rect.x1, rect.x2));
            const auto joinedRight = std::max(std::max(previous.x1, previous.x2), std::max(rect.x1, rect.x2));
            if (joinedLeft >= area.x1 && joinedRight <= area.x2) {
                result.textRects.back().x1 = joinedLeft;
                result.textRects.back().x2 = joinedRight;
                merged = true;
            }
        }
        if (!merged) {
            result.textRects.push_back(rect);
        }
    }

    g_free(text);
    g_free(layout);
    for (const auto& rect: result.textRects) {
        auto cairoRect = pdf_text_selection_detail::cairoRect(rect);
        cairo_region_union_rectangle(result.region.get(), &cairoRect);
    }
    return result;
}

}  // namespace prototype
