#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <cairo.h>
#include <pango/pangocairo.h>

#include "CanvasText.h"

namespace prototype {

inline PangoLayout* createTextLayout(cairo_t* cr, const TextItem& item) {
    auto* layout = pango_cairo_create_layout(cr);
    auto* context = pango_layout_get_context(layout);
    pango_context_set_matrix(context, nullptr);
    pango_context_set_round_glyph_positions(context, false);

    auto* description = pango_font_description_from_string(item.fontName.c_str());
    pango_font_description_set_absolute_size(description, item.fontSize * PANGO_SCALE);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    pango_layout_set_text(layout, item.text.data(), static_cast<int>(item.text.size()));
    pango_layout_set_width(layout, item.wrapWidth < 0 ? -1 : static_cast<int>(item.wrapWidth * PANGO_SCALE));
    pango_layout_set_alignment(layout, static_cast<PangoAlignment>(item.alignment));
    pango_layout_set_justify(layout, item.justify);
#if PANGO_VERSION_CHECK(1, 48, 5)
    pango_layout_set_line_spacing(layout, 1.0);
#endif

    auto* attributes = pango_attr_list_new();
    for (const auto& run: item.styleRuns) {
        if (run.start >= run.end || run.end > item.text.size() || run.start > G_MAXUINT || run.end > G_MAXUINT) {
            continue;
        }
        auto add = [&](PangoAttribute* attribute) {
            attribute->start_index = static_cast<guint>(run.start);
            attribute->end_index = static_cast<guint>(run.end);
            pango_attr_list_insert(attributes, attribute);
        };

        auto* runFont = pango_font_description_from_string(run.fontName.c_str());
        pango_font_description_set_absolute_size(runFont, run.fontSize * PANGO_SCALE);
        add(pango_attr_font_desc_new(runFont));
        pango_font_description_free(runFont);

        if (run.attributes & 1U) {
            add(pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        }
        if (run.attributes & 2U) {
            add(pango_attr_strikethrough_new(true));
        }
        if (run.attributes & (4U | 8U)) {
            add(pango_attr_scale_new(0.75));
            const double rise = run.attributes & 4U ? run.fontSize * 0.4 : -run.fontSize * 0.2;
            add(pango_attr_rise_new(static_cast<int>(rise * PANGO_SCALE)));
        }
        if (run.color) {
            const auto color = *run.color;
            const auto red = static_cast<guint16>(((color >> 16U) & 0xffU) * 257U);
            const auto green = static_cast<guint16>(((color >> 8U) & 0xffU) * 257U);
            const auto blue = static_cast<guint16>((color & 0xffU) * 257U);
            add(pango_attr_foreground_new(red, green, blue));
            add(pango_attr_foreground_alpha_new(static_cast<guint16>(((color >> 24U) & 0xffU) * 257U)));
        }
    }
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
    return layout;
}

inline void drawText(cairo_t* cr, const TextItem& item) {
    if (item.text.empty() || item.text.size() > G_MAXINT) {
        return;
    }
    cairo_save(cr);
    const cairo_matrix_t transform{item.xx, item.yx, item.xy, item.yy, item.x, item.y};
    cairo_transform(cr, &transform);
    cairo_set_source_rgba(cr, ((item.color >> 16U) & 0xffU) / 255.0,
                          ((item.color >> 8U) & 0xffU) / 255.0, (item.color & 0xffU) / 255.0,
                          ((item.color >> 24U) & 0xffU) / 255.0);
    auto* layout = createTextLayout(cr, item);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_restore(cr);
}

inline bool textContainsPoint(const TextItem& item, double x, double y) {
    if (item.text.empty() || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    const double determinant = item.xx * item.yy - item.xy * item.yx;
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
        return false;
    }
    const double tx = x - item.x;
    const double ty = y - item.y;
    const double localX = (item.yy * tx - item.xy * ty) / determinant;
    const double localY = (-item.yx * tx + item.xx * ty) / determinant;

    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto* cr = cairo_create(surface);
    auto* layout = createTextLayout(cr, item);
    PangoRectangle logical{};
    pango_layout_get_extents(layout, nullptr, &logical);
    const auto px = static_cast<int>(localX * PANGO_SCALE);
    const auto py = static_cast<int>(localY * PANGO_SCALE);
    const bool inside = px >= logical.x - PANGO_SCALE && py >= logical.y - PANGO_SCALE &&
                        px <= logical.x + logical.width + PANGO_SCALE &&
                        py <= logical.y + logical.height + PANGO_SCALE;
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return inside;
}

inline std::pair<int, int> measureText(const TextItem& item) {
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto* cr = cairo_create(surface);
    auto* layout = createTextLayout(cr, item);
    int width = 0;
    int height = 0;
    pango_layout_get_pixel_size(layout, &width, &height);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return {width, height};
}

inline std::pair<double, double> textNaturalSize(const TextItem& item) {
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto* cr = cairo_create(surface);
    auto* layout = createTextLayout(cr, item);
    PangoRectangle logical{};
    pango_layout_get_extents(layout, nullptr, &logical);
    const double offsetX = static_cast<double>(logical.x) / PANGO_SCALE;
    const double offsetY = static_cast<double>(logical.y) / PANGO_SCALE;
    const double width = item.wrapWidth < 0 ? static_cast<double>(logical.width) / PANGO_SCALE + offsetX :
                                              item.wrapWidth;
    const double height = static_cast<double>(logical.height) / PANGO_SCALE + offsetY;
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return {width, height};
}

}  // namespace prototype
