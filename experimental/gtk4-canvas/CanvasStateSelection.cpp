#include "CanvasState.h"

namespace prototype {

Point CanvasState::lerpPoint(const Point& a, const Point& b, double t) {
    return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.pressure + t * (b.pressure - a.pressure)};
}

std::array<Point, 4> CanvasState::textCorners(const TextItem& item) {
    const auto transform = [&](double x, double y) {
        return Point{item.xx * x + item.xy * y + item.x, item.yx * x + item.yy * y + item.y};
    };
    return {transform(0, 0), transform(item.layoutWidth, 0), transform(item.layoutWidth, item.layoutHeight),
            transform(0, item.layoutHeight)};
}

std::optional<SelectionBox> CanvasState::inkBounds(const Ink& stroke) {
    if (stroke.points.empty() || !std::isfinite(stroke.brush.width) || stroke.brush.width <= 0) {
        return std::nullopt;
    }
    double x0 = stroke.points.front().x;
    double x1 = x0;
    double y0 = stroke.points.front().y;
    double y1 = y0;
    double maxPressure = stroke.pressureSensitive ? std::max(0.01, stroke.points.front().pressure) : 1.0;
    for (const auto& point: stroke.points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.pressure)) {
            return std::nullopt;
        }
        x0 = std::min(x0, point.x);
        x1 = std::max(x1, point.x);
        y0 = std::min(y0, point.y);
        y1 = std::max(y1, point.y);
        if (stroke.pressureSensitive) {
            maxPressure = std::max(maxPressure, point.pressure);
        }
    }
    const double radius = stroke.brush.width * maxPressure / 2.0;
    return SelectionBox{stroke.page, x0 - radius, y0 - radius, x1 + radius, y1 + radius};
}

double CanvasState::snapVertical(int pageIndex, double y) const {
    if (!std::isfinite(y) || !std::isfinite(snapGridSize) || !std::isfinite(snapGridTolerance) || !snapGridEnabled ||
        snapGridSize <= 0 || snapGridTolerance <= 0) {
        return y;
    }
    const double offset = layersForPage(pageIndex).gridYOffset;
    const double snapped = y - std::remainder(y - offset, snapGridSize);
    return std::abs(snapped - y) < snapGridTolerance * snapGridSize / 2.0 ? snapped : y;
}

Point CanvasState::closestSelectionCorner(const std::array<Point, 4>& corners, Point point) {
    return *std::min_element(corners.begin(), corners.end(), [&](const Point& a, const Point& b) {
        return std::hypot(a.x - point.x, a.y - point.y) < std::hypot(b.x - point.x, b.y - point.y);
    });
}

Point CanvasState::snapSelectionMove(int pageIndex, Point point, bool altHeld) const {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(snapGridSize) ||
        !std::isfinite(snapGridTolerance) || snapGridSize <= 0 || snapGridTolerance <= 0 ||
        altHeld == snapGridEnabled) {
        return point;
    }
    const auto& pageGrid = layersForPage(pageIndex);
    const double stepX = pageGrid.gridXStep > 0 ? pageGrid.gridXStep : snapGridSize;
    const double stepY = pageGrid.gridYStep > 0 ? pageGrid.gridYStep : snapGridSize;
    if (!std::isfinite(stepX) || !std::isfinite(stepY) || stepX <= 0 || stepY <= 0) {
        return point;
    }
    const Point snapped{point.x - std::remainder(point.x - pageGrid.gridXOffset, stepX),
                        point.y - std::remainder(point.y - pageGrid.gridYOffset, stepY), point.pressure};
    const double threshold = 0.5 * std::hypot(stepX, stepY) * snapGridTolerance;
    return std::hypot(snapped.x - point.x, snapped.y - point.y) < threshold ? snapped : point;
}

double CanvasState::snapSelectionResizeAxis(int pageIndex, double value, bool horizontal, bool altHeld) const {
    if (!std::isfinite(value) || !std::isfinite(snapGridSize) || !std::isfinite(snapGridTolerance) ||
        snapGridSize <= 0 || snapGridTolerance <= 0 || altHeld == snapGridEnabled) {
        return value;
    }
    const auto& pageGrid = layersForPage(pageIndex);
    const auto offset = horizontal ? pageGrid.gridXOffset : pageGrid.gridYOffset;
    const double snapped = value - std::remainder(value - offset, snapGridSize);
    return std::abs(snapped - value) < snapGridTolerance * snapGridSize / 2.0 ? snapped : value;
}

