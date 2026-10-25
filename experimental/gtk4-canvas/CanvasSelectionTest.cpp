#include <iostream>
#include <stdexcept>

#include "CanvasInput.h"

using namespace prototype;

static void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

static CanvasState fixture() {
    CanvasState state;
    state.setPages({{400, 300}, {300, 200}});
    state.snapGridEnabled = false;
    state.ink = {{0, {{20, 30, .25}, {60, 50, .75}}, {Tool::Pen, {1, 0, 0}, 4}, true},
                 {0, {{0, 40}, {100, 40}}, {Tool::Highlighter, {1, 1, 0}, 14}},
                 {0, {{30, 60}, {50, 60}}, {Tool::Eraser, {1, 1, 1}, 5}},
                 {1, {{20, 30}, {60, 50}}, {Tool::Pen, {0, 0, 1}, 2}}};
    state.brush.tool = Tool::SelectRectangle;
    return state;
}

static void rectangle(CanvasState& state, int page, double x0, double y0, double x1, double y1) {
    const auto a = state.pageScreenPoint(page, x0, y0);
    const auto b = state.pageScreenPoint(page, x1, y1);
    check(state.beginSelection(a.x, a.y), "Cannot start rectangle selection");
    state.moveSelectionPointer(b.x, b.y);
    state.finishSelection();
}

static CanvasState multiLayerFixture() {
    CanvasState state;
    state.setPages({{300, 240}});
    state.snapGridEnabled = false;
    state.pageLayers[0].layers = {{"Lower", true, {0}}, {"Upper", true, {1}}, {"Hidden", false, {2}}};
    state.pageLayers[0].selectedLayer = 1;
    state.ink = {{0, {{50, 70}, {110, 70}}, {Tool::Pen, {0, 0, 0}, 2}, false, 0, 1, 0},
                 {0, {{55, 78}, {115, 78}}, {Tool::Pen, {1, 0, 0}, 2}, false, 0, 2, 1},
                 {0, {{60, 88}, {120, 88}}, {Tool::Pen, {0, 0, 1}, 2}, false, 0, 2, 1},
                 {0, {{65, 96}, {125, 96}}, {Tool::Pen, {0, 1, 0}, 2}, false, 0, 3, 2}};
    state.texts.push_back({0, 3, 2, 1, "upper layer", "Sans", "", false, 12, 0xff000000U, -1, 0, false,
                           1, 0, 0, 1, 70, 70, 45, 18, {}});
    state.brush.tool = Tool::SelectRectangle;
    return state;
}

static void selectLayerArea(CanvasState& state, bool lasso) {
    const auto start = state.pageScreenPoint(0, 30, 55);
    check(state.beginSelection(start.x, start.y), "Multilayer selection did not start");
    if (lasso) {
        for (const Point p: {Point{135, 55}, Point{135, 102}, Point{30, 102}, Point{30, 55}}) {
            const auto screen = state.pageScreenPoint(0, p.x, p.y);
            state.moveSelectionPointer(screen.x, screen.y);
        }
    } else {
        const auto end = state.pageScreenPoint(0, 135, 102);
        state.moveSelectionPointer(end.x, end.y);
    }
    state.finishSelection();
}

static void visibleLayerSelection() {
    auto state = multiLayerFixture();
    selectLayerArea(state, false);
    check(state.selected == std::vector<size_t>{0},
          "Single-layer rectangle selection should stay on the active layer");

    state.clearSelection();
    state.selectionAcrossLayers = true;
    selectLayerArea(state, false);
    check(state.selected == std::vector<size_t>({1, 2}),
          "Multilayer rectangle should choose the topmost visible layer with matching strokes");
    check(state.selectedTexts == std::vector<size_t>{0},
          "Multilayer rectangle should include text on the first matching visible layer");

    state.clearSelection();
    state.brush.tool = Tool::SelectLasso;
    selectLayerArea(state, true);
    check(state.selected == std::vector<size_t>({1, 2}),
          "Multilayer lasso should use the same topmost visible layer rule");
    check(state.selectedTexts == std::vector<size_t>{0},
          "Multilayer lasso should include matching text from the first visible layer");

    state.clearSelection();
    state.pageLayers[0].layers[1].visible = false;
    state.brush.tool = Tool::SelectRectangle;
    selectLayerArea(state, false);
    check(state.selected == std::vector<size_t>{0} && state.selectedTexts.empty(),
          "Multilayer selection should skip hidden layers and continue to the next visible match");
}

