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

int main() {
    try {
        pressureAndRelease();
        eraserAndPan();
        backlog();
        std::cout << "PASS: pressure/release, stylus eraser, grouped erase undo, temporary pan, backlog flags/order\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
