#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace prototype {

struct Point {
    double x;
    double y;
    double pressure = 1;
    bool operator==(const Point&) const = default;
};

enum class DrawingMode { Freehand, Line, Rectangle, Ellipse, Arrow, DoubleArrow, CoordinateSystem, ShapeRecognizer, Spline };

inline std::vector<Point> linePoints(Point start, Point end, bool centered, bool constrained) {
    if (constrained) {
        const double angle = std::round(std::atan2(end.y - start.y, end.x - start.x) / (M_PI / 4.0)) * (M_PI / 4.0);
        const double length = std::hypot(end.x - start.x, end.y - start.y);
        end = {start.x + length * std::cos(angle), start.y + length * std::sin(angle)};
    }
    if (centered) {
        start = {2 * start.x - end.x, 2 * start.y - end.y};
    }
    return {start, end};
}

inline std::vector<Point> rectanglePoints(Point start, Point end, bool centered, bool square) {
    double width = end.x - start.x;
    double height = end.y - start.y;
    if (square) {
        const double signX = width < 0 ? -1.0 : 1.0;
        const double signY = height < 0 ? -1.0 : 1.0;
        const double side = std::max(std::abs(width), std::abs(height));
        width = signX * side;
        height = signY * side;
    }
    const Point p1 = centered ? Point{start.x - width, start.y - height} : start;
    const Point p2{start.x + width, start.y + height};
    return {p1, {p1.x, p2.y}, p2, {p2.x, p1.y}, p1};
}

inline std::vector<Point> ellipsePoints(Point start, Point end, bool centered, bool circle) {
    double width = end.x - start.x;
    double height = end.y - start.y;
    if (circle) {
        width = std::copysign(std::max(std::abs(width), std::abs(height)), width == 0 ? 1.0 : width);
        height = std::copysign(std::abs(width), height == 0 ? 1.0 : height);
    }
    const double rx = centered ? width : width / 2.0;
    const double ry = centered ? height : height / 2.0;
    const double cx = centered ? start.x : start.x + rx;
    const double cy = centered ? start.y : start.y + ry;
    const auto quadrant = static_cast<size_t>(std::clamp(std::ceil(5 + 0.3 * (std::abs(rx) + std::abs(ry))), 5.0, 128.0));
    const size_t count = quadrant * 4;
    std::vector<Point> points;
    points.reserve(count + 1);
    for (size_t i = 0; i <= count; ++i) {
        const double angle = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(count);
        points.push_back({cx + rx * std::cos(angle), cy + ry * std::sin(angle)});
    }
    return points;
}

inline std::vector<Point> arrowPoints(Point start, Point end, double width, bool centered, bool constrained,
                                      bool doubleEnded) {
    auto endpoints = linePoints(start, end, centered, constrained);
    start = endpoints.front();
    end = endpoints.back();
    const double length = std::hypot(end.x - start.x, end.y - start.y);
    if (length < 1e-8) return {};
    const double slimness = length / std::max(width, 0.01);
    double delta = M_PI / 6.0;
    constexpr double thick1 = 7.0;
    constexpr double thick3 = 1.6;
    constexpr double length2 = 0.4;
    const double length4 = doubleEnded ? 0.5 : 0.8;
    double arrowDistance = width * thick1;
    if (slimness >= thick1 / length2) {
        // Full-size arrow head.
    } else if (slimness >= thick3 / length2) {
        arrowDistance = length * length2;
    } else if (slimness >= thick3 / length4) {
        arrowDistance = width * thick3;
        delta = (1.0 + (slimness - thick3 / length2) / (thick3 / length4 - thick3 / length2)) * M_PI / 6.0;
        arrowDistance *= std::sin(M_PI / 6.0) / std::sin(delta);
    } else {
        arrowDistance = length * length4 * std::sin(M_PI / 6.0) / std::sin(M_PI / 3.0);
        delta = M_PI / 3.0;
    }
    const double angle = std::atan2(end.y - start.y, end.x - start.x);
    std::vector<Point> points;
    points.reserve(doubleEnded ? 10 : 6);
    points.push_back(start);
    if (doubleEnded) {
        points.push_back({start.x + arrowDistance * std::cos(angle + delta), start.y + arrowDistance * std::sin(angle + delta)});
        points.push_back(start);
        points.push_back({start.x + arrowDistance * std::cos(angle - delta), start.y + arrowDistance * std::sin(angle - delta)});
        points.push_back(start);
    }
    points.push_back(end);
    points.push_back({end.x - arrowDistance * std::cos(angle + delta), end.y - arrowDistance * std::sin(angle + delta)});
    points.push_back(end);
    points.push_back({end.x - arrowDistance * std::cos(angle - delta), end.y - arrowDistance * std::sin(angle - delta)});
    points.push_back(end);
    return points;
}

