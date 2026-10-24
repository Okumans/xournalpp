#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "CanvasInput.h"
#include "StrokeClipboard.h"

using namespace prototype;

static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
static bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
static CanvasState fixture() {
    CanvasState state;
    state.setPages({{400, 300}, {300, 200}});
    state.ink = {{0, {{100, 100, .25}, {180, 160, .75}}, {Tool::Pen, {1, 0, 0}, 4}, true},
                 {0, {{240, 110}, {280, 170}}, {Tool::Highlighter, {1, 1, 0}, 14}},
                 {1, {{40, 50}, {100, 70}}, {Tool::Eraser, {1, 1, 1}, 5}}};
    state.brush.tool = Tool::SmartSelect;
    state.selected = {0};
    return state;
}
static void drag(CanvasState& state, Point from, Point to) {
    auto a = state.pageScreenPoint(state.page, from.x, from.y);
    auto b = state.pageScreenPoint(state.page, to.x, to.y);
    check(state.beginSelection(a.x, a.y), "Gesture did not begin");
    state.moveSelectionPointer(b.x, b.y);
}
int main(int argc, char** argv) {
    try {
        auto state = fixture();
        if (argc == 3 && std::string(argv[1]) == "--encode") {
            state.selected = {0, 1};
            auto bytes = clipboard::encode(state.copySelection());
            std::ofstream(argv[2], std::ios::binary).write(bytes.data(), bytes.size());
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--verify") {
            std::ifstream stream(argv[2], std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
            auto strokes = clipboard::decode(bytes);
            check(strokes.size() == 2 && strokes[0].pressureSensitive && near(strokes[0].points[0].pressure, .25) &&
                          strokes[1].brush.tool == Tool::Highlighter,
                  "Native clipboard round trip lost style/pressure");
            return 0;
        }
        // Side scaling changes one axis; native width changes by sqrt(|sx*sy|).
        drag(state, {180, 130}, {260, 130});
        check(near(state.selectionMatrix(0).xx, 2) && near(state.selectionMatrix(0).yy, 1),
              "Side scaling is not axis-specific");
        check(state.committedInk()[0].brush.width == 4, "Resize preview changed committed ink");
        state.finishSelection();
        check(near(state.ink[0].points[1].x, 260) && near(state.ink[0].brush.width, 4 * std::sqrt(2)),
              "Resize geometry/width mismatch");
        state.undo();
        state.redo();
        check(near(state.ink[0].brush.width, 4 * std::sqrt(2)), "Resize undo/redo changed width");
        state = fixture();
        drag(state, {180, 160}, {260, 220});
        state.finishSelection();
        check(near(state.ink[0].points[1].x, 260) && near(state.ink[0].points[1].y, 220) &&
                      state.ink[0].brush.width == 8,
              "Corner resizing did not preserve aspect ratio");
        state = fixture();
        drag(state, {180, 130}, {20, 130});
        state.finishSelection();
        check(near(state.ink[0].points[1].x, 20) && state.ink[0].brush.width == 4, "Mirror changed width sign");
        state = fixture();
        const auto handle = state.selectionHandles()[8];
        drag(state, handle, {140 - (handle.y - 130), 130 + handle.x - 140});
        state.finishSelection();
        check(near(state.ink[0].points[0].x, 170) && near(state.ink[0].points[0].y, 90) &&
                      state.ink[0].brush.width == 4,
              "Rotation must preserve width and rotate around selection center");
        state = fixture();
        drag(state, {130, 125}, {130, 125});
        const auto destination = state.pageScreenPoint(1, 150, 100);
        state.moveSelectionPointer(destination.x, destination.y);
        check(state.displayPage(0) == 1 && state.committedInk()[0].page == 0,
              "Cross-page preview mutated committed page");
        state.finishSelection();
        check(state.ink.back().page == 1 && state.selected == std::vector<size_t>{2} &&
                      state.ink[1].brush.tool == Tool::Eraser,
              "Transferred selection must be above destination ink");
        state.undo();
        check(state.ink[0].page == 0 && state.selected == std::vector<size_t>{0},
              "Cross-page undo lost order/selection");
        state.redo();
        check(state.ink.back().page == 1, "Cross-page redo failed");

        state = fixture();
        state.selected = {0, 1};
        const auto groupStart = state.pageScreenPoint(0, 130, 130);
        const auto groupEnd = state.pageScreenPoint(1, 150, 100);
        check(state.beginSelection(groupStart.x, groupStart.y), "Grouped cross-page move did not begin");
        state.moveSelectionPointer(groupEnd.x, groupEnd.y);
        state.finishSelection();
        check(state.ink.size() == 3 && state.ink[0].brush.color[2] == 1 && state.ink[1].brush.tool == Tool::Pen &&
                      state.ink[2].brush.tool == Tool::Highlighter && state.selected == std::vector<size_t>({1, 2}),
              "Cross-page group changed destination order or selection");
        state.undo();
        check(state.ink[0].page == 0 && state.ink[1].page == 0 && state.ink[2].page == 1 &&
                      state.selected == std::vector<size_t>({0, 1}),
              "Grouped cross-page undo failed");
        state.redo();
        check(state.ink[0].page == 1 && state.ink[1].page == 1 && state.ink[2].page == 1,
              "Grouped cross-page redo failed");

        state = fixture();
        state.clearSelection();
        state.ink.insert(state.ink.begin() + 1, state.ink[0]);
        drag(state, {140, 130}, {160, 150});
        state.finishSelection();
        check(state.selected == std::vector<size_t>{1} && state.ink[0].points[0].x == 100 &&
                      state.ink[1].points[0].x == 120,
              "Smart Select must move only the topmost hit stroke");
        state = fixture();
        state.clearSelection();
        state.brush.tool = Tool::SelectLasso;
        check(state.beginSelection(90, 90), "Lasso did not begin");
        for (const auto p: {Point{190, 90}, Point{190, 180}, Point{90, 180}, Point{90, 90}})
            state.moveSelectionPointer(p.x, p.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{0}, "Lasso winding containment failed");
        state.clearSelection();
        state.ink.push_back({0, {{.2, 98}, {30, 98}}, {Tool::Pen, {0, 0, 0}, 2}});
        check(state.beginSelection(.5, 90), "Page-edge lasso did not begin");
        for (const auto p: {Point{80, 90}, Point{80, 110}, Point{.5, 110}}) state.moveSelectionPointer(p.x, p.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{3}, "Lasso edge extension failed to include a touching stroke");
        state.clearSelection();
        state.brush.tool = Tool::SelectLasso;
        check(state.beginSelection(90, 90), "Second lasso did not begin");
        for (const auto p: {Point{190, 90}, Point{190, 180}, Point{90, 180}, Point{90, 90}})
            state.moveSelectionPointer(p.x, p.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{0}, "Lasso selection changed after page-edge projection");
        CanvasInput input(state);
        input.shiftHeld = true;
        input.begin(Device::Mouse, {230, 100, {}});
        input.motion(Device::Mouse, {290, 180, {}});
        input.finish(Device::Mouse);
        check(state.selected == std::vector<size_t>({0, 1}), "Shift rectangle must add selection even in lasso tool");
        const auto bytes = clipboard::encode(state.copySelection());
        const auto copy = clipboard::decode(bytes);
        check(copy.size() == 2 && copy[0].points[1].pressure == .75 && copy[1].brush.width == 14,
              "Clipboard lost pressure/style");
        for (size_t length = 0; length < bytes.size(); ++length) {
            bool rejected = false;
            try {
                clipboard::decode(std::string_view(bytes).substr(0, length));
            } catch (const std::exception&) {
                rejected = true;
            }
            check(rejected, "A truncated clipboard was accepted");
        }
        auto oversized = bytes;
        const auto block = oversized.find("_b");
        const auto huge = std::numeric_limits<size_t>::max();
        std::memcpy(oversized.data() + block + 2, &huge, sizeof(huge));
        bool rejected = false;
        try {
            clipboard::decode(oversized);
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Overflowing clipboard array count was accepted");
        rejected = false;
        try {
            clipboard::decode(bytes + "trailing");
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Trailing clipboard data accepted");
        state = fixture();
        check(state.pasteStrokes(copy, 1, {150, 100}), "Paste failed");
        check(state.selected == std::vector<size_t>({3, 4}) && state.ink.size() == 5,
              "Paste did not select appended group");
        state.undo();
        check(state.ink.size() == 3 && state.selected == std::vector<size_t>{0}, "Paste undo lost previous selection");
        state.redo();
        check(state.ink.size() == 5 && state.selected.size() == 2, "Paste redo failed");
        const auto revision = state.revision();
        check(!state.pasteStrokes({Ink{0, {}, {}}}, 0, {0, 0}) && state.revision() == revision &&
                      state.selected.size() == 2,
              "Malformed paste must be atomic");
        state = fixture();
        state.ink[0].points = {{100, 100}, {100, 100}};
        const auto dotHandle = state.selectionHandles()[4];
        drag(state, dotHandle, {100, 100});
        state.finishSelection();
        check(std::isfinite(state.ink[0].brush.width) && state.ink[0].brush.width > 0,
              "Degenerate dot resize broke width");
        std::cout << "PASS: affine geometry/width, mirror, rotate, cross-page order/undo, Smart Select, lasso, "
                     "Shift-add, bounded native clipboard and paste undo\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
