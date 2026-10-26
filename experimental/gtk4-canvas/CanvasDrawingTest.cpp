#include <cmath>
#include <iostream>
#include <stdexcept>

#include "CanvasInput.h"

using namespace prototype;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void geometry() {
    const auto line = linePoints({10, 20}, {48, 37}, false, true);
    check(line.size() == 2 && std::abs((line[1].x - line[0].x) - (line[1].y - line[0].y)) < 1e-6,
          "Shift line constraint should snap the closest direction to 45 degrees");
    const auto centered = linePoints({50, 50}, {70, 50}, true, false);
    check(centered.front().x == 30 && centered.back().x == 70, "Control should draw a line from its center");

    const auto square = rectanglePoints({10, 10}, {40, 30}, false, true);
    check(square.size() == 5 && std::abs(std::abs(square[2].x - square[0].x) - 30) < 1e-6 &&
                  std::abs(std::abs(square[2].y - square[0].y) - 30) < 1e-6 && square.front().x == square.back().x,
          "Shift rectangle should preserve drag direction while locking a square aspect ratio");
    const auto circle = ellipsePoints({10, 10}, {40, 30}, false, true);
    double minX = circle.front().x, maxX = minX, minY = circle.front().y, maxY = minY;
    for (const auto& p: circle) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
    }
    check(std::abs((maxX - minX) - (maxY - minY)) < 1e-6, "Shift ellipse should constrain a circle");
    check(arrowPoints({0, 0}, {100, 0}, 2, false, false, false).size() == 6 &&
                  arrowPoints({0, 0}, {100, 0}, 2, false, false, true).size() == 10,
          "Arrow tools should emit the expected connected arrowhead paths");
    check(coordinateSystemPoints({0, 0}, {30, 40}, false, false) ==
                  std::vector<Point>{{0, 0}, {0, 40}, {30, 40}},
          "Coordinate system tool should keep its L-shaped path");

    std::vector<Point> circleTrace;
    for (int i = 0; i <= 64; ++i) {
        const double angle = 2.0 * M_PI * i / 64.0;
        circleTrace.push_back({100 + 60 * std::cos(angle), 100 + 60 * std::sin(angle)});
    }
    const auto recognizedCircle = recognizeShape(circleTrace);
    check(recognizedCircle && recognizedCircle->size() > 20,
          "Shape recognizer should replace a sufficiently large closed circular trace");
    const std::array<Point, 4> rotatedCorners{{{60, 80}, {165, 116}, {145, 174}, {40, 138}}};
    std::vector<Point> rectangleTrace;
    for (size_t edge = 0; edge < rotatedCorners.size(); ++edge) {
        const auto a = rotatedCorners[edge];
        const auto b = rotatedCorners[(edge + 1) % rotatedCorners.size()];
        for (int step = 0; step < 12; ++step) {
            const double t = step / 12.0;
            rectangleTrace.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)});
        }
    }
    rectangleTrace.push_back(rotatedCorners.front());
    const auto recognizedRectangle = recognizeShape(rectangleTrace);
    check(recognizedRectangle && recognizedRectangle->size() == 5,
          "Shape recognizer should simplify a rotated four-corner rectangle");
    const std::array<Point, 3> triangleCorners{{{50, 60}, {155, 75}, {95, 155}}};
    std::vector<Point> triangleTrace;
    for (size_t edge = 0; edge < triangleCorners.size(); ++edge) {
        const auto a = triangleCorners[edge];
        const auto b = triangleCorners[(edge + 1) % triangleCorners.size()];
        for (int step = 0; step < 12; ++step) {
            const double t = step / 12.0;
            triangleTrace.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)});
        }
    }
    triangleTrace.push_back(triangleCorners.front());
    const auto recognizedTriangle = recognizeShape(triangleTrace);
    check(recognizedTriangle && recognizedTriangle->size() == 4,
          "Shape recognizer should simplify a closed three-corner trace");
    check(!recognizeShape({{0, 0}, {10, 10}, {20, 20}}),
          "Shape recognizer should preserve its 40 point minimum-size default");

    const auto smooth = splinePoints({{0, 0}, {20, 30}, {40, 0}});
    check(smooth.size() == 17 && smooth.front().x == 0 && smooth.back().x == 40 && smooth[4].y > 0,
          "Spline preview should interpolate the input knots and preserve endpoints");
}

