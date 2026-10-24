#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "BehaviorDefaults.h"

namespace prototype {

struct Point {
    double x;
    double y;
    double pressure = 1;
};

struct PageSize {
    double width = 595;
    double height = 842;
};

struct PageHit {
    int page = 0;
    Point local{};
};

enum class Tool { Pen, Highlighter, Eraser };
enum class EraserMode { Standard, Whiteout, DeleteStroke };

inline constexpr const std::array<double, 5>& widthsForTool(Tool tool) {
    switch (tool) {
        case Tool::Highlighter:
            return behavior::highlighterWidths;
        case Tool::Eraser:
            return behavior::eraserWidths;
        case Tool::Pen:
        default:
            return behavior::penWidths;
    }
}

struct Brush {
    Tool tool = Tool::Pen;
    std::array<double, 3> color = behavior::defaultPenColor;
    double width = behavior::penWidths[static_cast<size_t>(behavior::ToolSize::Medium)];
};

struct Ink {
    int page;
    std::vector<Point> points;
    Brush brush;
    bool pressureSensitive = false;
};

enum class Device { Mouse, Stylus };

// Deliberately independent of GTK. Coordinates are PDF points, not device pixels.
class CanvasState {
public:
    double zoom = 1;
    double offsetX = 0;
    double offsetY = 0;
    double pageWidth = 595;
    double pageHeight = 842;
    double pageGap = behavior::defaultPageGap;
    int page = 0;
    std::vector<PageSize> pages;
    Brush brush;
    EraserMode eraserMode = EraserMode::Standard;
    double eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::Medium)];
    std::vector<Ink> ink;
    std::optional<Ink> current;
    bool pinching = false;
    long long scrollBlockedUntil = 0;

    int pageCount() const { return pages.empty() ? 1 : static_cast<int>(pages.size()); }

    PageSize pageSize(int index) const {
        if (pages.empty() || index < 0 || static_cast<size_t>(index) >= pages.size()) {
            return {pageWidth, pageHeight};
        }
        return pages[static_cast<size_t>(index)];
    }

    void selectActivePage(int target) {
        if (!pages.empty()) {
            page = std::clamp(target, 0, pageCount() - 1);
            pageWidth = pages[static_cast<size_t>(page)].width;
            pageHeight = pages[static_cast<size_t>(page)].height;
        } else if (target >= 0) {
            page = target;
        }
    }

    void setPages(std::vector<PageSize> nextPages) {
        cancel();
        pages = std::move(nextPages);
        if (!pages.empty()) {
            selectActivePage(std::clamp(page, 0, pageCount() - 1));
        }
    }

    double layoutWidth() const {
        if (pages.empty()) {
            return pageWidth;
        }
        double width = 1.0;
        for (const auto& entry: pages) {
            width = std::max(width, entry.width);
        }
        return width;
    }

    double layoutHeight() const {
        if (pages.empty()) {
            return pageHeight;
        }
        double height = 0.0;
        for (size_t i = 0; i < pages.size(); ++i) {
            if (i) {
                height += pageGap;
            }
            height += pages[i].height;
        }
        return height;
    }

    double pageLeft(int index) const {
        if (pages.empty()) {
            return 0.0;
        }
        return 0.5 * (layoutWidth() - pageSize(index).width);
    }

    double pageTop(int index) const {
        if (pages.empty() || index <= 0) {
            return 0.0;
        }
        const size_t limit = std::min(static_cast<size_t>(index), pages.size());
        double top = 0.0;
        for (size_t i = 0; i < limit; ++i) {
            top += pages[i].height + pageGap;
        }
        return top;
    }

    double activeEraserWidth() const { return brush.tool == Tool::Eraser ? brush.width : eraserWidth; }

    Point documentPoint(double x, double y, double pressure = 1) const {
        return {(x - offsetX) / zoom, (y - offsetY) / zoom,
                std::isfinite(pressure) ? std::max(behavior::minimumPressure, pressure) : 1.0};
    }

    Point pageLocalPoint(int pageIndex, double x, double y, double pressure = 1) const {
        const auto doc = documentPoint(x, y, pressure);
        return {doc.x - pageLeft(pageIndex), doc.y - pageTop(pageIndex), doc.pressure};
    }

    Point pageScreenPoint(int pageIndex, double localX, double localY, double pressure = 1) const {
        return {offsetX + (pageLeft(pageIndex) + localX) * zoom, offsetY + (pageTop(pageIndex) + localY) * zoom,
                pressure};
    }

    bool contains(const Point& p) const {
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0 && p.y >= 0 && p.x <= pageWidth &&
               p.y <= pageHeight;
    }

    bool containsOnPage(int pageIndex, const Point& local) const {
        const auto sz = pageSize(pageIndex);
        return std::isfinite(local.x) && std::isfinite(local.y) && local.x >= 0 && local.y >= 0 &&
               local.x <= sz.width && local.y <= sz.height;
    }

    std::optional<PageHit> hitPage(double x, double y, double pressure = 1) const {
        if (pages.empty()) {
            const auto p = documentPoint(x, y, pressure);
            if (contains(p)) {
                return PageHit{page, p};
            }
            return std::nullopt;
        }
        const auto doc = documentPoint(x, y, pressure);
        if (!std::isfinite(doc.x) || !std::isfinite(doc.y) || doc.y < 0) {
            return std::nullopt;
        }
        double top = 0.0;
        for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
            const auto& sz = pages[static_cast<size_t>(i)];
            if (doc.y <= top + sz.height) {
                const Point local{doc.x - pageLeft(i), doc.y - top, doc.pressure};
                if (local.x >= 0 && local.x <= sz.width && local.y >= 0) {
                    return PageHit{i, local};
                }
                return std::nullopt;
            }
            top += sz.height + pageGap;
        }
        return std::nullopt;
    }

    void fit(double width, double height) {
        cancel();
        if (!pages.empty()) {
            selectActivePage(std::clamp(page, 0, pageCount() - 1));
        }
        const auto sz = pageSize(page);
        zoom = std::clamp(std::min(std::max(1.0, width - 64) / sz.width, std::max(1.0, height - 48) / sz.height), 0.1,
                          8.0);
        if (pages.empty()) {
            offsetX = (width - pageWidth * zoom) / 2;
            offsetY = (height - pageHeight * zoom) / 2;
        } else {
            offsetX = (width - layoutWidth() * zoom) / 2;
            offsetY = (height - sz.height * zoom) / 2 - pageTop(page) * zoom;
        }
    }

    void scrollToPage(int targetPage, double viewportWidth, double viewportHeight) {
        if (targetPage < 0 || (!pages.empty() && targetPage >= pageCount())) {
            return;
        }
        cancel();
        selectActivePage(targetPage);
        const auto sz = pageSize(page);
        if (pages.empty()) {
            offsetX = (viewportWidth - pageWidth * zoom) / 2;
            offsetY = (viewportHeight - pageHeight * zoom) / 2;
            return;
        }
        if (sz.width * zoom + 64 <= viewportWidth) {
            offsetX = (viewportWidth - layoutWidth() * zoom) / 2;
        }
        if (sz.height * zoom + 48 <= viewportHeight) {
            offsetY = (viewportHeight - sz.height * zoom) / 2 - pageTop(page) * zoom;
        } else {
            offsetY = 24.0 - pageTop(page) * zoom;
        }
    }

    // Mirrors Layout::updateVisibility in src/core/gui/Layout.cpp: select the page with the
    // largest visible viewport intersection area while keeping the current page if none intersect.
    void syncActivePage(double viewportWidth, double viewportHeight) {
        if (pages.empty() || viewportWidth <= 0 || viewportHeight <= 0) {
            return;
        }
        int bestPage = -1;
        double bestArea = 0.0;
        for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
            const auto& sz = pages[static_cast<size_t>(i)];
            const double x0 = std::max(0.0, offsetX + pageLeft(i) * zoom);
            const double y0 = std::max(0.0, offsetY + pageTop(i) * zoom);
            const double x1 = std::min(viewportWidth, offsetX + (pageLeft(i) + sz.width) * zoom);
            const double y1 = std::min(viewportHeight, offsetY + (pageTop(i) + sz.height) * zoom);
            if (x1 > x0 && y1 > y0) {
                const double area = (x1 - x0) * (y1 - y0);
                if (area > bestArea) {
                    bestArea = area;
                    bestPage = i;
                }
            }
        }
        if (bestPage >= 0) {
            selectActivePage(bestPage);
        }
    }

    void insertPageAfter(int afterIndex, PageSize size) {
        if (pages.empty()) {
            pages.push_back({pageWidth, pageHeight});
        }
        const int clamped = std::clamp(afterIndex, 0, pageCount() - 1);
        const int insertedIndex = clamped + 1;
        cancel();
        pages.insert(pages.begin() + insertedIndex, size);
        auto shiftStrokes = [&](std::vector<Ink>& strokes) {
            for (auto& stroke: strokes) {
                if (stroke.page >= insertedIndex) {
                    ++stroke.page;
                }
            }
        };
        shiftStrokes(ink);
        for (auto& edit: undoStates) {
            for (auto& change: edit.changes) {
                shiftStrokes(change.before);
                shiftStrokes(change.after);
            }
        }
        for (auto& edit: redoStates) {
            for (auto& change: edit.changes) {
                shiftStrokes(change.before);
                shiftStrokes(change.after);
            }
        }
        selectActivePage(insertedIndex);
    }

    bool deletePageAt(int removeIndex) {
        if (pageCount() <= 1 || removeIndex < 0 || removeIndex >= pageCount()) {
            return false;
        }
        cancel();
        pages.erase(pages.begin() + removeIndex);
        std::erase_if(ink, [&](const Ink& stroke) { return stroke.page == removeIndex; });
        for (auto& stroke: ink) {
            if (stroke.page > removeIndex) {
                --stroke.page;
            }
        }
        undoStates.clear();
        redoStates.clear();
        ++nextRevision;
        selectActivePage(std::min(removeIndex, pageCount() - 1));
        return true;
    }

    void zoomAt(double value, double x, double y) {
        if (!std::isfinite(value) || current) {
            return;
        }
        const auto anchor = documentPoint(x, y);
        zoom = std::clamp(value, 0.1, 8.0);
        offsetX = x - anchor.x * zoom;
        offsetY = y - anchor.y * zoom;
    }

    bool begin(Device source, double x, double y, double pressure = 1, bool eraserEnd = false) {
        const bool useEraser = eraserEnd || brush.tool == Tool::Eraser;
        const auto hit = hitPage(x, y, useEraser ? 1 : pressure);
        if ((useEraser && eraserMode != EraserMode::Whiteout) || pinching || current || eraseGesture || !hit) {
            return false;
        }
        selectActivePage(hit->page);
        owner = source;
        if (useEraser) {
            current = Ink{hit->page,
                          {hit->local},
                          Brush{Tool::Eraser, behavior::whiteoutColor, activeEraserWidth()},
                          false};
        } else {
            current = Ink{hit->page, {hit->local}, brush, false};
        }
        return true;
    }

    void append(Device source, double x, double y, double pressure = 1) {
        if (!current || source != owner) {
            return;
        }
        const auto p = pageLocalPoint(current->page, x, y, current->brush.tool == Tool::Eraser ? 1 : pressure);
        if (containsOnPage(current->page, p)) {
            auto& last = current->points.back();
            if (std::hypot(p.x - last.x, p.y - last.y) < behavior::motionThreshold) {
                if (current->points.size() == 1 && current->brush.tool == Tool::Pen) {
                    last.pressure = std::max(last.pressure, p.pressure);
                }
                return;
            }
            current->points.push_back(p);
        }
    }

    void finish(Device source, std::optional<double> releasePressure = std::nullopt) {
        if (current && source == owner) {
            if (current->points.size() == 1) {
                const auto point = current->points.front();
                if (releasePressure && std::isfinite(*releasePressure) && current->brush.tool == Tool::Pen) {
                    current->points.front().pressure = std::max(point.pressure, *releasePressure);
                }
                current->points.push_back(point);
            }
            record(Edit{{{ink.size(), {}, ink.size(), {*current}}}});
            ink.push_back(std::move(*current));
            current.reset();
        }
    }

    void cancel() {
        finishErase();
        current.reset();
    }

    void cancel(Device source) {
        if (current && owner == source) {
            cancel();
        }
    }

    void undo() {
        cancel();
        if (!undoStates.empty()) {
            apply(undoStates.back(), false);
            redoStates.push_back(std::move(undoStates.back()));
            undoStates.pop_back();
        }
    }

    void redo() {
        cancel();
        if (!redoStates.empty()) {
            apply(redoStates.back(), true);
            undoStates.push_back(std::move(redoStates.back()));
            redoStates.pop_back();
        }
    }

    bool canUndo() const { return !undoStates.empty(); }
    bool canRedo() const { return !redoStates.empty(); }
    size_t revision() const { return undoStates.empty() ? 0 : undoStates.back().revision; }

    void beginErase() {
        if (!eraseGesture) {
            EraseGesture gesture;
            gesture.slotOrigin.resize(ink.size());
            for (size_t i = 0; i < ink.size(); ++i) {
                gesture.slotOrigin[i] = i;
            }
            eraseGesture = std::move(gesture);
        }
    }

    void finishErase() {
        if (!eraseGesture) {
            return;
        }
        auto gesture = std::move(*eraseGesture);
        eraseGesture.reset();
        if (gesture.touchedOriginals.empty()) {
            return;
        }
        Edit edit;
        edit.changes.reserve(gesture.touchedOriginals.size());
        for (auto& [origin, original]: gesture.touchedOriginals) {
            const auto [first, last] = std::equal_range(gesture.slotOrigin.begin(), gesture.slotOrigin.end(), origin);
            const auto afterIdx = static_cast<size_t>(first - gesture.slotOrigin.begin());
            const auto afterEnd = static_cast<size_t>(last - gesture.slotOrigin.begin());
            std::vector<Ink> remaining(ink.begin() + static_cast<std::ptrdiff_t>(afterIdx),
                                       ink.begin() + static_cast<std::ptrdiff_t>(afterEnd));
            edit.changes.push_back({origin, {std::move(original)}, afterIdx, std::move(remaining)});
        }
        record(std::move(edit));
    }

    void erase(double x, double y) {
        if (current || pinching || eraserMode == EraserMode::Whiteout) {
            return;
        }
        const bool grouped = eraseGesture.has_value();
        if (!grouped) {
            beginErase();
        }
        if (const auto hit = hitPage(x, y)) {
            selectActivePage(hit->page);
            eraseStep(hit->page, hit->local);
        }
        if (!grouped) {
            finishErase();
        }
    }

    void beginPinch() {
        cancel();
        pinching = true;
    }

    void endPinch(long long now) {
        pinching = false;
        scrollBlockedUntil = now + 200000;
    }

    bool blocksScroll(bool discrete, long long now) const {
        return !discrete && (pinching || now < scrollBlockedUntil);
    }

    void resetInput() {
        cancel();
        pinching = false;
        scrollBlockedUntil = 0;
    }

