#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::smokeShiftStrokeConstraints(size_t expectedInkCount) {
    selectTool(Tool::Pen);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                               static_cast<guint>(prototype::DrawingMode::Freehand));
    gboolean handled = false;
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Shift_L, 0u, GdkModifierType(0), &handled);

    const auto firstStart = state.pageScreenPoint(0, 120, 200);
    g_signal_emit_by_name(mouse, "drag-begin", firstStart.x, firstStart.y);
    g_signal_emit_by_name(mouse, "drag-update", 40.0 * state.zoom, -15.0 * state.zoom);
    g_signal_emit_by_name(mouse, "drag-end", 40.0 * state.zoom, -15.0 * state.zoom);
    require(state.ink.size() == expectedInkCount + 1 &&
                    std::all_of(state.ink.back().points.begin(), state.ink.back().points.end(),
                                [](const prototype::Point& point) { return std::abs(point.y - 200) < 1e-6; }),
            "GTK Shift input did not constrain freehand samples to the horizontal axis");

    const auto secondStart = state.pageScreenPoint(0, 200, 300);
    g_signal_emit_by_name(mouse, "drag-begin", secondStart.x, secondStart.y);
    g_signal_emit_by_name(mouse, "drag-update", 40.0 * state.zoom, 20.0 * state.zoom);
    g_signal_emit_by_name(mouse, "drag-end", 40.0 * state.zoom, 20.0 * state.zoom);
    require(state.ink.size() == expectedInkCount + 2 &&
                    std::all_of(state.ink.back().points.begin(), state.ink.back().points.end(),
                                [](const prototype::Point& point) { return std::abs(point.y - 200) < 1e-6; }),
            "GTK Shift axis was not preserved across separate strokes");
    const auto endpoint = state.ink.back().points.back();

    const auto clickStart = state.pageScreenPoint(0, 260, 380);
    g_signal_emit_by_name(mouse, "drag-begin", clickStart.x, clickStart.y);
    g_signal_emit_by_name(mouse, "drag-end", 1.0, 1.0);
    require(state.ink.size() == expectedInkCount + 3 && state.ink.back().points.size() == 2 &&
                    state.ink.back().points.front().x == endpoint.x && state.ink.back().points.front().y == endpoint.y,
            "GTK Shift-click did not connect from the preceding Pen endpoint");

    g_signal_emit_by_name(keys, "key-released", GDK_KEY_Shift_L, 0u, GdkModifierType(0));
    waitForNextPaint();
    capture("shift-strokes");
    state.undo();
    state.undo();
    state.undo();
    update();
    require(state.ink.size() == expectedInkCount, "Shift stroke smoke operations did not undo cleanly");
}

void Prototype::smokeEraserShiftConstraints(size_t expectedInkCount) {
    const auto drag = [this](double x, double y, double dx, double dy) {
        const auto start = state.pageScreenPoint(0, x, y);
        g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
        g_signal_emit_by_name(mouse, "drag-update", dx * state.zoom, dy * state.zoom);
        g_signal_emit_by_name(mouse, "drag-end", dx * state.zoom, dy * state.zoom);
    };

    selectTool(Tool::Pen);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                               static_cast<guint>(prototype::DrawingMode::Freehand));
    drag(100, 500, 12, 0);
    drag(260, 500, 12, 0);
    require(state.ink.size() == expectedInkCount + 2, "Eraser Shift smoke setup did not create its two target strokes");

    selectTool(Tool::Eraser);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
    state.brush.width = 18;
    gboolean handled = false;
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Shift_L, 0u, GdkModifierType(0), &handled);
    drag(100, 500, 160, 40);
    require(state.ink.size() == expectedInkCount,
            "GTK Shift input did not constrain Standard Eraser samples along the saved axis");
    waitForNextPaint();
    capture("eraser-shift");

    state.undo();
    require(state.ink.size() == expectedInkCount + 2,
            "One undo did not restore the complete axis-constrained Eraser drag");
    state.redo();
    require(state.ink.size() == expectedInkCount, "Redo did not reapply the complete axis-constrained Eraser drag");

    selectTool(Tool::Pen);
    drag(350, 500, 12, 0);
    require(state.ink.size() == expectedInkCount + 1, "Eraser Shift persistence target was not drawn");
    selectTool(Tool::Eraser);
    state.brush.width = 18;
    const auto shiftedClick = state.pageScreenPoint(0, 350, 550);
    g_signal_emit_by_name(mouse, "drag-begin", shiftedClick.x, shiftedClick.y);
    g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
    require(state.ink.size() == expectedInkCount, "GTK Shift-click did not preserve the Eraser axis across gestures");

    g_signal_emit_by_name(keys, "key-released", GDK_KEY_Shift_L, 0u, GdkModifierType(0));
    selectTool(Tool::Pen);
    drag(450, 620, 12, 0);
    require(state.ink.size() == expectedInkCount + 1, "Eraser Shift release target was not drawn");
    selectTool(Tool::Eraser);
    state.brush.width = 18;
    drag(430, 590, 20, 30);
    require(state.ink.size() == expectedInkCount,
            "Releasing Shift did not restore raw pointer movement for the GTK Eraser path");

    for (int edit = 0; edit < 7; ++edit) {
        state.undo();
    }
    selectTool(Tool::Pen);
    update();
    require(state.ink.size() == expectedInkCount, "Eraser Shift smoke edits did not undo cleanly");
}