void CanvasState::collectVerticalSpaceObjects(VerticalSpacePreview& preview) const {
    preview.inkIndices.clear();
    preview.textIndices.clear();
    preview.imageIndices.clear();
    const auto belongsToSide = [&](const SelectionBox& bounds) {
        return preview.moveAbove ? bounds.y1 <= preview.startY : bounds.y0 >= preview.startY;
    };
    for (size_t index = 0; index < ink.size(); ++index) {
        const auto& stroke = ink[index];
        if (stroke.page == preview.page && stroke.layerId == preview.layerId) {
            const auto bounds = inkBounds(stroke);
            if (bounds && belongsToSide(*bounds)) {
                preview.inkIndices.push_back(index);
            }
        }
    }
    for (size_t index = 0; index < texts.size(); ++index) {
        const auto& text = texts[index];
        if (text.page != preview.page || text.layerId != preview.layerId || text.text.empty() || !validTextPage(text)) {
            continue;
        }
        const auto corners = textCorners(text);
        const auto bounds = std::minmax_element(corners.begin(), corners.end(),
                                                [](const Point& a, const Point& b) { return a.y < b.y; });
        if ((preview.moveAbove && bounds.second->y <= preview.startY) ||
            (!preview.moveAbove && bounds.first->y >= preview.startY)) {
            preview.textIndices.push_back(index);
        }
    }
    for (size_t index = 0; index < images.size(); ++index) {
        const auto& image = images[index];
        if (image.page != preview.page || image.layerId != preview.layerId || !validCanvasImage(image)) {
            continue;
        }
        const auto bounds = canvasImageBounds(image);
        if ((preview.moveAbove && bounds.y1 <= preview.startY) || (!preview.moveAbove && bounds.y0 >= preview.startY)) {
            preview.imageIndices.push_back(index);
        }
    }
}

double CanvasState::strokeDistance(const Ink& stroke, Point point) {
    if (stroke.points.size() < 2 || !std::isfinite(stroke.brush.width) || stroke.brush.width <= 0 ||
        stroke.capStyle < 0 || stroke.capStyle > 2 || !std::isfinite(point.x) || !std::isfinite(point.y)) {
        return std::numeric_limits<double>::infinity();
    }
    size_t first = 0;
    size_t last = stroke.points.size() - 1;
    const auto& points = stroke.points;
    while (first < last && points[first].x == points[first + 1].x && points[first].y == points[first + 1].y) {
        ++first;
    }
    while (last > first && points[last].x == points[last - 1].x && points[last].y == points[last - 1].y) {
        --last;
    }

    double distance = std::numeric_limits<double>::max();
    for (size_t i = 1; i < stroke.points.size(); ++i) {
        const auto& start = points[i - 1];
        const auto& end = points[i];
        const double dx = end.x - start.x;
        const double dy = end.y - start.y;
        const double length = std::hypot(dx, dy);
        if (!std::isfinite(length) || !std::isfinite(start.x) || !std::isfinite(start.y) || !std::isfinite(end.x) ||
            !std::isfinite(end.y) || !std::isfinite(start.pressure)) {
            return std::numeric_limits<double>::infinity();
        }
        if (length == 0) {
            if (stroke.pressureSensitive || stroke.capStyle == 0) {
                const double width =
                        stroke.brush.width * (stroke.pressureSensitive ? std::max(0.0, start.pressure) : 1.0);
                distance = std::min(distance,
                                    std::max(std::hypot(point.x - start.x, point.y - start.y) - width / 2.0, 0.0));
            }
            continue;
        }

        const double along = ((point.x - start.x) * dx + (point.y - start.y) * dy) / length;
        if (!stroke.pressureSensitive && stroke.capStyle != 0) {
            const double halfWidth = stroke.brush.width / 2.0;
            const bool atStart = i - 1 == first;
            const bool atEnd = i == last;
            const bool square = stroke.capStyle == 2;
            const double capStart = atStart && square ? -halfWidth : 0.0;
            const double capEnd = length + (atEnd && square ? halfWidth : 0.0);
            const double across = std::abs((point.x - start.x) * dy - (point.y - start.y) * dx) / length;
            double current =
                    std::hypot(std::max({capStart - along, along - capEnd, 0.0}), std::max(across - halfWidth, 0.0));
            if (!atStart) {
                current =
                        std::min(current, std::max(std::hypot(point.x - start.x, point.y - start.y) - halfWidth, 0.0));
            }
            if (!atEnd) {
                current = std::min(current, std::max(std::hypot(point.x - end.x, point.y - end.y) - halfWidth, 0.0));
            }
            distance = std::min(distance, current);
            continue;
        }

        const double factor = std::clamp(along / length, 0.0, 1.0);
        const double width = stroke.brush.width * (stroke.pressureSensitive ? std::max(0.0, start.pressure) : 1.0);
        const double projectedX = start.x + factor * dx;
        const double projectedY = start.y + factor * dy;
        distance = std::clamp(std::hypot(point.x - projectedX, point.y - projectedY) - width / 2.0, 0.0, distance);
    }
    return distance;
}