int main() {
    try {
        visibleLayerSelection();
        auto state = fixture();
        state.zoom = 1.8;
        state.offsetX = -15;
        state.offsetY = 22;
        rectangle(state, 0, 70, 70, 10, 20);
        check(state.selected == std::vector<size_t>({0, 2}),
              "Rectangle must contain all stroke points, preserve ordering, and exclude other pages");
        check(state.revision() == 0, "Selecting must not change saved state");
        const auto start = state.pageScreenPoint(0, 35, 45);
        const auto end = state.pageScreenPoint(0, 55, 75);
        check(state.beginSelection(start.x, start.y), "Selected area should start a move");
        state.moveSelectionPointer(end.x, end.y);
        check(state.selectionBox()->x0 == 40 && state.ink[0].points[0].x == 20 &&
                      state.committedInk()[0].points[0].x == 20,
              "Move preview must not leak into autosave's committed snapshot");
        state.finishSelection();
        const auto movedRevision = state.revision();
        check(movedRevision != 0 && state.ink[2].points[0].y == 90 && state.ink[0].pressureSensitive &&
                      state.ink[0].points[0].pressure == .25 && state.ink[0].brush.color[0] == 1 &&
                      state.ink[1].points[0].x == 0,
              "Moving changed stroke style, pressure, z-order or unselected content");
        state.undo();
        check(state.ink[0].points[0].x == 20 && state.revision() == 0 && state.selected == std::vector<size_t>({0, 2}),
              "Move undo failed to restore geometry/selection");
        state.redo();
        check(state.revision() == movedRevision && state.ink[0].points[0].x == 40,
              "Move redo failed to restore the saved revision");
        state.deleteSelection();
        check(state.ink.size() == 2 && state.selected.empty() && state.ink[0].brush.tool == Tool::Highlighter,
              "Delete selection must preserve other objects and their order");
        state.undo();
        check(state.ink.size() == 4 && state.selected == std::vector<size_t>({0, 2}),
              "Delete undo failed to restore all selected objects");
        state.redo();
        check(state.ink.size() == 2 && state.selected.empty(), "Delete redo failed");

        state = fixture();
        CanvasInput input(state);
        input.begin(Device::Mouse, {10, 20, std::nullopt});
        input.motion(Device::Mouse, {70, 70, std::nullopt});
        input.finish(Device::Mouse);
        input.begin(Device::Stylus, {35, 45, .4});
        input.motion(Device::Mouse, {80, 80, std::nullopt});
        check(state.ink[0].points[0].x == 20, "Another device stole the selection move");
        input.motion(Device::Stylus, {55, 75, .5});
        input.cancel(Device::Stylus);
        check(state.ink[0].points[0].x == 20 && state.revision() == 0 && !state.selecting(),
              "Explicit cancellation must discard the move preview");
        input.begin(Device::Mouse, {35, 45, std::nullopt});
        input.motion(Device::Mouse, {55, 75, std::nullopt});
        input.interrupt();
        check(state.ink[0].points[0].x == 40 && state.canUndo() && !input.active(),
              "Broken grabs must finalize delivered selection motion");
        state.clearSelection();
        rectangle(state, 1, 10, 20, 70, 70);
        check(state.selected == std::vector<size_t>({3}), "Continuous-page offsets corrupted rectangle selection");
        state.selectAllOnPage();
        check(state.selected == std::vector<size_t>({3}), "Select All escaped the active page");
        const auto p = state.pageScreenPoint(1, 35, 45);
        state.beginSelection(p.x, p.y);
        state.moveSelectionPointer(p.x + 10000, p.y + 10000);
        state.finishSelection();
        check(state.ink[3].points.back().x > 300 && state.ink[3].points.back().y > 200,
              "Native selection transforms must permit off-page coordinates");
        state.clearSelection();
        const auto rev = state.revision();
        state.deleteSelection();
        check(state.revision() == rev, "Empty selection deletion must not dirty the document");
        state = fixture();
        state.ink[0].points = {{.2, 30}, {20, 40}};
        rectangle(state, 0, .8, 20, 30, 50);
        check(state.selected == std::vector<size_t>({0}),
              "Page-edge selection must extend at the source's 1 pt threshold");
        std::cout << "PASS: rectangle containment, visible-layer rectangle/lasso selection, move/delete delta undo, "
                     "pressure/style/order, cancellation, autosave snapshot, pages\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
