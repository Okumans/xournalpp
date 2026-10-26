#include <iostream>
#include <limits>
#include <stdexcept>

#include "StylusSamples.h"

using namespace prototype;

static void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

static void pressureAndRelease() {
    CanvasState state;
    CanvasInput input(state);
    input.begin(Device::Stylus, {20, 20, 0.2});
    input.motion(Device::Stylus, {40, 20, 0.7});
    input.motion(Device::Stylus, {60, 20, std::nullopt});
    input.motion(Device::Mouse, {80, 20, 1});
    input.finish(Device::Mouse);
    check(state.current && state.current->points.size() == 3, "another device must not steal stylus input");
    check(state.current->points.back().pressure == 0.7, "missing pressure must reuse the previous device value");
    input.finish(Device::Stylus, 0);
    check(state.ink[0].pressureSensitive, "stylus pressure mode must survive finalization for native saving");
    check(state.ink[0].points.back().x == 60 && state.ink[0].points.back().pressure == 0.7,
          "release must not append a zero-pressure hover tail");
    input.begin(Device::Stylus, {20, 60, 0.1});
    input.motion(Device::Stylus, {20.1, 60, 0.8});
    check(state.current->points.size() == 1 && state.current->points[0].pressure == 0.8,
          "sub-threshold motion must thicken the initial dot without adding jitter");
    input.finish(Device::Stylus, 0.9);
    check(state.ink[1].points[0].pressure == 0.9, "dot finalization must retain peak pressure");
    input.begin(Device::Stylus, {20, 90, std::nullopt});
    input.motion(Device::Stylus, {40, 90, 0.2});
    input.finish(Device::Stylus);
    check(state.ink[2].points.back().pressure == 1, "pressure-less input must keep default constant width");
    check(!state.ink[2].pressureSensitive, "missing initial pressure must serialize as a constant-width stroke");
    input.begin(Device::Stylus, {20, 120, 0.4});
    input.motion(Device::Stylus, {40, 120, 0.6});
    input.interrupt();
    check(state.ink.size() == 4 && !input.active(), "a broken grab must finalize the delivered samples");
    input.begin(Device::Stylus, {20, 140, 0.4});
    input.cancel(Device::Stylus);
    check(state.ink.size() == 4 && !state.current, "explicit sequence cancellation must discard unfinished ink");
}

static void eraserAndPan() {
    CanvasState state;
    CanvasInput input(state);
    for (int x: {30, 90, 150, 210}) {
        input.begin(Device::Mouse, {double(x), 40, std::nullopt});
        input.finish(Device::Mouse);
    }
    input.begin(Device::Stylus, {150, 40, 0.5}, true);
    input.motion(Device::Stylus, {30, 40, 0.5});
    input.motion(Device::Stylus, {210, 40, 0.5});
    input.finish(Device::Stylus);
    check(state.ink.size() == 1 && state.ink[0].points[0].x == 90, "eraser end must delete contacted strokes");
    check(state.brush.tool == Tool::Pen, "temporary eraser must preserve the toolbar brush");
    state.undo();
    check(state.ink.size() == 4, "one undo must restore the complete eraser drag");
    for (size_t i = 0; i < state.ink.size(); ++i) {
        check(state.ink[i].points[0].x == 30 + i * 60, "erasing undo must preserve original element order");
    }
    state.redo();
    check(state.ink.size() == 1, "one redo must reapply the complete eraser drag");
    input.motion(Device::Stylus, {90, 40, 0});
    check(state.ink.size() == 1, "hovering without an active sequence must not erase");
    input.spaceHeld = true;
    input.begin(Device::Mouse, {90, 80, std::nullopt});
    input.spaceHeld = false;
    input.motion(Device::Mouse, {120, 100, std::nullopt});
    input.finish(Device::Mouse);
    check(state.offsetX == 30 && state.offsetY == 20 && state.ink.size() == 1,
          "temporary pan must survive Space release until the pointer sequence ends");
    input.begin(Device::Mouse, {-50, -50, std::nullopt});
    input.motion(Device::Mouse, {-30, -40, std::nullopt});
    input.finish(Device::Mouse);
    check(state.offsetX == 50 && state.offsetY == 30, "a drag starting outside a page must pan");
    input.spaceHeld = true;
    input.begin(Device::Mouse, {90, 80, std::nullopt});
    input.reset();
    check(!input.active() && !input.spaceHeld && !state.current, "focus loss must release temporary input tools");
}

