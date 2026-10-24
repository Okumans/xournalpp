#pragma once

#include "BehaviorDefaults.h"
#include "CanvasState.h"

namespace prototype {

struct InputSample {
    double x;
    double y;
    std::optional<double> pressure;
};

// Normalize backend events before touching the session document. Temporary input
// tools never change the toolbar's selected brush.
class CanvasInput {
public:
    explicit CanvasInput(CanvasState& state): state(state) {}

    bool spaceHeld = false;

    bool begin(Device device, InputSample sample, bool eraserEnd = false) {
        if (owner || state.current || state.pinching || !finitePosition(sample)) {
            return false;
        }
        pressureMode = validPressure(sample.pressure);
        lastPressure = pressureMode ? *sample.pressure : 1;
        if (spaceHeld || !state.contains(state.documentPoint(sample.x, sample.y))) {
            mode = Mode::Pan;
        } else if ((eraserEnd || state.brush.tool == Tool::Eraser) && state.eraserMode != EraserMode::Whiteout) {
            mode = Mode::Erase;
            state.beginErase();
            state.erase(sample.x, sample.y);
        } else {
            mode = Mode::Draw;
            if (!state.begin(device, sample.x, sample.y, pressure(sample.pressure), eraserEnd)) {
                return false;
            }
            state.current->pressureSensitive = pressureMode && state.current->brush.tool == Tool::Pen;
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
                state.append(device, sample.x, sample.y, pressure(sample.pressure));
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
            // StrokeHandler finalizes the last delivered sample. A release may
            // report a hover position; it must not add a tail to the stroke.
            state.finish(device, pressure(releasePressure));
        } else if (mode == Mode::Erase) {
            state.finishErase();
        }
        owner.reset();
    }

    void cancel(Device device) {
        if (owner && *owner != device) {
            return;
        }
        if (owner && mode == Mode::Erase) {
            state.finishErase();
        }
        state.cancel(device);
        owner.reset();
    }

    void cancel() {
        if (owner) {
            cancel(*owner);
        } else {
            state.cancel();
        }
    }

    void reset() {
        cancel();
        spaceHeld = false;
        state.resetInput();
    }

    bool active() const { return owner.has_value(); }

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
    enum class Mode { Draw, Erase, Pan };

    static bool finitePosition(InputSample sample) { return std::isfinite(sample.x) && std::isfinite(sample.y); }

    static bool validPressure(std::optional<double> value) { return value && std::isfinite(*value) && *value >= 0; }

    double pressure(std::optional<double> value) {
        if (!pressureMode) {
            return 1;  // Default pressure guessing is disabled in the original app.
        }
        if (validPressure(value)) {
            lastPressure = *value;
        }
        return std::max(behavior::minimumPressure, lastPressure * behavior::pressureMultiplier);
    }

    CanvasState& state;
    std::optional<Device> owner;
    Mode mode = Mode::Draw;
    bool pressureMode = false;
    double lastPressure = 1;
    double lastX = 0;
    double lastY = 0;
};

}  // namespace prototype