inline std::vector<Point> coordinateSystemPoints(Point start, Point end, bool square, bool reverse) {
    double width = end.x - start.x;
    double height = end.y - start.y;
    if (square) {
        const double signX = width < 0 ? -1.0 : 1.0;
        const double signY = height < 0 ? -1.0 : 1.0;
        const double side = std::max(std::abs(width), std::abs(height));
        width = signX * side;
        height = signY * side;
    }
    if (reverse) return {{start.x, start.y + height}, start, {start.x + width, start.y}};
    return {start, {start.x, start.y + height}, {start.x + width, start.y + height}};
}

inline std::vector<Point> splinePoints(const std::vector<Point>& knots) {
    if (knots.size() < 3) return knots;
    std::vector<Point> out;
    out.reserve((knots.size() - 1) * 8 + 1);
    for (size_t i = 0; i + 1 < knots.size(); ++i) {
        const auto& p0 = knots[i ? i - 1 : i];
        const auto& p1 = knots[i];
        const auto& p2 = knots[i + 1];
        const auto& p3 = knots[std::min(i + 2, knots.size() - 1)];
        for (size_t j = 0; j < 8; ++j) {
            const double t = static_cast<double>(j) / 8.0;
            const double t2 = t * t;
            const double t3 = t2 * t;
            const auto catmull = [&](double a, double b, double c, double d) {
                return 0.5 * ((2 * b) + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 +
                              (-a + 3 * b - 3 * c + d) * t3);
            };
            const double pressure = p1.pressure + (p2.pressure - p1.pressure) * t;
            out.push_back({catmull(p0.x, p1.x, p2.x, p3.x), catmull(p0.y, p1.y, p2.y, p3.y), pressure});
        }
    }
    out.push_back(knots.back());
    return out;
}

inline std::vector<Point> splineBezierPoints(const std::vector<Point>& knots, const std::vector<Point>& tangents,
                                             std::optional<Point> previewEnd = std::nullopt) {
    if (knots.empty()) return {};
    std::vector<Point> points;
    if (knots.size() == 1 && !previewEnd) return {knots.front(), knots.front()};
    const size_t segmentCount = knots.size() > 1 ? knots.size() - 1 : 0;
    const bool previewSegment = previewEnd &&
            (knots.size() == 1 || std::hypot(previewEnd->x - knots.back().x, previewEnd->y - knots.back().y) > 1e-8);
    const size_t totalSegments = segmentCount + (previewSegment ? 1 : 0);
    points.reserve(totalSegments * 12 + 1);
    const auto tangentAt = [&](size_t index) {
        return index < tangents.size() ? tangents[index] : Point{0, 0};
    };
    for (size_t i = 0; i < totalSegments; ++i) {
        const bool isPreview = i >= segmentCount;
        const Point p0 = knots[isPreview ? knots.size() - 1 : i];
        const Point p3 = isPreview ? *previewEnd : knots[i + 1];
        const Point t0 = tangentAt(isPreview ? knots.size() - 1 : i);
        const Point t1 = isPreview ? Point{0, 0} : tangentAt(i + 1);
        const Point p1{p0.x + t0.x, p0.y + t0.y};
        const Point p2{p3.x - t1.x, p3.y - t1.y};
        for (size_t j = 0; j < 12; ++j) {
            const double t = static_cast<double>(j) / 12.0;
            const double u = 1.0 - t;
            const double pressure = p0.pressure + (p3.pressure - p0.pressure) * t;
            points.push_back({u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x,
                              u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y,
                              pressure});
        }
    }
    if (totalSegments) {
        const Point end = previewSegment ? *previewEnd : knots.back();
        points.push_back(end);
    }
    return points;
}