static void dedicatedHandPan() {
    CanvasState state;
    CanvasInput input(state);
    input.begin(Device::Mouse, {40, 40, std::nullopt});
    input.finish(Device::Mouse);
    state.selected = {0};
    const auto selectedPoints = state.ink.front().points;
    state.brush.tool = Tool::Hand;

    check(input.begin(Device::Mouse, {100, 100, std::nullopt}) && input.panning(),
          "The dedicated Hand tool must pan when a drag begins over a page");
    input.motion(Device::Mouse, {135, 120, std::nullopt});
    input.finish(Device::Mouse);
    check(state.offsetX == 35 && state.offsetY == 20 && state.ink.size() == 1 &&
                  state.ink.front().points == selectedPoints && state.selected == std::vector<size_t>{0} &&
                  state.hasSelection(),
          "Hand panning must preserve document ink and the current selection");
}

static void backlog() {
    CanvasState state;
    CanvasInput input(state);
    input.begin(Device::Stylus, {20, 20, 0.2});
    GdkTimeCoord samples[3]{};
    for (size_t i = 0; i < 3; ++i) {
        samples[i].flags = GdkAxisFlags(GDK_AXIS_FLAG_X | GDK_AXIS_FLAG_Y);
        samples[i].axes[GDK_AXIS_X] = 30 + i * 10;
        samples[i].axes[GDK_AXIS_Y] = 20;
    }
    samples[0].flags = GdkAxisFlags(samples[0].flags | GDK_AXIS_FLAG_PRESSURE);
    samples[0].axes[GDK_AXIS_PRESSURE] = 0.7;
    samples[1].axes[GDK_AXIS_PRESSURE] = std::numeric_limits<double>::quiet_NaN();
    samples[1].flags = GdkAxisFlags(samples[1].flags | GDK_AXIS_FLAG_PRESSURE);
    samples[2].flags = GDK_AXIS_FLAG_X;  // Missing Y is not a valid point.
    for (const auto& sample: samples) {
        if (auto translated = historySample(sample)) {
            input.motion(Device::Stylus, *translated);
        }
    }
    input.motion(Device::Stylus, {60, 20, 0.9});
    input.finish(Device::Stylus);
    check(state.ink[0].points.size() == 4 && state.ink[0].points[2].x == 40 && state.ink[0].points[2].pressure == 0.7,
          "backlog must preserve order and missing-axis semantics");
    check(isEraserEnd(GDK_DEVICE_TOOL_TYPE_ERASER) && !isEraserEnd(GDK_DEVICE_TOOL_TYPE_PEN),
          "backend tool mapping must distinguish eraser and pen ends");
}

static void whiteoutAndPartialInput() {
    CanvasState state;
    CanvasInput input(state);
    input.begin(Device::Stylus, {20, 60, 0.3});
    input.motion(Device::Stylus, {180, 60, 0.9});
    input.finish(Device::Stylus, 0.9);
    check(state.ink.size() == 1 && state.ink[0].pressureSensitive, "expected one pressure-sensitive pen stroke");

    // Stylus eraser end in default Standard mode splits the pen stroke without changing toolbar brush.
    input.begin(Device::Stylus, {100, 60, 0.5}, true);
    input.finish(Device::Stylus);
    check(state.brush.tool == Tool::Pen && state.ink.size() == 2,
          "stylus eraser end in standard mode must split stroke and preserve pen tool");
    state.undo();
    check(state.ink.size() == 1, "undo must restore split stroke");

    // Stylus eraser end in Whiteout mode draws opaque white constant-width ink without changing toolbar brush.
    state.eraserMode = EraserMode::Whiteout;
    state.eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::Thick)];
    input.begin(Device::Stylus, {40, 60, 0.2}, true);
    input.motion(Device::Stylus, {140, 60, 0.8});
    input.finish(Device::Stylus, 0.8);
    check(state.brush.tool == Tool::Pen && state.ink.size() == 2 && state.ink[1].brush.tool == Tool::Eraser &&
                  !state.ink[1].pressureSensitive && std::abs(state.ink[1].brush.width - 12.0) < 1e-9,
          "stylus eraser end in whiteout mode must commit constant-width whiteout ink");
}

