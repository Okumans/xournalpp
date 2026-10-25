#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "CanvasInput.h"

namespace {

using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(double a, double b) { return std::abs(a - b) < 1e-8; }

void penTrailIsTransientAndFades() {
    using namespace prototype;

    CanvasState state;
    state.setPages({{300, 400}, {300, 400}});
    state.laserFadeOutTimeMs = 200;
    const auto savedRevision = state.revision();
    const auto savedInkCount = state.ink.size();
    state.brush = {Tool::LaserPointerPen, behavior::laserPointerColor, behavior::laserPenWidths[2]};
    CanvasInput input(state);

    const auto start = state.pageScreenPoint(0, 40, 60);
    const auto finish = state.pageScreenPoint(0, 110, 90);
    require(input.begin(Device::Stylus, {start.x, start.y, 0.25}), "Laser Pointer Pen rejected page input");
    require(state.currentLaser && state.currentLaser->pressureSensitive,
            "Pressure-sensitive laser pen did not start as transient input");
    input.motion(Device::Stylus, {finish.x, finish.y, 0.8});
    input.finish(Device::Stylus, 0.9);

    require(!state.currentLaser && state.laserTrails.size() == 1 && state.laserTrails.front().strokes.size() == 1,
            "Finished laser stroke was not retained in its transient page trail");
    const auto& stroke = state.laserTrails.front().strokes.front();
    require(stroke.brush.tool == Tool::LaserPointerPen && stroke.points.size() == 2 &&
                    near(stroke.points.front().pressure, 0.25) && near(stroke.points.back().pressure, 0.8),
            "Laser pen lost its samples or pressure values");
    require(state.ink.size() == savedInkCount && state.revision() == savedRevision && !state.canUndo(),
            "Laser stroke entered the document, modified state, or created undo history");

    const auto fadeStart = *state.laserTrails.front().fadeStart;
    require(!state.advanceLaserFades(fadeStart + 49ms) && state.laserTrails.front().alpha == 255,
            "Laser trail began fading before the configured delay and first fade interval");
    require(state.advanceLaserFades(fadeStart + 50ms) && state.laserTrails.front().alpha == 230,
            "Laser fade did not reduce alpha by the source-matched 25/255 step");
    require(state.advanceLaserFades(fadeStart + 500ms) && state.laserTrails.front().alpha == 5,
            "Laser fade did not preserve the final partial alpha step");
    require(state.advanceLaserFades(fadeStart + 550ms) && state.laserTrails.empty(),
            "Laser trail was not removed at the end of its fade");
}

void highlighterCancelAndResume() {
    using namespace prototype;

    CanvasState state;
    state.setPages({{300, 400}, {300, 400}});
    state.laserFadeOutTimeMs = 500;
    state.brush = {Tool::LaserPointerHighlighter, behavior::laserPointerColor,
                   behavior::laserHighlighterWidths[2]};
    CanvasInput input(state);

    const auto first = state.pageScreenPoint(0, 50, 70);
    const auto second = state.pageScreenPoint(0, 180, 70);
    require(input.begin(Device::Mouse, {first.x, first.y, std::nullopt}),
            "Laser Pointer Highlighter rejected mouse input");
    input.motion(Device::Mouse, {second.x, second.y, std::nullopt});
    input.finish(Device::Mouse);
    require(state.laserTrails.size() == 1 && state.laserTrails.front().strokes.size() == 1 &&
                    !state.laserTrails.front().strokes.front().pressureSensitive,
            "Laser highlighter did not retain a pressure-independent trail");

    const auto oldFade = *state.laserTrails.front().fadeStart;
    state.advanceLaserFades(oldFade + 100ms);
    require(state.laserTrails.front().alpha == 205, "Laser highlighter fade clock did not advance");

    const auto resumed = state.pageScreenPoint(0, 70, 120);
    require(input.begin(Device::Mouse, {resumed.x, resumed.y, std::nullopt}),
            "A new laser stroke could not resume a fading trail");
    require(state.laserTrails.front().alpha == 255 && !state.laserTrails.front().fadeStart,
            "Beginning another stroke did not cancel fade and restore the existing trail");
    const auto resumedEnd = state.pageScreenPoint(0, 130, 150);
    input.motion(Device::Mouse, {resumedEnd.x, resumedEnd.y, std::nullopt});
    input.finish(Device::Mouse);
    require(state.laserTrails.front().strokes.size() == 2 && state.laserTrails.front().alpha == 255 &&
                    state.laserTrails.front().fadeStart,
            "Resumed laser trail did not collect the new stroke and restart its fade delay");

    const auto canceledStart = state.pageScreenPoint(0, 200, 200);
    require(input.begin(Device::Mouse, {canceledStart.x, canceledStart.y, std::nullopt}),
            "Laser highlighter rejected a second active stroke");
    input.cancel(Device::Mouse);
    require(!state.currentLaser && state.laserTrails.front().strokes.size() == 2 &&
                    state.laserTrails.front().fadeStart,
            "Cancelling active laser input persisted it or stopped fading completed strokes");

    const auto pageOne = state.pageScreenPoint(1, 50, 70);
    require(input.begin(Device::Stylus, {pageOne.x, pageOne.y, std::nullopt}),
            "Laser highlighter rejected input on a second page");
    input.finish(Device::Stylus);
    require(state.laserTrails.size() == 2 && state.laserTrails.back().page == 1,
            "Laser trails from separate pages were combined");
    state.clearLaserPointers();
    require(state.laserTrails.empty() && !state.currentLaser,
            "Clearing transient laser pointers left marks behind");
}

}  // namespace

int main() {
    using namespace prototype;
    require(near(widthsForTool(Tool::LaserPointerPen)[0], 0.7) &&
                    near(widthsForTool(Tool::LaserPointerPen)[2], 2.4) &&
                    near(widthsForTool(Tool::LaserPointerHighlighter)[4], 30.0),
            "Laser tool widths do not match ToolHandler presets");
    penTrailIsTransientAndFades();
    highlighterCancelAndResume();
    std::cout << "PASS: laser pointer tools remain transient and follow the source fade lifecycle\n";
}