double CanvasState::quadrilateralDistance(const std::array<Point, 4>& corners, Point point) {
    const auto& origin = corners[0];
    const auto& horizontal = corners[1];
    const auto& vertical = corners[3];
    const double hx = horizontal.x - origin.x;
    const double hy = horizontal.y - origin.y;
    const double vx = vertical.x - origin.x;
    const double vy = vertical.y - origin.y;
    const double determinant = hx * vy - vx * hy;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-12) {
        return std::numeric_limits<double>::infinity();
    }
    const double dx = point.x - origin.x;
    const double dy = point.y - origin.y;
    const double localX = (dx * vy - vx * dy) / determinant;
    const double localY = (hx * dy - dx * hy) / determinant;
    const double projectedX = origin.x + std::clamp(localX, 0.0, 1.0) * hx + std::clamp(localY, 0.0, 1.0) * vx;
    const double projectedY = origin.y + std::clamp(localX, 0.0, 1.0) * hy + std::clamp(localY, 0.0, 1.0) * vy;
    return std::hypot(point.x - projectedX, point.y - projectedY);
}

TextItem CanvasState::transformText(TextItem item, const SelectionTransform& transform) {
    const double xx = transform.xx * item.xx + transform.xy * item.yx;
    const double yx = transform.yx * item.xx + transform.yy * item.yx;
    const double xy = transform.xx * item.xy + transform.xy * item.yy;
    const double yy = transform.yx * item.xy + transform.yy * item.yy;
    const double x = transform.xx * item.x + transform.xy * item.y + transform.x0;
    const double y = transform.yx * item.x + transform.yy * item.y + transform.y0;
    item.xx = xx;
    item.yx = yx;
    item.xy = xy;
    item.yy = yy;
    item.x = x;
    item.y = y;
    return item;
}

CanvasImage CanvasState::transformImage(CanvasImage image, const SelectionTransform& transform) {
    const auto source = image.transform;
    image.transform = {transform.xx * source.xx + transform.xy * source.yx,
                       transform.yx * source.xx + transform.yy * source.yx,
                       transform.xx * source.xy + transform.xy * source.yy,
                       transform.yx * source.xy + transform.yy * source.yy,
                       transform.xx * source.x + transform.xy * source.y + transform.x0,
                       transform.yx * source.x + transform.yy * source.y + transform.y0};
    return image;
}

Point CanvasState::pointAt(const Ink& stroke, PathParam param) {
    return lerpPoint(stroke.points[param.index], stroke.points[param.index + 1], param.t);
}