static void interPageGapPanAndCrossPageInput() {
    CanvasState state;
    state.setPages({{595, 842}, {595, 842}});
    CanvasInput input(state);

    // Dragging inside the 15pt vertical gap between Page 1 and Page 2 must temporarily pan.
    const double gapY = 842.0 + 7.5;
    check(input.begin(Device::Mouse, {120, gapY, std::nullopt}) && input.panning(),
          "starting a drag inside the inter-page gap must enter temporary hand-pan mode");
    input.motion(Device::Mouse, {120, gapY - 40.0, std::nullopt});
    input.finish(Device::Mouse);
    check(state.ink.empty() && std::abs(state.offsetY + 40.0) < 1e-9,
          "inter-page gap pan must move offsetY without committing ink");

    // Drawing directly on Page 2 records Page 2 local coordinates.
    const auto p1a = state.pageScreenPoint(0, 40, 810);
    const auto p1b = state.pageScreenPoint(0, 180, 810);
    input.begin(Device::Mouse, {p1a.x, p1a.y, std::nullopt});
    input.motion(Device::Mouse, {p1b.x, p1b.y, std::nullopt});
    input.finish(Device::Mouse);

    const auto p2a = state.pageScreenPoint(1, 40, 50);
    const auto p2b = state.pageScreenPoint(1, 180, 50);
    input.begin(Device::Stylus, {p2a.x, p2a.y, 0.4});
    input.motion(Device::Stylus, {p2b.x, p2b.y, 0.8});
    input.finish(Device::Stylus, 0.8);
    check(state.ink.size() == 2 && state.ink[0].page == 0 && state.ink[1].page == 1 &&
                  std::abs(state.ink[1].points.front().y - 50.0) < 1e-9,
          "CanvasInput must route multi-page strokes to their hit page in local coordinates");

    // Continuous stylus eraser drag across Page 1 -> gap -> Page 2 splits both in a single undo step.
    const auto e1 = state.pageScreenPoint(0, 110, 810);
    const auto gapMid = state.pageScreenPoint(0, 110, 849.5);
    const auto e2 = state.pageScreenPoint(1, 110, 50);
    input.begin(Device::Stylus, {e1.x, e1.y, 0.5}, true);
    input.motion(Device::Stylus, {gapMid.x, gapMid.y, 0.5});
    input.motion(Device::Stylus, {e2.x, e2.y, 0.5});
    input.finish(Device::Stylus);
    check(state.ink.size() == 4, "cross-page stylus eraser drag must split strokes on both pages");
    state.undo();
    check(state.ink.size() == 2, "one undo must restore cross-page stylus eraser drag");
}

void importedPressureOverrides() {
    CanvasState state;
    CanvasInput input(state);
    state.fit(800, 1000);
    state.minimumPressure = 0.16;
    state.pressureMultiplier = 1.5;

    const auto x = state.offsetX + 100 * state.zoom;
    const auto y = state.offsetY + 100 * state.zoom;
    input.begin(Device::Stylus, {x, y, 0.05});
    input.motion(Device::Stylus, {x + 40 * state.zoom, y, 0.40});
    input.finish(Device::Stylus);
    check(state.ink.size() == 1 && state.ink.back().pressureSensitive &&
                  std::abs(state.ink.back().points.front().pressure - 0.16) < 1e-9 &&
                  std::abs(state.ink.back().points.back().pressure - 0.60) < 1e-9,
          "CanvasInput must apply imported minimumPressure and pressureMultiplier");

    state.pressureSensitivity = false;
    input.begin(Device::Stylus, {x, y + 40 * state.zoom, 0.30});
    input.motion(Device::Stylus, {x + 40 * state.zoom, y + 40 * state.zoom, 0.80});
    input.finish(Device::Stylus);
    check(state.ink.size() == 2 && !state.ink.back().pressureSensitive &&
                  std::abs(state.ink.back().points.front().pressure - 1.0) < 1e-9 &&
                  std::abs(state.ink.back().points.back().pressure - 1.0) < 1e-9,
          "Disabling pressureSensitivity must force constant-width stylus strokes");
}

