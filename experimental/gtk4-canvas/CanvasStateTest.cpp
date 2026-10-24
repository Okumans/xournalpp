#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "BehaviorDefaults.h"
#include "CanvasState.h"

using namespace prototype;

static void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static void brushAndHistory() {
    CanvasState state;
    state.brush = {Tool::Pen, {1, 0, 0}, 4};
    state.begin(Device::Mouse, 40, 40);
    state.brush = {Tool::Highlighter, {1, 1, 0}, 18};
    state.append(Device::Mouse, 100, 40);
    state.finish(Device::Mouse);
    check(state.ink[0].brush.width == 4 && state.ink[0].brush.color[0] == 1 && state.ink[0].brush.tool == Tool::Pen,
          "changing the selected brush cannot restyle a stroke in progress");
    state.begin(Device::Mouse, 40, 40);
    state.append(Device::Mouse, 100, 40, 0.3);
    state.finish(Device::Mouse);
    check(state.ink[1].brush.tool == Tool::Highlighter && state.ink[1].brush.width == 18,
          "new strokes capture the selected brush");
    state.undo();
    check(state.ink.size() == 1 && state.canRedo(), "undo exposes a redo step");
    state.erase(500, 500);
    check(state.canRedo(), "a no-op eraser must preserve redo");
    state.redo();
    check(state.ink.size() == 2 && state.ink[1].brush.tool == Tool::Highlighter,
          "redo restores stroke style and ordering");
    state.erase(70, 40);
    check(state.ink.empty(), "eraser can remove multiple overlapping strokes");
    state.undo();
    check(state.ink.size() == 2 && state.ink[0].brush.tool == Tool::Pen && state.ink[1].brush.tool == Tool::Highlighter,
          "erase undo restores all strokes in their original order");
    state.redo();
    check(state.ink.empty(), "erase redo removes the same strokes");
    state.undo();
    state.undo();
    state.begin(Device::Mouse, 200, 80);
    state.finish(Device::Mouse);
    check(!state.canRedo(), "a new edit replaces the undone history branch");

    CanvasState bounded;
    for (int i = 0; i < 140; ++i) {
        bounded.begin(Device::Mouse, i, 20);
        bounded.finish(Device::Mouse);
    }
    for (int i = 0; i < 140; ++i) {
        bounded.undo();
    }
    check(bounded.ink.empty() && !bounded.canUndo(),
          "history must retain edits beyond the old prototype's 128-step cap");
    for (int i = 0; i < 140; ++i) {
        bounded.redo();
    }
    check(bounded.ink.size() == 140 && !bounded.canRedo(), "retained history remains fully redoable");
}

int main() {
    try {
        brushAndHistory();
        check(std::abs(behavior::steppedZoom(1, true, true) - 1.02) < 1e-12,
              "wheel zoom must use the original default two-percent step");
        check(std::abs(behavior::steppedZoom(1, true, false) - 1.1) < 1e-12,
              "keyboard zoom must use the original default ten-percent step");
        check(std::abs(behavior::steppedZoom(behavior::steppedZoom(1, true, false), false, false) - 1) < 1e-12,
              "opposite zoom steps must return to the starting zoom");
        CanvasState state;
        state.fit(1200, 900);
        const auto x = state.offsetX + 100 * state.zoom;
        const auto y = state.offsetY + 120 * state.zoom;
        const auto anchor = state.documentPoint(x, y);
        state.zoomAt(2.5, x, y);
        const auto after = state.documentPoint(x, y);
        check(std::hypot(anchor.x - after.x, anchor.y - after.y) < 1e-9, "zoom must preserve anchor");
        state.zoomAt(std::numeric_limits<double>::infinity(), x, y);
        check(state.zoom == 2.5, "non-finite zoom must be ignored");
        check(!state.begin(Device::Mouse, -10000, -10000), "outside-page ink must be rejected");
        check(state.begin(Device::Stylus, x, y, 0.2), "stylus begins inside page");
        check(!state.begin(Device::Mouse, x, y), "mouse cannot steal stylus stroke");
        state.append(Device::Mouse, x + 20, y + 20);
        state.finish(Device::Mouse);
        check(state.current && state.current->points.size() == 1, "foreign events cannot modify or finish stroke");
        state.cancel(Device::Mouse);
        check(state.current.has_value(), "cancelling a mouse gesture cannot discard stylus ink");
        state.append(Device::Stylus, x + 100, y, 0.8);
        state.zoomAt(4, x, y);
        check(state.zoom == 2.5, "zoom cannot move an active stroke");
        state.finish(Device::Stylus);
        check(state.ink.size() == 1 && state.ink[0].points.back().pressure == 0.8, "pressure survives commit");
        check(state.documentPoint(x, y, std::numeric_limits<double>::quiet_NaN()).pressure == 1,
              "invalid pressure must not poison Cairo drawing");
        state.erase(x + 50, y);
        check(state.ink.empty(), "eraser hits segment between sparse samples");
        state.undo();
        check(state.ink.size() == 1, "erase undo restores ink");
        state.page = 1;
        state.erase(x + 50, y);
        check(state.ink.size() == 1, "eraser cannot affect another page");
        state.undo();
        check(state.ink.empty(), "stroke undo works across page navigation");
        state.begin(Device::Mouse, x, y);
        state.beginPinch();
        check(!state.current && !state.begin(Device::Stylus, x, y), "pinch cancels ink and owns input");
        check(state.blocksScroll(false, 0) && !state.blocksScroll(true, 0), "pinch blocks smooth scroll only");
        state.endPinch(100);
        check(state.blocksScroll(false, 200099) && !state.blocksScroll(false, 200100), "pinch cooldown is bounded");
        state.begin(Device::Mouse, x, y);
        state.resetInput();
        check(!state.current && !state.pinching && state.scrollBlockedUntil == 0, "focus loss clears input");
        state.fit(400, 500);
        check(state.zoom > 0 && std::isfinite(state.offsetX), "narrow viewport remains valid");
        std::cout
                << "PASS: anchor zoom, device ownership, pressure, erase/undo, page isolation, pinch/cancel, resize\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
