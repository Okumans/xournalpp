#pragma once

#include <chrono>
#include <unordered_map>

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
        if (!shift) {
            resetShiftAxes();
        }
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
        synchronizePageStructure();
        if (state.drawingMode == DrawingMode::Spline && !eraserEnd && state.brush.tool != Tool::Eraser &&
            state.brush.tool != Tool::Hand && !spaceHeld) {
            return splinePress(device, sample);
        }
        if (owner || state.current) return false;
        shiftClickConnection.reset();
        pointerDragged = false;
        pressureMode = state.pressureSensitivity && validPressure(sample.pressure);
        lastPressure = pressureMode ? *sample.pressure : 1;
        const auto hit = state.hitPage(sample.x, sample.y);
        if (spaceHeld || state.brush.tool == Tool::Hand || !hit) {
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
            InputSample drawingSample = sample;
            std::optional<ShiftClickConnection> connection;
            if (supportsShiftLineConstraint(state.brush.tool, eraserEnd)) {
                auto& constraint = shiftConstraints[hit->page];
                const auto raw = hit->local;
                const auto projected = constrainAtBegin(constraint, raw, shiftHeld);
                const auto screen = state.pageScreenPoint(hit->page, projected.x, projected.y);
                drawingSample.x = screen.x;
                drawingSample.y = screen.y;
                if (shiftHeld) {
                    connection = shiftClickTarget(constraint, hit->page);
                }
            }
            if (!state.begin(device, drawingSample.x, drawingSample.y, pressure(sample.pressure), eraserEnd,
                             shiftHeld, controlHeld, altHeld)) {
                return false;
            }
            shiftClickConnection = connection;
            state.current->pressureSensitive = pressureMode && state.current->brush.tool == Tool::Pen;
            if (stabilizedMode()) {
                stabilizer = CanvasStabilizer(stabilizerConfig);
                stabilizer.begin({drawingSample.x, drawingSample.y, pressure(sample.pressure),
                                  timestamp(sample.timeMs)});
            }
        }
        owner = device;
        lastX = sample.x;
        lastY = sample.y;
        startX = sample.x;
        startY = sample.y;
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
                if (std::hypot(sample.x - startX, sample.y - startY) > 4.0) {
                    pointerDragged = true;
                }
                {
                    InputSample drawingSample = sample;
                    if (state.current && supportsShiftLineConstraint(state.current->brush.tool, false)) {
                        const int page = state.current->page;
                        auto& constraint = shiftConstraints[page];
                        const auto raw = state.pageLocalPoint(page, sample.x, sample.y,
                                                              pressure(sample.pressure));
                        const auto projected = constrainDuringStroke(constraint, raw, shiftHeld);
                        const auto screen = state.pageScreenPoint(page, projected.x, projected.y);
                        drawingSample.x = screen.x;
                        drawingSample.y = screen.y;
                    }
                    if (stabilizedMode()) {
                        const auto filtered = stabilizer.process({drawingSample.x, drawingSample.y,
                                                                  pressure(sample.pressure),
                                                                  timestamp(sample.timeMs)});
                        state.append(device, filtered.x, filtered.y, filtered.pressure,
                                     shiftHeld, controlHeld, altHeld);
                    } else {
                        state.append(device, drawingSample.x, drawingSample.y, pressure(sample.pressure),
                                     shiftHeld, controlHeld, altHeld);
                    }
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

    void finish(Device device, std::optional<double> releasePressure = std::nullopt,
                std::optional<Point> releasePoint = std::nullopt) {
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
            finishShiftClick(releasePoint, releasePressure);
            const auto oldInkCount = state.ink.size();
            state.finish(device, pressure(releasePressure));
            if (state.ink.size() > oldInkCount) {
                rememberShiftEndpoint(state.ink.back());
            }
            stabilizer.reset();
            shiftClickConnection.reset();
            pointerDragged = false;
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
        shiftClickConnection.reset();
        pointerDragged = false;
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
        shiftConstraints.clear();
        shiftClickConnection.reset();
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

    struct ShiftEndpoint {
        Point point;
        int page = 0;
        size_t layerId = 1;
        Tool tool = Tool::Pen;
        std::uintptr_t strokeReference = 0;
    };

    struct StraightLineConstraint {
        enum class Axis { Undecided, Horizontal, Vertical };

        Point lastPointerPosition;
        Point anchor;
        Axis axis = Axis::Undecided;
        bool shiftActive = false;
        std::optional<ShiftEndpoint> endpoint;
    };

    struct ShiftClickConnection {
        Point from;
        int page = 0;
        size_t layerId = 1;
        Tool tool = Tool::Pen;
    };

    static bool finitePosition(InputSample sample) { return std::isfinite(sample.x) && std::isfinite(sample.y); }

    static bool validPressure(std::optional<double> value) { return value && std::isfinite(*value) && *value >= 0; }

    static bool isDrawingBrush(Tool tool) { return tool == Tool::Pen || tool == Tool::Highlighter; }

    bool supportsShiftLineConstraint(Tool tool, bool eraserEnd) const {
        return !eraserEnd && isDrawingBrush(tool) &&
               (state.drawingMode == DrawingMode::Freehand || state.drawingMode == DrawingMode::ShapeRecognizer);
    }

    static void resetShiftAxis(StraightLineConstraint& constraint) {
        constraint.axis = StraightLineConstraint::Axis::Undecided;
        constraint.shiftActive = false;
    }

    void resetShiftAxes() {
        for (auto& [page, constraint]: shiftConstraints) {
            (void)page;
            resetShiftAxis(constraint);
        }
    }

    void synchronizePageStructure() {
        if (knownPageStructureGeneration == state.pageStructureGeneration) {
            return;
        }
        shiftConstraints.clear();
        shiftClickConnection.reset();
        knownPageStructureGeneration = state.pageStructureGeneration;
    }

    static Point projectOnShiftAxis(StraightLineConstraint& constraint, Point raw) {
        const double deltaX = raw.x - constraint.anchor.x;
        const double deltaY = raw.y - constraint.anchor.y;
        if (constraint.axis == StraightLineConstraint::Axis::Undecided && (deltaX != 0 || deltaY != 0)) {
            constraint.axis = std::abs(deltaX) >= std::abs(deltaY) ? StraightLineConstraint::Axis::Horizontal :
                                                                     StraightLineConstraint::Axis::Vertical;
        }
        switch (constraint.axis) {
            case StraightLineConstraint::Axis::Horizontal:
                raw.y = constraint.anchor.y;
                break;
            case StraightLineConstraint::Axis::Vertical:
                raw.x = constraint.anchor.x;
                break;
            case StraightLineConstraint::Axis::Undecided:
                raw.x = constraint.anchor.x;
                raw.y = constraint.anchor.y;
                break;
        }
        return raw;
    }

    static Point projectOnSavedAxis(const StraightLineConstraint& constraint, Point raw) {
        switch (constraint.axis) {
            case StraightLineConstraint::Axis::Horizontal:
                raw.y = constraint.anchor.y;
                break;
            case StraightLineConstraint::Axis::Vertical:
                raw.x = constraint.anchor.x;
                break;
            case StraightLineConstraint::Axis::Undecided:
                raw.x = constraint.anchor.x;
                raw.y = constraint.anchor.y;
                break;
        }
        return raw;
    }

    static Point constrainAtBegin(StraightLineConstraint& constraint, Point raw, bool shift) {
        if (!shift) {
            resetShiftAxis(constraint);
            constraint.lastPointerPosition = raw;
            return raw;
        }
        if (!constraint.shiftActive) {
            constraint.anchor = raw;
            constraint.axis = StraightLineConstraint::Axis::Undecided;
            constraint.shiftActive = true;
        }
        auto projected = projectOnSavedAxis(constraint, raw);
        constraint.lastPointerPosition = raw;
        return projected;
    }

    static Point constrainDuringStroke(StraightLineConstraint& constraint, Point raw, bool shift) {
        if (!shift) {
            resetShiftAxis(constraint);
            constraint.lastPointerPosition = raw;
            return raw;
        }
        if (!constraint.shiftActive) {
            constraint.anchor = constraint.lastPointerPosition;
            constraint.axis = StraightLineConstraint::Axis::Undecided;
            constraint.shiftActive = true;
        }
        auto projected = projectOnShiftAxis(constraint, raw);
        constraint.lastPointerPosition = raw;
        return projected;
    }

    std::optional<ShiftClickConnection> shiftClickTarget(const StraightLineConstraint& constraint, int page) const {
        if (!constraint.endpoint || !isDrawingBrush(state.brush.tool)) {
            return std::nullopt;
        }
        const auto& endpoint = *constraint.endpoint;
        if (endpoint.page != page || endpoint.layerId != state.activeLayerId(page) || endpoint.tool != state.brush.tool ||
            endpoint.strokeReference == 0) {
            return std::nullopt;
        }
        const bool stillExists = std::any_of(state.ink.begin(), state.ink.end(), [&](const Ink& stroke) {
            return stroke.pluginReference == endpoint.strokeReference && stroke.page == endpoint.page &&
                   stroke.layerId == endpoint.layerId && stroke.brush.tool == endpoint.tool;
        });
        if (!stillExists) {
            return std::nullopt;
        }
        return ShiftClickConnection{endpoint.point, endpoint.page, endpoint.layerId, endpoint.tool};
    }

    void finishShiftClick(std::optional<Point> releasePoint, std::optional<double> releasePressure) {
        if (!shiftClickConnection || !state.current || pointerDragged ||
            state.current->page != shiftClickConnection->page ||
            state.current->layerId != shiftClickConnection->layerId ||
            state.current->brush.tool != shiftClickConnection->tool) {
            return;
        }
        const Point release = releasePoint.value_or(Point{lastX, lastY});
        if (std::hypot(release.x - startX, release.y - startY) > 4.0) {
            return;
        }
        const double endPressure = pressure(releasePressure);
        const auto end = state.pageLocalPoint(shiftClickConnection->page, release.x, release.y, endPressure);
        if (!std::isfinite(end.x) || !std::isfinite(end.y)) {
            return;
        }
        state.current->points = {shiftClickConnection->from, end};
    }

    void rememberShiftEndpoint(const Ink& stroke) {
        if (!isDrawingBrush(stroke.brush.tool) || stroke.points.empty() || stroke.pluginReference == 0) {
            return;
        }
        auto& constraint = shiftConstraints[stroke.page];
        constraint.endpoint = ShiftEndpoint{stroke.points.back(), stroke.page, stroke.layerId, stroke.brush.tool,
                                            stroke.pluginReference};
    }

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
    double startX = 0;
    double startY = 0;
    bool pointerDragged = false;
    std::uint64_t knownPageStructureGeneration = 0;
    std::unordered_map<int, StraightLineConstraint> shiftConstraints;
    std::optional<ShiftClickConnection> shiftClickConnection;
    CanvasStabilizer stabilizer;
    Device splineSource = Device::Mouse;
};

}  // namespace prototype
