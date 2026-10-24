#pragma once

#include <cairo.h>

#include "BehaviorDefaults.h"
#include "CanvasState.h"

namespace prototype {

inline void drawInk(cairo_t* cr, const Ink& ink) {
    if (ink.points.empty()) {
        return;
    }
    cairo_save(cr);
    const bool highlight = ink.brush.tool == Tool::Highlighter;
    if (highlight) {
        // Composite a stroke once so overlapping segments have uniform opacity.
        cairo_push_group(cr);
    }
    const auto& color = ink.brush.color;
    cairo_set_source_rgb(cr, color[0], color[1], color[2]);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (size_t i = 0; i < ink.points.size(); ++i) {
        const auto& a = ink.points[i ? i - 1 : 0];
        const auto& b = ink.points[i];
        const auto width = ink.brush.width * (highlight ? 1.0 : (a.pressure + b.pressure) / 2);
        if (a.x == b.x && a.y == b.y) {
            cairo_arc(cr, b.x, b.y, width / 2, 0, 6.283185307179586);
            cairo_fill(cr);
        } else {
            cairo_set_line_width(cr, width);
            cairo_move_to(cr, a.x, a.y);
            cairo_line_to(cr, b.x, b.y);
            cairo_stroke(cr);
        }
    }
    if (highlight) {
        cairo_pop_group_to_source(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
        cairo_paint_with_alpha(cr, behavior::highlightOpacity);
    }
    cairo_restore(cr);
}

}  // namespace prototype
