#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::verifyCursorFeedbackIntegration() {
    require(canvas && pointerMotion && gtk_widget_get_mapped(canvas),
            "Cursor feedback verification needs a mapped canvas motion controller");

    const int viewportWidth = gtk_widget_get_width(canvas);
    const int viewportHeight = gtk_widget_get_height(canvas);
    require(viewportWidth > 0 && viewportHeight > 0, "Cursor feedback canvas is not mapped");

    state.setPages({{600, 800}});
    state.zoom = 1.0;
    state.offsetX = (viewportWidth - state.pageSize(0).width) / 2.0;
    state.offsetY = (viewportHeight - state.pageSize(0).height) / 2.0;
    state.constrainScrollOffsets(viewportWidth, viewportHeight);
    state.clearSelection();
    state.texts.clear();
    selectedImageIndex.reset();
    input.reset();

    prototype::TextItem text;
    text.page = 0;
    text.layerId = state.activeLayerId(0);
    text.text = "Cursor feedback";
    text.x = 180;
    text.y = 200;
    text.layoutWidth = 180;
    text.layoutHeight = 40;
    state.texts.push_back(std::move(text));

    const auto screenPoint = [this](double x, double y) { return state.pageScreenPoint(0, x, y); };
    const auto cursorName = [this] {
        auto* cursor = gtk_widget_get_cursor(canvas);
        const char* name = cursor ? gdk_cursor_get_name(cursor) : nullptr;
        return name ? std::string(name) : std::string{};
    };
    const auto expectCursor = [this, &cursorName](double x, double y, const char* expected) {
        g_signal_emit_by_name(pointerMotion, "motion", x, y);
        updateCanvasCursor(x, y);
        const auto actual = cursorName();
        require(actual == expected,
                (std::string("Expected canvas cursor '") + expected + "', got '" + actual + "'").c_str());
    };

    state.brush.tool = Tool::Text;
    auto point = screenPoint(220, 215);
    expectCursor(point.x, point.y, "text");

    state.brush.tool = Tool::Eraser;
    point = screenPoint(80, 120);
    expectCursor(point.x, point.y, "crosshair");

    state.brush.tool = Tool::Hand;
    expectCursor(point.x, point.y, "grab");

    state.brush.tool = Tool::SmartSelect;
    point = screenPoint(220, 215);
    expectCursor(point.x, point.y, "text");

    state.selectedTexts = {0};
    update();
    const auto handles = state.selectionHandles();
    point = screenPoint((handles[0].x + handles[4].x) / 2, (handles[0].y + handles[4].y) / 2);
    expectCursor(point.x, point.y, "all-scroll");
    require(state.beginSelection(point.x, point.y), "The selected text body did not begin a move gesture");
    refreshCanvasCursor();
    require(cursorName() == "grabbing", "An active selection move should use the grabbing cursor");
    state.cancelSelection();
    update();

    const auto resizeHandle = state.selectionHandles()[1];
    point = screenPoint(resizeHandle.x, resizeHandle.y);
    expectCursor(point.x, point.y, "ns-resize");
    require(state.beginSelection(point.x, point.y), "The selected text edge did not begin a resize gesture");
    refreshCanvasCursor();
    require(cursorName() == "ns-resize", "An active selection resize should retain its resize cursor");
    state.cancelSelection();
    update();

    point = screenPoint(handles[1].x, handles[1].y);
    expectCursor(point.x, point.y, "ns-resize");
    point = screenPoint(handles[3].x, handles[3].y);
    expectCursor(point.x, point.y, "ew-resize");
    point = screenPoint(handles[0].x, handles[0].y);
    expectCursor(point.x, point.y, "nwse-resize");
    point = screenPoint(handles[8].x, handles[8].y);
    expectCursor(point.x, point.y, "crosshair");
    require(state.beginSelection(point.x, point.y), "The selected text rotation handle did not begin a gesture");
    refreshCanvasCursor();
    require(cursorName() == "crosshair", "An active selection rotation should retain its rotation cursor");
    state.cancelSelection();
    update();

    input.spaceHeld = true;
    expectCursor(point.x, point.y, "grab");
    input.spaceHeld = false;
    auto* panState = static_cast<PanState*>(g_object_get_data(G_OBJECT(panGestures.front()), "pan-state"));
    panState->enabled = true;
    refreshCanvasCursor();
    require(cursorName() == "grabbing", "An active pan gesture should use the grabbing cursor");
    panState->enabled = false;
    refreshCanvasCursor();

    std::cout << "PASS: GTK4 contextual tool, text, selection, transform and pan cursors\n";
    g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
}

}  // namespace prototype::gtk4
