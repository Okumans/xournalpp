#pragma once

#include <algorithm>

#include <cairo.h>

#include "BehaviorDefaults.h"
#include "CanvasState.h"
#include "model/Point.h"
#include "model/StrokeContour.h"

namespace prototype {

void drawCanvasImage(cairo_t* cr, const CanvasImage& image);
void drawText(cairo_t* cr, const TextItem& item);

inline void drawInk(cairo_t* cr, const Ink& ink, const SelectionTransform& matrix = {}) {
    if (ink.points.empty()) {
        return;
    }
    cairo_save(cr);
    const bool highlight = ink.brush.tool == Tool::Highlighter ||
                           ink.brush.tool == Tool::LaserPointerHighlighter;
    const bool whiteout = ink.brush.tool == Tool::Eraser;
    if (highlight) {
        // Composite a stroke once so overlapping segments have uniform opacity.
        cairo_push_group(cr);
    }
    const auto& color = whiteout ? behavior::whiteoutColor : ink.brush.color;
    cairo_set_source_rgb(cr, color[0], color[1], color[2]);
    const auto capStyle = ink.capStyle == 1 ? CAIRO_LINE_CAP_BUTT :
                          ink.capStyle == 2 ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_ROUND;
    cairo_set_line_cap(cr, capStyle);
    if (ink.fill >= 0 && ink.brush.tool == Tool::Pen && ink.points.size() > 2) {
        const auto first = matrix.apply(ink.points.front());
        cairo_move_to(cr, first.x, first.y);
        for (size_t i = 1; i < ink.points.size(); ++i) {
            const auto point = matrix.apply(ink.points[i]);
            cairo_line_to(cr, point.x, point.y);
        }
        cairo_set_source_rgba(cr, color[0], color[1], color[2], static_cast<double>(ink.fill) / 255.0);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, color[0], color[1], color[2]);
    } else if (highlight && ink.fill >= 0 && ink.brush.tool == Tool::Highlighter && ink.points.size() > 2) {
        const auto first = matrix.apply(ink.points.front());
        cairo_move_to(cr, first.x, first.y);
        for (size_t i = 1; i < ink.points.size(); ++i) {
            const auto point = matrix.apply(ink.points[i]);
            cairo_line_to(cr, point.x, point.y);
        }
        cairo_fill(cr);
    }
    if (!ink.pressureSensitive && ink.points.size() > 1) {
        cairo_set_line_width(cr, ink.brush.width * matrix.widthScale);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        if (ink.brush.lineDashes.empty()) {
            cairo_set_dash(cr, nullptr, 0, 0);
        } else {
            cairo_set_dash(cr, ink.brush.lineDashes.data(), static_cast<int>(ink.brush.lineDashes.size()), 0);
        }
        const auto start = matrix.apply(ink.points.front());
        cairo_move_to(cr, start.x, start.y);
        for (size_t i = 1; i < ink.points.size(); ++i) {
            const auto point = matrix.apply(ink.points[i]);
            cairo_line_to(cr, point.x, point.y);
        }
        cairo_stroke(cr);
    } else {
        std::vector<::Point> pressurePoints;
        pressurePoints.reserve(ink.points.size());
        for (const auto& sample: ink.points) {
            const auto point = matrix.apply(sample);
            const double pressure = (highlight || whiteout || !ink.pressureSensitive) ? 1.0 : sample.pressure;
            pressurePoints.emplace_back(point.x, point.y,
                                        ink.brush.width * matrix.widthScale * pressure);
        }
        if (pressurePoints.size() == 1 ||
            (pressurePoints.size() == 2 && pressurePoints.front().equalsPos(pressurePoints.back()))) {
            cairo_arc(cr, pressurePoints.front().x, pressurePoints.front().y, pressurePoints.front().z / 2,
                      0, 6.283185307179586);
            cairo_fill(cr);
        } else if (pressurePoints.size() > 1) {
            if (ink.brush.lineDashes.empty()) {
                xoj::view::StrokeContour(pressurePoints).addToCairo(cr);
            } else {
                xoj::view::StrokeContourDashes(pressurePoints, ink.brush.lineDashes).addToCairo(cr, 0);
            }
            cairo_fill(cr);
        }
    }
    if (highlight) {
        cairo_pop_group_to_source(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
        const double opacity = ink.fill >= 0 ? static_cast<double>(ink.fill) / 255.0 : behavior::highlightOpacity;
        cairo_paint_with_alpha(cr, opacity);
    }
    cairo_restore(cr);
}

inline void drawInkPreview(cairo_t* cr, const CanvasState& state, size_t index) {
    drawInk(cr, state.ink.at(index), state.selectionMatrix(index));
}

inline void drawLaserPointer(cairo_t* cr, const CanvasState& state, int page) {
    for (const auto& trail: state.laserTrails) {
        if (trail.page != page || trail.alpha == 0 || trail.strokes.empty()) {
            continue;
        }
        cairo_save(cr);
        cairo_push_group(cr);
        for (const auto& stroke: trail.strokes) {
            drawInk(cr, stroke);
        }
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, static_cast<double>(trail.alpha) / 255.0);
        cairo_restore(cr);
    }
    if (state.currentLaser && state.currentLaser->page == page) {
        drawInk(cr, *state.currentLaser);
    }
}