std::optional<CanvasState::Interval> CanvasState::intersectLineWithBox(const Point& p, const Point& q, const Box& box) {
    auto strip = [](double a1, double a2, double stripMin, double stripMax) -> Interval {
        const double norm = 1.0 / (a2 - a1);
        const double t1 = (stripMin - a1) * norm;
        const double t2 = (stripMax - a1) * norm;
        return t1 < t2 ? Interval{t1, t2} : Interval{t2, t1};
    };
    if (p.x == q.x) {
        if (p.y == q.y) {
            return std::nullopt;
        }
        if (box.minX < p.x && p.x < box.maxX) {
            return strip(p.y, q.y, box.minY, box.maxY);
        }
        return std::nullopt;
    }
    if (p.y == q.y) {
        if (box.minY < p.y && p.y < box.maxY) {
            return strip(p.x, q.x, box.minX, box.maxX);
        }
        return std::nullopt;
    }
    const auto vertical = strip(p.y, q.y, box.minY, box.maxY);
    const auto horizontal = strip(p.x, q.x, box.minX, box.maxX);
    const double lo = std::max(vertical.min, horizontal.min);
    const double hi = std::min(vertical.max, horizontal.max);
    return lo < hi ? std::optional<Interval>{Interval{lo, hi}} : std::nullopt;
}

std::vector<double> CanvasState::intersectSegmentWithBox(const Point& p, const Point& q, const Box& box) {
    std::vector<double> hits;
    if (const auto interval = intersectLineWithBox(p, q, box)) {
        if (interval->min > 0.0 && interval->min <= 1.0) {
            hits.push_back(interval->min);
        }
        if (interval->max > 0.0 && interval->max <= 1.0) {
            hits.push_back(interval->max);
        }
    }
    return hits;
}

bool CanvasState::allPointsCoincident(const std::vector<Point>& pts) {
    for (size_t i = 1; i < pts.size(); ++i) {
        if (pts[i].x != pts[0].x || pts[i].y != pts[0].y) {
            return false;
        }
    }
    return true;
}

bool CanvasState::intersectsSquare(const Ink& stroke, double cx, double cy, double halfSize) {
    if (stroke.points.empty()) {
        return false;
    }
    // Intentional improvement over legacy Stroke::intersects: use exact segment-vs-box
    // clipping at all segment lengths and include half the painted stroke width so clicking
    // the visible body of a thick stroke with a fine Delete-Stroke eraser hits reliably.
    const double reach = halfSize + 0.5 * stroke.brush.width;
    const Box square{cx - reach, cy - reach, cx + reach, cy + reach};
    if (square.contains(stroke.points.front())) {
        return true;
    }
    for (size_t i = 1; i < stroke.points.size(); ++i) {
        const auto& a = stroke.points[i - 1];
        const auto& b = stroke.points[i];
        if (square.contains(b) || !intersectSegmentWithBox(a, b, square).empty()) {
            return true;
        }
    }
    return false;
}