inline std::optional<std::vector<Point>> recognizeShape(const std::vector<Point>& input, double minSize = 40.0) {
    if (input.size() < 3) return std::nullopt;
    double minX = input.front().x, maxX = minX, minY = input.front().y, maxY = minY;
    double length = 0;
    for (size_t i = 0; i < input.size(); ++i) {
        const auto& p = input[i];
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
        if (i) length += std::hypot(p.x - input[i - 1].x, p.y - input[i - 1].y);
    }
    const double width = maxX - minX, height = maxY - minY;
    const double diagonal = std::hypot(width, height);
    if (diagonal < minSize || length < minSize) return std::nullopt;
    const double direct = std::hypot(input.back().x - input.front().x, input.back().y - input.front().y);
    if (direct / std::max(length, 1e-9) > 0.94) {
        Point end = input.back();
        const double angle = std::atan2(end.y - input.front().y, end.x - input.front().x);
        constexpr double snapTolerance = 5.0 * M_PI / 180.0;
        if (std::abs(angle) < snapTolerance || std::abs(std::abs(angle) - M_PI) < snapTolerance) {
            double averageY = 0;
            for (const auto& p: input) averageY += p.y;
            end.y = averageY / input.size();
            Point start = input.front();
            start.y = end.y;
            return linePoints(start, end, false, false);
        }
        if (std::abs(std::abs(angle) - M_PI / 2.0) < snapTolerance) {
            double averageX = 0;
            for (const auto& p: input) averageX += p.x;
            end.x = averageX / input.size();
            Point start = input.front();
            start.x = end.x;
            return linePoints(start, end, false, false);
        }
        return linePoints(input.front(), end, false, false);
    }

    const double closeTolerance = std::max(8.0, diagonal * 0.14);
    const bool closed = std::hypot(input.back().x - input.front().x, input.back().y - input.front().y) <= closeTolerance;
    if (!closed || width < 1e-6 || height < 1e-6) return std::nullopt;
    // Axis-aligned rectangular traces hug the bounding-box edges more closely
    // than circles do, so test this before the radial circle fit.
    double edgeError = 0;
    for (const auto& p: input) {
        const double dx = std::min(std::abs(p.x - minX), std::abs(p.x - maxX));
        const double dy = std::min(std::abs(p.y - minY), std::abs(p.y - maxY));
        edgeError += std::min(dx, dy);
    }
    if (edgeError / input.size() < diagonal * 0.02) {
        return rectanglePoints({minX, minY}, {maxX, maxY}, false, false);
    }

    const Point center{(minX + maxX) / 2.0, (minY + maxY) / 2.0};
    double meanRadius = 0;
    for (const auto& p: input) meanRadius += std::hypot(p.x - center.x, p.y - center.y);
    meanRadius /= input.size();
    double radiusVariance = 0;
    for (const auto& p: input) {
        const double error = std::hypot(p.x - center.x, p.y - center.y) - meanRadius;
        radiusVariance += error * error;
    }
    radiusVariance = std::sqrt(radiusVariance / input.size()) / std::max(meanRadius, 1e-9);
    if (radiusVariance < 0.12) {
        return ellipsePoints({minX, minY}, {maxX, maxY}, false, false);
    }

    std::vector<Point> trace = input;
    if (std::hypot(trace.front().x - trace.back().x, trace.front().y - trace.back().y) <= closeTolerance) {
        trace.pop_back();
    }
    const auto pointSegmentDistance = [](Point p, Point a, Point b) {
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double length2 = dx * dx + dy * dy;
        const double t = length2 > 1e-12 ?
                std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length2, 0.0, 1.0) : 0.0;
        return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
    };
    std::vector<Point> simplified;
    if (trace.size() >= 3) {
        const double tolerance = std::max(2.0, diagonal * 0.05);
        auto simplify = [&](auto&& self, size_t first, size_t last) -> void {
            double greatest = tolerance;
            size_t split = last;
            for (size_t i = first + 1; i < last; ++i) {
                const double distance = pointSegmentDistance(trace[i], trace[first], trace[last]);
                if (distance > greatest) {
                    greatest = distance;
                    split = i;
                }
            }
            if (split != last) {
                self(self, first, split);
                simplified.push_back(trace[split]);
                self(self, split, last);
            }
        };
        simplified.push_back(trace.front());
        simplify(simplify, 0, trace.size() - 1);
        simplified.push_back(trace.back());
    }
    while (simplified.size() > 1 &&
           std::hypot(simplified.front().x - simplified.back().x,
                      simplified.front().y - simplified.back().y) < closeTolerance * 0.5) {
        simplified.pop_back();
    }
    if (simplified.size() == 3 || simplified.size() == 4) {
        bool rectangle = simplified.size() == 4;
        if (rectangle) {
            for (size_t i = 0; i < simplified.size(); ++i) {
                const auto& p = simplified[i];
                const auto& a = simplified[(i + simplified.size() - 1) % simplified.size()];
                const auto& b = simplified[(i + 1) % simplified.size()];
                const double ax = a.x - p.x, ay = a.y - p.y;
                const double bx = b.x - p.x, by = b.y - p.y;
                const double denom = std::hypot(ax, ay) * std::hypot(bx, by);
                if (denom < 1e-8 || std::abs((ax * bx + ay * by) / denom) > 0.26) {
                    rectangle = false;
                    break;
                }
            }
        }
        if (simplified.size() == 3 || rectangle) {
            simplified.push_back(simplified.front());
            return simplified;
        }
    }
    return std::nullopt;
}

inline std::vector<Point> makeDrawingPath(DrawingMode mode, Point start, Point end, double width, bool shift,
                                         bool control, bool alt, const std::vector<Point>& raw = {}) {
    switch (mode) {
        case DrawingMode::Line: return linePoints(start, end, control || alt, shift);
        case DrawingMode::Rectangle: return rectanglePoints(start, end, control || alt, shift);
        case DrawingMode::Ellipse: return ellipsePoints(start, end, control || alt, shift);
        case DrawingMode::Arrow: return arrowPoints(start, end, width, control || alt, shift, false);
        case DrawingMode::DoubleArrow: return arrowPoints(start, end, width, control || alt, shift, true);
        case DrawingMode::CoordinateSystem: return coordinateSystemPoints(start, end, shift, control);
        case DrawingMode::Spline: return splinePoints(raw);
        case DrawingMode::Freehand:
        case DrawingMode::ShapeRecognizer: return raw;
    }
    return raw;
}

}  // namespace prototype
