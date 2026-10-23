#pragma once

#include <algorithm>
#include <cmath>

#include <cairo.h>

#include "control/ToolEnums.h"
#include "util/raii/CairoWrappers.h"
#include "util/safe_casts.h"

namespace xoj::gui {

inline double eraserCursorFootprint(double thickness, double zoom, EraserType mode) {
    return thickness * zoom * (mode == ERASER_TYPE_WHITEOUT ? 1.0 : 2.0);
}

struct EraserCursorImage {
    xoj::util::CairoSurfaceSPtr surface;
    double hotspot;
};

inline EraserCursorImage createEraserCursorImage(double thickness, double zoom, EraserType mode, int scale) {
    const bool round = mode == ERASER_TYPE_WHITEOUT;
    const double footprint = eraserCursorFootprint(thickness, zoom, mode);
    constexpr double margin = 2.0;
    const int size = std::max(5, ceil_cast<int>(footprint + 2 * margin));
    const double center = size / 2.0;
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size * scale, size * scale);
    cairo_surface_set_device_scale(surface, scale, scale);
    auto* cr = cairo_create(surface);
    if (round) {
        cairo_arc(cr, center, center, footprint / 2, 0, 2 * M_PI);
    } else {
        cairo_rectangle(cr, center - footprint / 2, center - footprint / 2, footprint, footprint);
    }
    // A two-tone outline stays visible without hiding the content being erased.
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_set_line_width(cr, 3);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    if (mode == ERASER_TYPE_DELETE_STROKE) {
        cairo_move_to(cr, center - 2, center - 2);
        cairo_line_to(cr, center + 2, center + 2);
        cairo_move_to(cr, center - 2, center + 2);
        cairo_line_to(cr, center + 2, center - 2);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    return {xoj::util::CairoSurfaceSPtr(surface, xoj::util::adopt), center};
}

}  // namespace xoj::gui