CanvasState::SplitResult CanvasState::eraseWithPaddedBox(const Ink& stroke, double cx, double cy, double halfSize) {
    if (stroke.points.empty()) {
        return {};
    }
    const Box inner{cx - halfSize, cy - halfSize, cx + halfSize, cy + halfSize};
    if (stroke.points.size() == 1 || allPointsCoincident(stroke.points)) {
        if (inner.contains(stroke.points.front())) {
            return {true, {}};
        }
        return {};
    }

    const double outerHalf = halfSize + behavior::eraserPaddingCapRound * stroke.brush.width;
    const Box outer{cx - outerHalf, cy - outerHalf, cx + outerHalf, cy + outerHalf};
    const size_t segCount = stroke.points.size() - 1;

    struct Flags {
        bool isInsideOuter = false;
        bool wentInsideInner = false;
        bool lastEndedOnBoundary = false;
    } flags;

    {
        const auto& firstKnot = stroke.points[0];
        const auto& nextKnot = stroke.points[1];
        if (inner.contains(firstKnot)) {
            flags = {true, true, false};
        } else if (outer.contains(firstKnot)) {
            const auto innerLine = intersectLineWithBox(firstKnot, nextKnot, inner);
            flags = {true, innerLine && innerLine->max <= 0.0, false};
        }
    }

    std::vector<PathParam> removed;
    if (flags.isInsideOuter) {
        removed.push_back({0, 0.0});
    }

    for (size_t idx = 0; idx < segCount; ++idx) {
        const auto& a = stroke.points[idx];
        const auto& b = stroke.points[idx + 1];
        const auto outerHits = intersectSegmentWithBox(a, b, outer);
        if (!outerHits.empty() || outer.contains(a) || outer.contains(b)) {
            const auto innerHits = intersectSegmentWithBox(a, b, inner);
            size_t innerPos = 0;
            auto skipInnerBelow = [&](double limit) {
                bool skipped = false;
                while (innerPos < innerHits.size() && innerHits[innerPos] < limit) {
                    skipped = true;
                    ++innerPos;
                }
                return skipped;
            };

            if (flags.lastEndedOnBoundary) {
                flags.lastEndedOnBoundary = false;
                const Point probe = outerHits.empty() ? b : lerpPoint(a, b, 0.5 * outerHits.front());
                if (outer.contains(probe) != (removed.size() % 2 != 0)) {
                    if (!removed.empty()) {
                        removed.pop_back();
                    }
                }
            }

            for (double outerHit: outerHits) {
                flags.wentInsideInner |= skipInnerBelow(outerHit);
                if (!flags.isInsideOuter || flags.wentInsideInner) {
                    removed.push_back({idx, outerHit});
                    if (outerHit == 1.0) {
                        flags.lastEndedOnBoundary = true;
                    }
                } else if (!removed.empty()) {
                    removed.pop_back();
                }
                flags.wentInsideInner = false;
                flags.isInsideOuter = !flags.isInsideOuter;
            }
            if (innerPos < innerHits.size()) {
                flags.wentInsideInner = true;
            }
        }
    }

    if (removed.size() % 2 != 0) {
        const auto& lastPoint = stroke.points.back();
        const auto& prevPoint = stroke.points[segCount - 1];
        if (outer.contains(lastPoint)) {
            const auto innerLine = intersectLineWithBox(lastPoint, prevPoint, inner);
            if (flags.wentInsideInner || (innerLine && innerLine->max < 0.0)) {
                removed.push_back({segCount - 1, 1.0});
            } else {
                removed.pop_back();
            }
        } else {
            return {};
        }
    }

    if (removed.empty()) {
        return {};
    }

    for (size_t i = 0; i + 1 < removed.size(); i += 2) {
        const auto& start = removed[i];
        const auto& end = removed[i + 1];
        const Point mid = start.index == end.index ? pointAt(stroke, {start.index, 0.5 * (start.t + end.t)}) :
                                                     stroke.points[(start.index + end.index + 1) / 2];
        if (!outer.contains(mid)) {
            return {};
        }
    }

    // Complement `removed` inside [{0, 0.0}, {segCount - 1, 1.0}].
    const PathParam domainStart{0, 0.0};
    const PathParam domainEnd{segCount - 1, 1.0};
    std::vector<PathParam> kept = removed;
    if (kept.front() == domainStart) {
        kept.erase(kept.begin());
    } else {
        kept.insert(kept.begin(), domainStart);
    }
    if (!kept.empty() && kept.back() == domainEnd) {
        kept.pop_back();
    } else {
        kept.push_back(domainEnd);
    }

    struct Section {
        PathParam min;
        PathParam max;
    };
    std::vector<Section> sections;
    sections.reserve(kept.size() / 2);
    for (size_t i = 0; i + 1 < kept.size(); i += 2) {
        if (kept[i] < kept[i + 1]) {
            sections.push_back({kept[i], kept[i + 1]});
        }
    }

    auto cloneLinear = [&](PathParam lower, PathParam upper) -> std::optional<Ink> {
        std::vector<Point> pts;
        pts.reserve(upper.index - lower.index + 2);
        pts.push_back(pointAt(stroke, lower));
        for (size_t i = lower.index + 1; i <= upper.index; ++i) {
            if (pts.back().x != stroke.points[i].x || pts.back().y != stroke.points[i].y) {
                pts.push_back(stroke.points[i]);
            }
        }
        const Point endPt = pointAt(stroke, upper);
        if (pts.back().x != endPt.x || pts.back().y != endPt.y) {
            pts.push_back(endPt);
        }
        if (pts.size() < 2) {
            return std::nullopt;
        }
        Ink piece{stroke.page, std::move(pts), stroke.brush, stroke.pressureSensitive, stroke.layerOrder};
        piece.layerId = stroke.layerId;
        piece.originLayer = stroke.originLayer;
        return piece;
    };

    const bool closedStroke =
            stroke.points.size() >= 3 && std::hypot(stroke.points.front().x - stroke.points.back().x,
                                                    stroke.points.front().y - stroke.points.back().y) < 0.01;
    const bool mergeClosedEnds = closedStroke && sections.size() >= 2 && sections.front().min == domainStart &&
                                 sections.back().max == domainEnd;

    SplitResult out{true, {}};
    size_t firstSec = 0;
    size_t endSec = sections.size();
    if (mergeClosedEnds) {
        std::vector<Point> pts;
        const auto tailStart = sections.back().min;
        const auto headEnd = sections.front().max;
        pts.push_back(pointAt(stroke, tailStart));
        for (size_t i = tailStart.index + 1; i + 1 < stroke.points.size(); ++i) {
            if (pts.back().x != stroke.points[i].x || pts.back().y != stroke.points[i].y) {
                pts.push_back(stroke.points[i]);
            }
        }
        for (size_t i = 0; i <= headEnd.index; ++i) {
            if (pts.back().x != stroke.points[i].x || pts.back().y != stroke.points[i].y) {
                pts.push_back(stroke.points[i]);
            }
        }
        const Point endPt = pointAt(stroke, headEnd);
        if (pts.back().x != endPt.x || pts.back().y != endPt.y) {
            pts.push_back(endPt);
        }
        if (pts.size() >= 2) {
            Ink piece{stroke.page, std::move(pts), stroke.brush, stroke.pressureSensitive, stroke.layerOrder};
            piece.layerId = stroke.layerId;
            piece.originLayer = stroke.originLayer;
            out.remaining.push_back(std::move(piece));
        }
        ++firstSec;
        --endSec;
    }
    for (size_t i = firstSec; i < endSec; ++i) {
        if (auto piece = cloneLinear(sections[i].min, sections[i].max)) {
            out.remaining.push_back(std::move(*piece));
        }
    }
    return out;
}