private:
    struct Box {
        double minX;
        double minY;
        double maxX;
        double maxY;

        bool contains(const Point& p) const {
            return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
        }
    };

    struct Interval {
        double min;
        double max;
    };

    struct PathParam {
        size_t index;
        double t;

        bool operator==(const PathParam& other) const { return index == other.index && t == other.t; }
        bool operator<(const PathParam& other) const {
            return index < other.index || (index == other.index && t < other.t);
        }
        bool operator<=(const PathParam& other) const { return !(other < *this); }
    };

    struct SplitResult {
        bool changed = false;
        std::vector<Ink> remaining;
    };

    struct Change {
        size_t beforeIndex = 0;
        std::vector<Ink> before;
        size_t afterIndex = 0;
        std::vector<Ink> after;
    };

    struct Edit {
        std::vector<Change> changes;
        size_t revision = 0;
    };

    struct EraseGesture {
        std::vector<size_t> slotOrigin;
        std::map<size_t, Ink> touchedOriginals;
    };

    static Point lerpPoint(const Point& a, const Point& b, double t) {
        return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.pressure + t * (b.pressure - a.pressure)};
    }

    static Point pointAt(const Ink& stroke, PathParam param) {
        return lerpPoint(stroke.points[param.index], stroke.points[param.index + 1], param.t);
    }

    static std::optional<Interval> intersectLineWithBox(const Point& p, const Point& q, const Box& box) {
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

    static std::vector<double> intersectSegmentWithBox(const Point& p, const Point& q, const Box& box) {
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

    static bool allPointsCoincident(const std::vector<Point>& pts) {
        for (size_t i = 1; i < pts.size(); ++i) {
            if (pts[i].x != pts[0].x || pts[i].y != pts[0].y) {
                return false;
            }
        }
        return true;
    }

    static bool intersectsSquare(const Ink& stroke, double cx, double cy, double halfSize) {
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

    // Mirrors Stroke::intersectWithPaddedBox + ErasableStroke::getStrokes in src/core/model/.
    static SplitResult eraseWithPaddedBox(const Ink& stroke, double cx, double cy, double halfSize) {
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
            const Point mid = start.index == end.index ?
                                      pointAt(stroke, {start.index, 0.5 * (start.t + end.t)}) :
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
            return Ink{stroke.page, std::move(pts), stroke.brush, stroke.pressureSensitive};
        };

        const bool closedStroke = stroke.points.size() >= 3 &&
                                  std::hypot(stroke.points.front().x - stroke.points.back().x,
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
                out.remaining.push_back({stroke.page, std::move(pts), stroke.brush, stroke.pressureSensitive});
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

    void eraseStep(int targetPage, const Point& p) {
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
            if (ink[i].page != targetPage) {
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

    void record(Edit edit) {
        redoStates.clear();
        edit.revision = ++nextRevision;
        // Keep stroke deltas, rather than a full document copy for each edit.
        undoStates.push_back(std::move(edit));
    }

    void apply(const Edit& edit, bool forward) {
        for (auto it = edit.changes.rbegin(); it != edit.changes.rend(); ++it) {
            if (forward) {
                const auto pos = ink.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex);
                ink.erase(pos, pos + static_cast<std::ptrdiff_t>(it->before.size()));
                ink.insert(ink.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex), it->after.begin(),
                           it->after.end());
            } else {
                const auto pos = ink.begin() + static_cast<std::ptrdiff_t>(it->afterIndex);
                ink.erase(pos, pos + static_cast<std::ptrdiff_t>(it->after.size()));
                ink.insert(ink.begin() + static_cast<std::ptrdiff_t>(it->afterIndex), it->before.begin(),
                           it->before.end());
            }
        }
    }

    Device owner = Device::Mouse;
    size_t nextRevision = 0;
    std::vector<Edit> undoStates;
    std::vector<Edit> redoStates;
    std::optional<EraseGesture> eraseGesture;
};

}  // namespace prototype