inline void drawSelection(cairo_t* cr, const CanvasState& state, int page) {
    if (state.verticalSpacePreview()) {
        return;
    }
    const auto bounds = state.selectionBox();
    if (!bounds || bounds->page != page) {
        return;
    }
    cairo_save(cr);
    const auto path = state.selectionPath();
    if (!path.empty()) {
        cairo_move_to(cr, path.front().x, path.front().y);
        for (const auto& p: path) cairo_line_to(cr, p.x, p.y);
        cairo_close_path(cr);
    } else if (!state.selected.empty()) {
        const auto handles = state.selectionHandles();
        cairo_move_to(cr, handles[0].x, handles[0].y);
        for (auto n: {2, 4, 6}) cairo_line_to(cr, handles[n].x, handles[n].y);
        cairo_close_path(cr);
    } else {
        cairo_rectangle(cr, bounds->x0, bounds->y0, bounds->x1 - bounds->x0, bounds->y1 - bounds->y0);
    }
    cairo_set_source_rgba(cr, 0.2, 0.52, 0.9, 0.08);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.2, 0.52, 0.9, 1);
    cairo_set_line_width(cr, 1.5 / state.zoom);
    const double dashes[] = {5 / state.zoom, 3 / state.zoom};
    cairo_set_dash(cr, dashes, 2, 0);
    cairo_stroke(cr);
    if (state.hasSelection() && state.brush.tool != Tool::Hand) {
        const auto handles = state.selectionHandles();
        cairo_set_dash(cr, nullptr, 0, 0);
        cairo_move_to(cr, handles[3].x, handles[3].y);
        cairo_line_to(cr, handles[8].x, handles[8].y);
        cairo_stroke(cr);
        for (size_t i = 0; i < handles.size(); ++i) {
            const auto& p = handles[i];
            if (i == 8) cairo_arc(cr, p.x, p.y, 4 / state.zoom, 0, 6.283185307179586);
            else cairo_rectangle(cr, p.x - 3/state.zoom, p.y - 3/state.zoom, 6/state.zoom, 6/state.zoom);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, .2, .52, .9);
            cairo_stroke(cr);
        }
    }
    cairo_restore(cr);
}

inline void drawVerticalSpacePreview(cairo_t* cr, const CanvasState& state, int page) {
    const auto* preview = state.verticalSpacePreview();
    if (!preview || preview->page != page) {
        return;
    }

    const double top = std::min(preview->startY, preview->endY);
    const double bottom = std::max(preview->startY, preview->endY);
    cairo_save(cr);
    cairo_rectangle(cr, 0, top, state.pageSize(page).width, bottom - top);
    cairo_set_source_rgb(cr, 0.2, 0.52, 0.9);
    cairo_set_line_width(cr, 1.0 / state.zoom);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, 0.2, 0.52, 0.9, 0.3);
    cairo_fill(cr);
    cairo_restore(cr);

    enum class Kind { Ink, Text, Image };
    struct Item {
        size_t order;
        Kind kind;
        size_t index;
    };
    const auto& layer = state.layersForPage(page).layers.at(preview->layerId - 1);
    for (const size_t origin: layer.origins) {
        std::vector<Item> items;
        for (const auto index: preview->inkIndices) {
            if (state.ink[index].originLayer == origin) {
                items.push_back({state.ink[index].layerOrder, Kind::Ink, index});
            }
        }
        for (const auto index: preview->textIndices) {
            if (state.texts[index].originLayer == origin) {
                items.push_back({state.texts[index].layerOrder, Kind::Text, index});
            }
        }
        for (const auto index: preview->imageIndices) {
            if (state.images[index].originLayer == origin) {
                items.push_back({state.images[index].layerOrder, Kind::Image, index});
            }
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.order < b.order; });
        for (const auto& item: items) {
            if (item.kind == Kind::Ink) {
                drawInkPreview(cr, state, item.index);
            } else if (item.kind == Kind::Text) {
                drawText(cr, state.displayText(item.index));
            } else {
                drawCanvasImage(cr, state.displayImage(item.index));
            }
        }
    }
}

}  // namespace prototype