void CanvasState::eraseStep(int targetPage, const Point& p) {
    if (!eraseGesture) {
        return;
    }
    const double halfSize = std::max(0.05, activeEraserWidth());
    std::vector<Ink> nextInk;
    std::vector<size_t> nextOrigins;
    bool anyChanged = false;
    auto flushUntouchedPrefix = [&](size_t upTo) {
        if (!anyChanged) {
            anyChanged = true;
            nextInk.reserve(ink.size() + 1);
            nextOrigins.reserve(ink.size() + 1);
            for (size_t k = 0; k < upTo; ++k) {
                nextInk.push_back(std::move(ink[k]));
                nextOrigins.push_back(eraseGesture->slotOrigin[k]);
            }
        }
    };
    for (size_t i = 0; i < ink.size(); ++i) {
        const size_t origin = eraseGesture->slotOrigin[i];
        if (ink[i].page != targetPage || ink[i].layerId != activeLayerId(targetPage) ||
            !layerVisible(targetPage, ink[i].layerId)) {
            if (anyChanged) {
                nextInk.push_back(std::move(ink[i]));
                nextOrigins.push_back(origin);
            }
            continue;
        }
        if (eraserMode == EraserMode::DeleteStroke) {
            if (intersectsSquare(ink[i], p.x, p.y, halfSize)) {
                flushUntouchedPrefix(i);
                eraseGesture->touchedOriginals.try_emplace(origin, std::move(ink[i]));
                continue;
            }
            if (anyChanged) {
                nextInk.push_back(std::move(ink[i]));
                nextOrigins.push_back(origin);
            }
        } else {
            auto split = eraseWithPaddedBox(ink[i], p.x, p.y, halfSize);
            if (!split.changed) {
                if (anyChanged) {
                    nextInk.push_back(std::move(ink[i]));
                    nextOrigins.push_back(origin);
                }
                continue;
            }
            flushUntouchedPrefix(i);
            eraseGesture->touchedOriginals.try_emplace(origin, std::move(ink[i]));
            for (auto& piece: split.remaining) {
                nextInk.push_back(std::move(piece));
                nextOrigins.push_back(origin);
            }
        }
    }
    if (anyChanged) {
        ink = std::move(nextInk);
        eraseGesture->slotOrigin = std::move(nextOrigins);
    }
}

}  // namespace prototype