static void shiftLineConstraintPersistsUntilRelease() {
    CanvasState state;
    CanvasInput input(state);
    input.setModifiers(true, false, false);

    input.begin(Device::Mouse, {100, 100, std::nullopt});
    input.motion(Device::Mouse, {140, 104, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, Point{140, 104});
    check(state.ink.size() == 1 && state.ink[0].points.back().y == 100,
          "Shift movement must project onto the dominant horizontal axis");

    input.begin(Device::Mouse, {160, 130, std::nullopt});
    check(state.current && state.current->points.front().y == 100,
          "the Shift axis must continue across released strokes while Shift remains held");
    input.motion(Device::Mouse, {190, 150, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, Point{190, 150});
    check(state.ink.size() == 2 && state.ink[1].points.back().y == 100,
          "the held horizontal constraint must apply to the next stroke");

    input.setModifiers(false, false, false);
    input.begin(Device::Mouse, {200, 180, std::nullopt});
    check(state.current && state.current->points.front().y == 180,
          "releasing Shift must remove the saved axis before the next stroke");
    input.motion(Device::Mouse, {220, 205, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, Point{220, 205});
    check(state.ink.size() == 3 && state.ink[2].points.back().y == 205,
          "drawing after Shift release must follow the raw pointer again");

    input.begin(Device::Mouse, {250, 240, std::nullopt});
    input.motion(Device::Mouse, {270, 240, std::nullopt});
    input.setModifiers(true, false, false);
    input.motion(Device::Mouse, {290, 260, std::nullopt});
    check(state.current && state.current->points.back().y == 240,
          "pressing Shift during a stroke must anchor to the last raw pointer position");
    input.setModifiers(false, false, false);
    input.motion(Device::Mouse, {300, 280, std::nullopt});
    check(state.current && state.current->points.back().y == 280,
          "releasing Shift during a stroke must immediately restore raw pointer movement");
    input.finish(Device::Mouse);
}

static void shiftClickConnectsOnlyToCurrentEndpoint() {
    CanvasState state;
    CanvasInput input(state);
    input.begin(Device::Mouse, {20, 30, std::nullopt});
    input.motion(Device::Mouse, {60, 30, std::nullopt});
    input.finish(Device::Mouse);
    const Point endpoint = state.ink.back().points.back();

    input.setModifiers(true, false, false);
    input.begin(Device::Mouse, {100, 80, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, Point{101, 81});
    check(state.ink.size() == 2 && state.ink.back().points.size() == 2 &&
                  state.ink.back().points.front().x == endpoint.x && state.ink.back().points.front().y == endpoint.y &&
                  state.ink.back().points.back().x == 101 && state.ink.back().points.back().y == 81,
          "a same-page, same-layer Pen Shift-click must connect from the previous stroke endpoint");

    input.begin(Device::Mouse, {120, 100, std::nullopt});
    input.motion(Device::Mouse, {140, 100, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, Point{140, 100});
    const Point draggedEndpoint = state.ink.back().points.back();
    check(state.ink.size() == 3 && state.ink.back().points.front().x == 100 &&
                  state.ink.back().points.front().y == 80,
          "a Shift drag beyond the click threshold must remain an independent stroke");

    state.brush.tool = Tool::Highlighter;
    input.begin(Device::Mouse, {100, 120, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, Point{101, 121});
    check(state.ink.size() == 4 && state.ink.back().brush.tool == Tool::Highlighter &&
                  state.ink.back().points.front().x != draggedEndpoint.x,
          "a Shift-click with a different drawing tool must start its own stroke");

    CanvasState undoneState;
    CanvasInput undoneInput(undoneState);
    undoneInput.begin(Device::Mouse, {20, 30, std::nullopt});
    undoneInput.motion(Device::Mouse, {60, 30, std::nullopt});
    undoneInput.finish(Device::Mouse);
    undoneState.undo();
    undoneInput.setModifiers(true, false, false);
    undoneInput.begin(Device::Mouse, {100, 80, std::nullopt});
    undoneInput.finish(Device::Mouse, std::nullopt, Point{101, 81});
    check(undoneState.ink.size() == 1 && undoneState.ink.back().points.front().x == 100,
          "a Shift-click must not connect to an endpoint whose stroke was undone");
}

static void shiftClickRespectsPageAndLayer() {
    CanvasState state;
    state.setPages({{595, 842}, {595, 842}});
    CanvasInput input(state);
    const auto firstStart = state.pageScreenPoint(0, 40, 40);
    const auto firstEnd = state.pageScreenPoint(0, 100, 40);
    input.begin(Device::Mouse, {firstStart.x, firstStart.y, std::nullopt});
    input.motion(Device::Mouse, {firstEnd.x, firstEnd.y, std::nullopt});
    input.finish(Device::Mouse);
    input.setModifiers(true, false, false);

    const auto otherPage = state.pageScreenPoint(1, 150, 70);
    input.begin(Device::Mouse, {otherPage.x, otherPage.y, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, otherPage);
    check(state.ink.size() == 2 && state.ink.back().page == 1 &&
                  state.ink.back().points.front().x == 150 && state.ink.back().points.back().x == 150,
          "a Shift-click on a different page must not connect to the prior page's stroke");

    const auto pageOneStart = state.pageScreenPoint(0, 180, 120);
    const auto pageOneRelease = state.pageScreenPoint(0, 181, 121);
    state.selectLayer(0, 1);
    input.begin(Device::Mouse, {pageOneStart.x, pageOneStart.y, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, pageOneRelease);
    check(state.ink.size() == 3 && state.ink.back().points.front().x == 100 &&
                  state.ink.back().points.back().x == 181,
          "returning to a page must keep its own eligible Pen endpoint");

    check(state.addLayerAboveCurrent(), "the test must create a second foreground layer");
    const auto secondLayerStart = state.pageScreenPoint(0, 240, 160);
    const auto secondLayerRelease = state.pageScreenPoint(0, 241, 161);
    input.begin(Device::Mouse, {secondLayerStart.x, secondLayerStart.y, std::nullopt});
    input.finish(Device::Mouse, std::nullopt, secondLayerRelease);
    check(state.ink.size() == 4 && state.ink.back().layerId == 2 &&
                  state.ink.back().points.size() == 2 && state.ink.back().points.front().x == 180 &&
                  state.ink.back().points.back().x == 180,
          "a Shift-click on a different layer must not connect to the prior layer's stroke");
}

static void shiftProjectionPrecedesStabilization() {
    CanvasState state;
    CanvasInput input(state);
    StabilizerConfig config;
    config.averaging = AveragingMethod::Arithmetic;
    config.bufferSize = 4;
    input.setStabilizerConfig(config);
    input.setModifiers(true, false, false);
    input.begin(Device::Mouse, {80, 80, std::nullopt});
    input.motion(Device::Mouse, {110, 110, std::nullopt});
    input.motion(Device::Mouse, {150, 95, std::nullopt});
    check(state.current && std::all_of(state.current->points.begin(), state.current->points.end(),
                                       [](const Point& point) { return point.y == 80; }),
          "Shift projection must happen before the stabilizer so preview samples stay on the saved axis");
    input.finish(Device::Mouse);
    check(std::all_of(state.ink.back().points.begin(), state.ink.back().points.end(),
                      [](const Point& point) { return point.y == 80; }),
          "stabilized Shift strokes must remain on the saved axis after finalization");
}

static void pageInsertionDoesNotReuseAnotherPagesShiftAxis() {
    CanvasState state;
    state.setPages({{595, 842}, {595, 842}});
    CanvasInput input(state);
    input.setModifiers(true, false, false);
    const auto first = state.pageScreenPoint(1, 100, 80);
    const auto moved = state.pageScreenPoint(1, 130, 85);
    input.begin(Device::Mouse, {first.x, first.y, std::nullopt});
    input.motion(Device::Mouse, {moved.x, moved.y, std::nullopt});
    input.finish(Device::Mouse);

    state.insertPageAfter(0, {595, 842});
    const auto insertedPage = state.pageScreenPoint(1, 90, 90);
    input.begin(Device::Mouse, {insertedPage.x, insertedPage.y, std::nullopt});
    check(state.current && state.current->points.front().x == 90 && state.current->points.front().y == 90,
          "inserting a page must clear constraints keyed to the old page index");
    input.finish(Device::Mouse);
}

int main() {
    try {
        pressureAndRelease();
        eraserAndPan();
        dedicatedHandPan();
        backlog();
        whiteoutAndPartialInput();
        interPageGapPanAndCrossPageInput();
        importedPressureOverrides();
        shiftLineConstraintPersistsUntilRelease();
        shiftClickConnectsOnlyToCurrentEndpoint();
        shiftClickRespectsPageAndLayer();
        shiftProjectionPrecedesStabilization();
        pageInsertionDoesNotReuseAnotherPagesShiftAxis();
        std::cout << "PASS: pressure/release, stylus eraser, grouped erase undo, temporary and dedicated hand pan, "
                     "backlog flags/order, Photoshop-style Shift stroke constraints\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