static void inputAndStabilization() {
    CanvasState state;
    CanvasInput input(state);
    state.drawingMode = DrawingMode::Rectangle;
    input.begin(Device::Mouse, {20, 20, std::nullopt});
    input.setModifiers(true, false, false);
    input.motion(Device::Mouse, {60, 40, std::nullopt});
    input.finish(Device::Mouse);
    check(state.ink.size() == 1 && state.ink.back().points.size() == 5 &&
                  std::abs(std::abs(state.ink.back().points[2].x - state.ink.back().points[0].x) - 40) < 1e-6 &&
                  std::abs(std::abs(state.ink.back().points[2].y - state.ink.back().points[0].y) - 40) < 1e-6,
          "live Shift changes should constrain and commit one undoable rectangle");
    state.undo();
    check(state.ink.empty(), "geometric strokes should use normal canvas undo history");
    input.setModifiers(false, false, false);

    state.drawingMode = DrawingMode::ShapeRecognizer;
    input.begin(Device::Mouse, {160, 100, std::nullopt});
    for (int i = 1; i <= 48; ++i) {
        const double angle = 2.0 * M_PI * i / 48.0;
        input.motion(Device::Mouse, {100 + 60 * std::cos(angle), 100 + 60 * std::sin(angle), std::nullopt});
    }
    input.finish(Device::Mouse);
    check(state.ink.size() == 1 && state.ink.back().points.size() > 20,
          "recognizer should replace raw input with a smooth closed shape at stroke finish");

    state.drawingMode = DrawingMode::Freehand;
    StabilizerConfig config;
    config.averaging = AveragingMethod::Arithmetic;
    config.bufferSize = 3;
    input.setStabilizerConfig(config);
    input.begin(Device::Mouse, {20, 220, std::nullopt, 10});
    input.motion(Device::Mouse, {50, 220, std::nullopt, 20});
    check(state.current && std::abs(state.current->points.back().x - 30.0) < 1e-6,
          "arithmetic stabilization should average a three-event trailing buffer");
    input.finish(Device::Mouse);
    check(std::abs(state.ink.back().points.back().x - 50.0) < 1e-6,
          "stabilizer finalization should smoothly close the gap to the last delivered sample");

    CanvasStabilizer inertia({AveragingMethod::None, StrokePreprocessor::Inertia});
    inertia.begin({0, 0, 1, 1});
    const auto moved = inertia.process({50, 0, 1, 2});
    check(moved.x > 0 && moved.x < 50 && std::isfinite(moved.x),
          "inertia preprocessor should move smoothly toward the latest input");
    CanvasStabilizer gaussian({AveragingMethod::VelocityGaussian, StrokePreprocessor::None});
    gaussian.begin({0, 0, 1, 1});
    const auto weighted = gaussian.process({10, 0, 1, 2});
    check(weighted.x >= 0 && weighted.x <= 10 && std::isfinite(weighted.x),
          "velocity Gaussian averaging should produce a finite point within the sampled path");

    CanvasState splineState;
    CanvasInput splineInput(splineState);
    splineState.drawingMode = DrawingMode::Spline;
    check(splineInput.splinePress(Device::Mouse, {60, 60, std::nullopt}), "First spline knot did not start a path");
    splineInput.finish(Device::Mouse);
    check(splineInput.splineInProgress && splineState.current && splineState.ink.empty(),
          "Spline click should stay editable until explicit completion");
    check(splineInput.splinePress(Device::Mouse, {140, 80, std::nullopt}), "Second spline knot was rejected");
    splineInput.motion(Device::Mouse, {145, 112, std::nullopt});
    splineInput.finish(Device::Mouse);
    check(splineState.current && splineState.current->points.size() > 2,
          "Spline drag should define a tangent and update the live curve");
    splineInput.splineHover(150, 140);
    splineInput.finalizeSpline();
    check(splineState.ink.size() == 1 && splineState.ink.front().points.size() >= 13 &&
                  !splineInput.splineInProgress,
          "Escape-style spline completion should commit one interpolated stroke");
}

int main() {
    try {
        geometry();
        inputAndStabilization();
        std::cout << "PASS: geometric tools, shape recognition, spline interpolation, and stroke stabilization\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
