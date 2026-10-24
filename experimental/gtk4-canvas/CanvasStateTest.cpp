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
    state.eraserMode = EraserMode::DeleteStroke;
    state.erase(70, 40);
    check(state.ink.empty(), "delete-stroke eraser can remove multiple overlapping strokes");
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

static void savedHistory() {
    CanvasState state;
    check(state.revision() == 0, "new state starts at the clean revision");
    state.begin(Device::Mouse, 20, 20);
    state.finish(Device::Mouse);
    const auto saved = state.revision();
    state.begin(Device::Mouse, 40, 40);
    state.finish(Device::Mouse);
    check(state.revision() != saved, "an additional edit changes the revision");
    state.undo();
    check(state.revision() == saved, "undo back to saved state must be clean");
    state.undo();
    check(state.revision() == 0, "undo to initial state restores revision zero");
    state.redo();
    check(state.revision() == saved, "redo to saved state must be clean");
    state.undo();
    state.begin(Device::Mouse, 80, 80);
    state.finish(Device::Mouse);
    check(state.revision() != saved && !state.canRedo(), "same-depth replacement is a new revision");
    const auto current = state.revision();
    state.beginErase();
    state.finishErase();
    check(state.revision() == current, "no-op erase must not dirty a saved state");
    state.beginErase();
    state.erase(80, 80);
    state.finishErase();
    check(state.revision() != current, "one grouped eraser drag changes the revision");
    state.undo();
    check(state.revision() == current, "undo eraser restores the saved revision");
}

