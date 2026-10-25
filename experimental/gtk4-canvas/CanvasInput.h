#pragma once

#include <chrono>

#include "BehaviorDefaults.h"
#include "CanvasState.h"
#include "CanvasStabilizer.h"

namespace prototype {

struct InputSample {
    double x;
    double y;
    std::optional<double> pressure;
    uint32_t timeMs = 0;
};

// Normalize backend events before touching the session document. Temporary input
// tools never change the toolbar's selected brush.
class CanvasInput {
public:
    explicit CanvasInput(CanvasState& state): state(state) {}

    bool spaceHeld = false;
    bool shiftHeld = false;
    bool controlHeld = false;
    bool altHeld = false;
    StabilizerConfig stabilizerConfig;
    bool splineInProgress = false;

    void setModifiers(bool shift, bool control, bool alt) {
        shiftHeld = shift;
        controlHeld = control;
        altHeld = alt;
        state.refreshCurrentShape(shiftHeld, controlHeld, altHeld);
    }

    void setStabilizerConfig(StabilizerConfig config) { stabilizerConfig = config; }

    bool begin(Device device, InputSample sample, bool eraserEnd = false) {
        if (!finitePosition(sample) || state.pinching) {
            return false;
        }
        if (state.drawingMode == DrawingMode::Spline && !eraserEnd && state.brush.tool != Tool::Eraser &&
            state.brush.tool != Tool::Hand && !spaceHeld) {
            return splinePress(device, sample);
        }
        if (owner || state.current) return false;
        pressureMode = state.pressureSensitivity && validPressure(sample.pressure);
        lastPressure = pressureMode ? *sample.pressure : 1;
        if (spaceHeld || state.brush.tool == Tool::Hand || !state.hitPage(sample.x, sample.y)) {
            mode = Mode::Pan;
        } else if ((eraserEnd || state.brush.tool == Tool::Eraser) && state.eraserMode != EraserMode::Whiteout) {
            mode = Mode::Erase;
            state.beginErase();
            state.erase(sample.x, sample.y);
        } else if (isSelectionTool(state.brush.tool) && !eraserEnd) {
            mode = Mode::Select;
            if (!state.beginSelection(sample.x, sample.y, shiftHeld)) {
                return false;
            }
        } else if (isLaserPointerTool(state.brush.tool) && !eraserEnd) {
            mode = Mode::Laser;
            if (!state.beginLaser(device, sample.x, sample.y, pressure(sample.pressure), state.brush, pressureMode)) {
                return false;
            }
        } else {
            mode = Mode::Draw;
            if (!state.begin(device, sample.x, sample.y, pressure(sample.pressure), eraserEnd,
                             shiftHeld, controlHeld, altHeld)) {
                return false;
            }
            state.current->pressureSensitive = pressureMode && state.current->brush.tool == Tool::Pen;
            if (stabilizedMode()) {
                stabilizer = CanvasStabilizer(stabilizerConfig);
                stabilizer.begin({sample.x, sample.y, pressure(sample.pressure), timestamp(sample.timeMs)});
            }
        }
        owner = device;
        lastX = sample.x;
        lastY = sample.y;
        return true;
    }

    void motion(Device device, InputSample sample) {
        if (!owner || *owner != device || !finitePosition(sample) || state.pinching) {
            return;
        }
        switch (mode) {
            case Mode::Pan:
                state.offsetX += sample.x - lastX;
                state.offsetY += sample.y - lastY;
                break;
            case Mode::Erase:
                state.erase(sample.x, sample.y);
                break;
            case Mode::Draw:
                if (stabilizedMode()) {
                    const auto filtered = stabilizer.process({sample.x, sample.y, pressure(sample.pressure),
                                                               timestamp(sample.timeMs)});
                    state.append(device, filtered.x, filtered.y, filtered.pressure,
                                 shiftHeld, controlHeld, altHeld);
                } else {
                    state.append(device, sample.x, sample.y, pressure(sample.pressure),
                                 shiftHeld, controlHeld, altHeld);
                }
                break;
            case Mode::Select:
                state.moveSelectionPointer(sample.x, sample.y, altHeld);
                break;
            case Mode::Spline:
                state.moveSplineTangent(sample.x, sample.y, pressure(sample.pressure));
                break;
            case Mode::Laser:
                state.appendLaser(device, sample.x, sample.y, pressure(sample.pressure));
                break;
        }
        lastX = sample.x;
        lastY = sample.y;
    }

    void finish(Device device, std::optional<double> releasePressure = std::nullopt) {
        if (!owner || *owner != device) {
            return;
        }
        if (mode == Mode::Draw) {
            // A stabilizer may smooth the tail to the final delivered motion,
            // while a release-only hover coordinate is ignored.
            if (stabilizedMode()) {
                for (const auto& sample: stabilizer.finalize()) {
                    state.append(device, sample.x, sample.y, sample.pressure,
                                 shiftHeld, controlHeld, altHeld);
                }
                if (state.current && stabilizerConfig.preprocessor != StrokePreprocessor::None &&
                    state.current->points.size() >= 3) {
                    const auto count = state.current->points.size();
                    state.current->points[count - 2].pressure =
                            (state.current->points[count - 2].pressure + state.current->points[count - 3].pressure) / 2;
                }
            }
            state.finish(device, pressure(releasePressure));
            stabilizer.reset();
        } else if (mode == Mode::Spline) {
            // Each press/drag sets a spline knot and its tangent. The spline
            // remains editable until it is closed, double-clicked, or escaped.
        } else if (mode == Mode::Erase) {
            state.finishErase();
        } else if (mode == Mode::Select) {
            state.finishSelection();
        } else if (mode == Mode::Laser) {
            state.finishLaser(device, pressure(releasePressure));
        }
        owner.reset();
    }

