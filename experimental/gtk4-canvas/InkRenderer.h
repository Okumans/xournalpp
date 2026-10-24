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
    const bool whiteout = ink.brush.tool == Tool::Eraser;
    if (highlight) {
        // Composite a stroke once so overlapping segments have uniform opacity.
        cairo_push_group(cr);
    }
    const auto& color = whiteout ? behavior::whiteoutColor : ink.brush.color;
    cairo_set_source_rgb(cr, color[0], color[1], color[2]);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (size_t i = 0; i < ink.points.size(); ++i) {
        const auto& a = ink.points[i ? i - 1 : 0];
        const auto& b = ink.points[i];
        const auto width = ink.brush.width * ((highlight || whiteout) ? 1.0 : (a.pressure + b.pressure) / 2);
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

inline void drawInkPreview(cairo_t* cr, const CanvasState& state, size_t index) {
    const auto translation = state.selectionOffset(index);
    if (translation.x == 0 && translation.y == 0) {
        drawInk(cr, state.ink.at(index));
        return;
    }
    cairo_save(cr);
    cairo_translate(cr, translation.x, translation.y);
    drawInk(cr, state.ink.at(index));
    cairo_restore(cr);
}

inline void drawSelection(cairo_t* cr, const CanvasState& state, int page) {
    const auto bounds = state.selectionBox();
    if (!bounds || bounds->page != page) {
        return;
    }
    const auto padding = state.selecting() && state.selected.empty() ? 0.0 : 4 / state.zoom;
    cairo_save(cr);
    cairo_rectangle(cr, bounds->x0 - padding, bounds->y0 - padding,
                    bounds->x1 - bounds->x0 + 2 * padding, bounds->y1 - bounds->y0 + 2 * padding);
    cairo_set_source_rgba(cr, 0.2, 0.52, 0.9, 0.08);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.2, 0.52, 0.9, 1);
    cairo_set_line_width(cr, 1.5 / state.zoom);
    const double dashes[] = {5 / state.zoom, 3 / state.zoom};
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_stroke(cr);
    cairo_restore(cr);
}

}  // namespace prototype