void Prototype::smokeDrawingTools() {
    const auto before = state.ink.size();
    selectTool(Tool::Pen);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                               static_cast<guint>(prototype::DrawingMode::Freehand));
    gtk_menu_button_popup(GTK_MENU_BUTTON(paletteButton));
    gtk_expander_set_expanded(GTK_EXPANDER(drawingOptionsExpander), true);
    waitForNextPaint();
    require(gtk_widget_get_visible(palettePopover), "Drawing controls popover did not open");
    capture("drawing-controls");
    gtk_popover_popdown(GTK_POPOVER(palettePopover));
    smokeShiftStrokeConstraints(before);
    smokeEraserShiftConstraints(before);

    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                               static_cast<guint>(prototype::DrawingMode::Rectangle));
    const auto start = state.pageScreenPoint(0, 190, 230);
    g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
    gboolean handled = false;
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Shift_L, 0u, GdkModifierType(0), &handled);
    g_signal_emit_by_name(mouse, "drag-update", 60.0 * state.zoom, 35.0 * state.zoom);
    require(state.current && state.current->points.size() == 5 &&
                    std::abs(std::abs(state.current->points[2].x - state.current->points[0].x) -
                             std::abs(state.current->points[2].y - state.current->points[0].y)) < 1e-6,
            "Toolbar rectangle and live Shift constraint did not update the preview");
    g_signal_emit_by_name(mouse, "drag-end", 60.0 * state.zoom, 35.0 * state.zoom);
    g_signal_emit_by_name(keys, "key-released", GDK_KEY_Shift_L, 0u, GdkModifierType(0));
    require(state.ink.size() == before + 1 && state.ink.back().points.size() == 5,
            "Constrained rectangle did not commit through the canvas input path");
    waitForNextPaint();
    capture("shape-tools");
    state.undo();

    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                               static_cast<guint>(prototype::DrawingMode::ShapeRecognizer));
    const double centerX = 300, centerY = 330, radius = 54;
    const auto first = state.pageScreenPoint(0, centerX + radius, centerY);
    begin(Device::Mouse, first.x, first.y);
    for (int i = 1; i <= 48; ++i) {
        const double angle = 2.0 * M_PI * i / 48.0;
        const auto next =
                state.pageScreenPoint(0, centerX + radius * std::cos(angle), centerY + radius * std::sin(angle));
        input.motion(Device::Mouse, {next.x, next.y, std::nullopt});
    }
    finish(Device::Mouse);
    require(state.ink.size() == before + 1 && state.ink.back().points.size() > 20,
            "Shape recognizer did not replace the circular stroke in the GTK smoke path");
    waitForNextPaint();
    capture("shape-recognized");
    state.undo();
    require(state.ink.size() == before, "Shape tool smoke operations did not undo cleanly");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown), static_cast<guint>(prototype::DrawingMode::Spline));
    auto firstKnot = state.pageScreenPoint(0, 400, 260);
    g_signal_emit_by_name(textClick, "pressed", 1, firstKnot.x, firstKnot.y);
    g_signal_emit_by_name(textClick, "released", 1, firstKnot.x, firstKnot.y);
    auto secondKnot = state.pageScreenPoint(0, 460, 275);
    g_signal_emit_by_name(textClick, "pressed", 1, secondKnot.x, secondKnot.y);
    auto tangentPoint = state.pageScreenPoint(0, 475, 305);
    g_signal_emit_by_name(pointerMotion, "motion", tangentPoint.x, tangentPoint.y);
    g_signal_emit_by_name(textClick, "released", 1, tangentPoint.x, tangentPoint.y);
    require(input.splineInProgress && state.current && state.current->points.size() > 2,
            "Toolbar spline clicks and tangent drag did not update the live path");
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Escape, 0u, GdkModifierType(0), &handled);
    require(!input.splineInProgress && state.ink.size() == before + 1 && state.ink.back().points.size() > 2,
            "Escape did not finalize the interactive spline through the GTK key path");
    waitForNextPaint();
    capture("spline-tools");
    state.undo();
    require(state.ink.size() == before, "Interactive spline should commit as a single undo action");
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                               static_cast<guint>(prototype::DrawingMode::Freehand));
    update();
}

}  // namespace prototype::gtk4