    bool splinePress(Device device, InputSample sample, bool doubleClick = false) {
        if (!finitePosition(sample) || owner || state.pinching || state.brush.tool == Tool::Eraser ||
            !isDrawingBrush(state.brush.tool)) {
            return false;
        }
        if (doubleClick && splineInProgress) {
            finalizeSpline();
            return true;
        }
        if (!splineInProgress) {
            pressureMode = state.pressureSensitivity && validPressure(sample.pressure);
            lastPressure = pressureMode ? *sample.pressure : 1;
            if (!state.begin(device, sample.x, sample.y, pressure(sample.pressure), false,
                             shiftHeld, controlHeld, altHeld)) {
                return false;
            }
            state.current->pressureSensitive = pressureMode && state.current->brush.tool == Tool::Pen;
            splineInProgress = true;
            splineSource = device;
        } else {
            if (state.splineKnots.empty()) return false;
            const auto first = state.pageScreenPoint(state.current->page, state.splineKnots.front().x,
                                                     state.splineKnots.front().y);
            if (std::hypot(first.x - sample.x, first.y - sample.y) < 10.0) {
                state.closeSplinePath();
                finalizeSpline();
                return true;
            }
            if (!state.addSplineKnot(sample.x, sample.y, pressure(sample.pressure))) return false;
        }
        owner = device;
        mode = Mode::Spline;
        lastX = sample.x;
        lastY = sample.y;
        return true;
    }

    void splineHover(double x, double y) {
        if (splineInProgress && !owner && std::isfinite(x) && std::isfinite(y)) {
            state.updateSplineHover(x, y);
        }
    }

    void finalizeSpline() {
        if (!splineInProgress) return;
        owner.reset();
        state.finish(splineSource);
        splineInProgress = false;
    }

    bool removeSplineKnot() { return splineInProgress && state.removeLastSplineKnot(); }
    bool moveLastSplineKnot(double dx, double dy) { return splineInProgress && state.moveLastSplineKnot(dx, dy); }
    bool rotateLastSplineTangent(double degrees) {
        return splineInProgress && state.rotateLastSplineTangent(degrees);
    }
    bool scaleLastSplineTangent(double factor) { return splineInProgress && state.scaleLastSplineTangent(factor); }

    void cancel(Device device) {
        if (owner && *owner != device) {
            return;
        }
        if (owner && mode == Mode::Spline) {
            state.cancel();
            splineInProgress = false;
            stabilizer.reset();
            owner.reset();
            return;
        }
        if (!owner && splineInProgress && device == splineSource) {
            state.cancel();
            splineInProgress = false;
            stabilizer.reset();
            return;
        }
        if (owner && mode == Mode::Erase) {
            state.finishErase();
        }
        if (owner && mode == Mode::Select) {
            state.cancelSelection();
        }
        if (owner && mode == Mode::Laser) {
            state.cancelLaser(*owner);
        }
        state.cancel(device);
        if (mode == Mode::Spline) splineInProgress = false;
        stabilizer.reset();
        owner.reset();
    }

    void cancel() {
        if (owner) {
            cancel(*owner);
        } else {
            state.cancel();
            stabilizer.reset();
            splineInProgress = false;
        }
    }

    void reset() {
        cancel();
        spaceHeld = false;
        shiftHeld = false;
        state.resetInput();
        controlHeld = false;
        altHeld = false;
    }

    bool active() const { return owner.has_value(); }

    bool active(Device device) const { return owner && *owner == device; }

    void interrupt() {
        // PenInputHandler ends an action when its grab is broken. Preserve the
        // delivered samples, while explicit sequence cancellation discards ink.
        if (owner) {
            finish(*owner);
        }
        reset();
    }
    bool panning() const { return owner && mode == Mode::Pan; }

private:
    enum class Mode { Draw, Erase, Pan, Select, Spline, Laser };

    static bool finitePosition(InputSample sample) { return std::isfinite(sample.x) && std::isfinite(sample.y); }

    static bool validPressure(std::optional<double> value) { return value && std::isfinite(*value) && *value >= 0; }

    static bool isDrawingBrush(Tool tool) { return tool == Tool::Pen || tool == Tool::Highlighter; }

    bool stabilizedMode() const {
        return mode == Mode::Draw && state.current && state.current->brush.tool != Tool::Eraser &&
               (state.drawingMode == DrawingMode::Freehand || state.drawingMode == DrawingMode::ShapeRecognizer) &&
               (stabilizerConfig.averaging != AveragingMethod::None ||
                stabilizerConfig.preprocessor != StrokePreprocessor::None);
    }

    static uint32_t timestamp(uint32_t value) {
        if (value) return value;
        using namespace std::chrono;
        return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    double pressure(std::optional<double> value) {
        if (!pressureMode) {
            return 1;  // Default pressure guessing is disabled in the original app.
        }
        if (validPressure(value)) {
            lastPressure = *value;
        }
        return std::max(state.minimumPressure, lastPressure * state.pressureMultiplier);
    }

    CanvasState& state;
    std::optional<Device> owner;
    Mode mode = Mode::Draw;
    bool pressureMode = false;
    double lastPressure = 1;
    double lastX = 0;
    double lastY = 0;
    CanvasStabilizer stabilizer;
    Device splineSource = Device::Mouse;
};

}  // namespace prototype