static void partialAndWhiteoutEraser() {
    check(behavior::penWidths == std::array<double, 5>{0.42, 0.85, 1.41, 2.26, 5.67},
          "pen width presets must match ToolHandler::initTools");
    check(behavior::highlighterWidths == std::array<double, 5>{1.0, 2.83, 8.50, 19.84, 30.0},
          "highlighter width presets must match ToolHandler::initTools");
    check(behavior::eraserWidths == std::array<double, 5>{1.0, 2.83, 8.50, 12.0, 18.0},
          "eraser width presets must match ToolHandler::initTools");
    check(behavior::matchToolSize(behavior::penWidths, 1.41) == behavior::ToolSize::Medium &&
                  !behavior::matchToolSize(behavior::penWidths, 6.0).has_value(),
          "preset matching must distinguish standard presets from custom widths");

    CanvasState state;
    check(state.eraserMode == EraserMode::Standard && std::abs(state.eraserWidth - 8.50) < 1e-9 &&
                  std::abs(state.brush.width - 1.41) < 1e-9,
          "default tool sizes and eraser mode must match Xournal++ medium presets");

    // Surround a pressure-sensitive stroke with two untouched neighbor strokes to check z-order.
    state.brush = {Tool::Pen, {0, 0, 0}, 2};
    state.begin(Device::Mouse, 20, 30);
    state.append(Device::Mouse, 180, 30);
    state.finish(Device::Mouse);

    state.brush = {Tool::Pen, {1, 0, 0}, 4};
    state.begin(Device::Stylus, 20, 100, 0.2);
    state.current->pressureSensitive = true;
    state.append(Device::Stylus, 180, 100, 1.0);
    state.finish(Device::Stylus);

    state.brush = {Tool::Highlighter, {1, 1, 0}, 8.50};
    state.begin(Device::Mouse, 20, 170);
    state.append(Device::Mouse, 180, 170);
    state.finish(Device::Mouse);

    const auto beforeEraseRev = state.revision();
    // Grazing outerBox (outerHalf = 8.5 + 0.4 * 4 = 10.1) without entering innerBox (8.5) must not erase.
    state.erase(100, 109.5);
    check(state.ink.size() == 3 && state.revision() == beforeEraseRev,
          "grazing the padded outer box without hitting the inner eraser square must not split ink");

    // Grouped Standard erase drag: split the middle stroke twice (into 3 pieces) and delete a dot.
    state.brush = {Tool::Pen, {0, 0.5, 0}, 3};
    state.begin(Device::Mouse, 240, 100);
    state.finish(Device::Mouse);
    const auto savedRev = state.revision();
    check(state.ink.size() == 4, "expected four strokes before grouped partial erase");

    state.beginErase();
    state.erase(70, 100);
    state.erase(130, 100);
    state.erase(240, 100);
    state.finishErase();

    // Stroke 0 (y=30) untouched, Stroke 1 (y=100) split into 3 pieces at indices 1..3,
    // Stroke 2 (y=170) now at index 4, Stroke 3 (dot at 240,100) completely removed -> 5 strokes total.
    check(state.ink.size() == 5, "grouped partial erase must split contacted stroke and remove dot");
    check(state.ink[0].points.front().y == 30 && state.ink[4].brush.tool == Tool::Highlighter,
          "partial erase must preserve z-order of surrounding strokes");
    // Expected outerHalf = 8.5 + 0.4 * 4 = 10.1 -> first gap [59.9, 80.1], second gap [119.9, 140.1].
    check(std::abs(state.ink[1].points.back().x - 59.9) < 1e-6 &&
                  std::abs(state.ink[2].points.front().x - 80.1) < 1e-6 &&
                  std::abs(state.ink[2].points.back().x - 119.9) < 1e-6 &&
                  std::abs(state.ink[3].points.front().x - 140.1) < 1e-6,
          "partial erase must split at exact PaddedBox boundaries");
    check(state.ink[1].pressureSensitive && state.ink[2].pressureSensitive && state.ink[3].pressureSensitive &&
                  state.ink[1].points.front().pressure < state.ink[1].points.back().pressure &&
                  state.ink[3].points.front().pressure < state.ink[3].points.back().pressure,
          "split sub-strokes must preserve pressure mode and interpolated pressures");

    state.undo();
    check(state.ink.size() == 4 && state.revision() == savedRev && state.ink[1].points.size() == 2 &&
                  state.ink[1].points.front().x == 20 && state.ink[1].points.back().x == 180 &&
                  state.ink[3].points.front().x == 240,
          "one undo must restore all split and deleted strokes in original order");
    state.redo();
    check(state.ink.size() == 5 && std::abs(state.ink[2].points.front().x - 80.1) < 1e-6,
          "one redo must reapply the grouped partial erase");

    // Whiteout mode draws opaque white Tool::Eraser strokes without altering Pen brush when eraserEnd is used.
    state.eraserMode = EraserMode::Whiteout;
    state.eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::Thick)];
    state.brush = {Tool::Pen, {0.2, 0.2, 0.8}, 1.41};
    check(state.begin(Device::Stylus, 40, 100, 0.3, true), "stylus eraser end in whiteout mode must begin stroke");
    state.append(Device::Stylus, 120, 100, 0.9);
    state.finish(Device::Stylus, 0.9);
    check(state.brush.tool == Tool::Pen && state.ink.back().brush.tool == Tool::Eraser &&
                  state.ink.back().brush.color == behavior::whiteoutColor &&
                  std::abs(state.ink.back().brush.width - 12.0) < 1e-9 && !state.ink.back().pressureSensitive,
          "whiteout must commit a constant-width white Tool::Eraser stroke and preserve toolbar brush");
    state.eraserMode = EraserMode::DeleteStroke;
    state.erase(80, 100);
    check(state.ink.back().brush.tool != Tool::Eraser, "whiteout strokes must be erasable like native strokes");

    // DeleteStroke must hit the visible painted body of a 30pt Very Thick stroke even with a 1pt Very Fine eraser.
    state.brush = {Tool::Highlighter, behavior::defaultHighlighterColor,
                   behavior::highlighterWidths[static_cast<size_t>(behavior::ToolSize::VeryThick)]};
    state.begin(Device::Mouse, 20, 220);
    state.append(Device::Mouse, 180, 220);
    state.finish(Device::Mouse);
    const auto countWithThick = state.ink.size();
    state.eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::VeryFine)];
    state.erase(100, 232);  // 12pt from centerline y=220, well inside the 15pt half-width of a 30pt stroke.
    check(state.ink.size() == countWithThick - 1,
          "delete-stroke eraser must hit visible painted thickness of wide strokes");
}

int main() {
    try {
        savedHistory();
        brushAndHistory();
        partialAndWhiteoutEraser();
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
        check(state.ink.size() == 2, "standard eraser splits segment between sparse samples");
        state.undo();
        check(state.ink.size() == 1, "erase undo restores ink");
        state.eraserMode = EraserMode::DeleteStroke;
        state.erase(x + 50, y);
        check(state.ink.empty(), "delete-stroke eraser removes sparse segment");
        state.undo();
        check(state.ink.size() == 1, "delete-stroke undo restores ink");
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
