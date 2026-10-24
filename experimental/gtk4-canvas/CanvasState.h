#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "BehaviorDefaults.h"
#include "CanvasText.h"

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

enum class Tool { Pen, Highlighter, Eraser, SelectRectangle, SelectLasso, SmartSelect, Text };
inline bool isSelectionTool(Tool tool) {
    return tool == Tool::SelectRectangle || tool == Tool::SelectLasso || tool == Tool::SmartSelect;
}
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
    size_t layerOrder = 0;
};

struct SelectionBox {
    int page;
    double x0, y0, x1, y1;
};

struct SelectionTransform {
    double xx = 1, yx = 0, xy = 0, yy = 1, x0 = 0, y0 = 0;
    double widthScale = 1;

    Point apply(Point p) const { return {xx * p.x + xy * p.y + x0, yx * p.x + yy * p.y + y0, p.pressure}; }
    bool identity() const { return xx == 1 && yy == 1 && xy == 0 && yx == 0 && x0 == 0 && y0 == 0; }
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
    bool pressureSensitivity = true;
    double minimumPressure = behavior::minimumPressure;
    double pressureMultiplier = behavior::pressureMultiplier;
    double zoomStep = behavior::zoomStep;
    double scrollZoomStep = behavior::scrollZoomStep;
    std::string textFontName = "Sans";
    double textFontSize = 12;
    std::vector<Ink> ink;
    std::vector<TextItem> texts;
    std::optional<Ink> current;
    std::vector<size_t> selected;
    bool pinching = false;
    long long scrollBlockedUntil = 0;

    int pageCount() const { return pages.empty() ? 1 : static_cast<int>(pages.size()); }

    std::optional<SelectionBox> selectionBox() const {
        if (selectionGesture && selectionGesture->moving) {
            const auto& g = *selectionGesture;
            const auto corners = transformedCorners(g.bounds, g.transform);
            SelectionBox box{g.targetPage, corners[0].x, corners[0].y, corners[0].x, corners[0].y};
            for (const auto& p: corners) {
                box.x0 = std::min(box.x0, p.x); box.y0 = std::min(box.y0, p.y);
                box.x1 = std::max(box.x1, p.x); box.y1 = std::max(box.y1, p.y);
            }
            return box;
        }
        if (selectionGesture) {
            const auto& g = *selectionGesture;
            return SelectionBox{g.page, std::min(g.start.x, g.latest.x), std::min(g.start.y, g.latest.y),
                                 std::max(g.start.x, g.latest.x), std::max(g.start.y, g.latest.y)};
        }
        std::optional<SelectionBox> box;
        for (auto index: selected) {
            if (index >= ink.size()) {
                continue;
            }
            for (const auto& point: ink[index].points) {
                if (!box) {
                    box = SelectionBox{ink[index].page, point.x, point.y, point.x, point.y};
                } else {
                    box->x0 = std::min(box->x0, point.x);
                    box->y0 = std::min(box->y0, point.y);
                    box->x1 = std::max(box->x1, point.x);
                    box->y1 = std::max(box->y1, point.y);
                }
            }
        }
        return box;
    }

    bool selecting() const { return selectionGesture.has_value(); }

    const std::vector<Ink>& committedInk() const { return ink; }

    static std::array<Point, 4> transformedCorners(const SelectionBox& b, const SelectionTransform& matrix = {}) {
        return {matrix.apply({b.x0, b.y0}), matrix.apply({b.x1, b.y0}),
                matrix.apply({b.x1, b.y1}), matrix.apply({b.x0, b.y1})};
    }

    SelectionTransform selectionMatrix(size_t index) const {
        if (selectionGesture && selectionGesture->moving && std::binary_search(selected.begin(), selected.end(), index)) {
            return selectionGesture->transform;
        }
        return {};
    }

    int displayPage(size_t index) const {
        if (selectionGesture && selectionGesture->moving && std::binary_search(selected.begin(), selected.end(), index)) {
            return selectionGesture->targetPage;
        }
        return ink.at(index).page;
    }

    std::vector<Point> selectionPath() const {
        if (selectionGesture && !selectionGesture->moving && selectionGesture->lasso) {
            return selectionGesture->boundary;
        }
        return {};
    }

    SelectionBox handleBounds(SelectionBox box) const {
        const auto extent = 20 / zoom;
        if (box.x1-box.x0 < extent) {
            const auto center = (box.x0+box.x1)/2;
            box.x0 = center-extent/2; box.x1 = center+extent/2;
        }
        if (box.y1-box.y0 < extent) {
            const auto center = (box.y0+box.y1)/2;
            box.y0 = center-extent/2; box.y1 = center+extent/2;
        }
        return box;
    }

    std::array<Point, 9> selectionHandles() const {
        const auto b = selectionBox();
        if (!b) return {};
        auto corners = selectionGesture && selectionGesture->moving ?
                               transformedCorners(selectionGesture->bounds, selectionGesture->transform) :
                               transformedCorners(handleBounds(*b));
        auto mid = [](Point a, Point b) { return Point{(a.x + b.x) / 2, (a.y + b.y) / 2}; };
        const auto right = mid(corners[1], corners[2]);
        const auto center = mid(corners[0], corners[2]);
        const auto length = std::hypot(right.x - center.x, right.y - center.y);
        const Point rotation = length > 1e-8 ? Point{right.x + 24 / zoom * (right.x-center.x)/length,
                                                    right.y + 24 / zoom * (right.y-center.y)/length} :
                                                 Point{right.x + 24 / zoom, right.y};
        return {corners[0], mid(corners[0], corners[1]), corners[1], right, corners[2],
                mid(corners[2], corners[3]), corners[3], mid(corners[3], corners[0]), rotation};
    }

    Point selectionOffset(size_t index) const {
        const auto matrix = selectionMatrix(index);
        return {matrix.x0, matrix.y0};
    }

    void cancelSelection() {
        if (selectionGesture) {
            selected = std::move(selectionGesture->previousSelection);
            selectionGesture.reset();
        }
    }

    void clearSelection() {
        cancelSelection();
        selected.clear();
    }

    std::optional<size_t> pickStroke(int pageIndex, Point point) const {
        for (size_t i = ink.size(); i-- > 0;) {
            const auto& stroke = ink[i];
            if (stroke.page != pageIndex) continue;
            for (size_t n = 0; n < stroke.points.size(); ++n) {
                const auto& a = stroke.points[n ? n-1 : 0];
                const auto& b = stroke.points[n];
                const auto dx = b.x-a.x, dy = b.y-a.y;
                const auto length2 = dx*dx+dy*dy;
                const auto t = length2 ? std::clamp(((point.x-a.x)*dx+(point.y-a.y)*dy)/length2, 0.0, 1.0) : 0;
                const auto pressure = stroke.pressureSensitive ? a.pressure + t*(b.pressure-a.pressure) : 1;
                if (std::hypot(point.x-a.x-t*dx, point.y-a.y-t*dy) <= .5 + stroke.brush.width*pressure/2) return i;
            }
        }
        return std::nullopt;
    }

    static void extendLassoAtPageEdges(std::vector<Point>& boundary, const PageSize& size) {
        if (boundary.size() <= 2 || size.width <= 0 || size.height <= 0) return;
        const auto onEdge = [&](const Point& p) {
            return p.x <= 1 || p.x >= size.width - 1 || p.y <= 1 || p.y >= size.height - 1;
        };
        if (std::none_of(boundary.begin(), boundary.end(), onEdge)) return;
        const auto project = [&](const Point& p) {
            return Point{p.x <= 1 ? std::numeric_limits<double>::lowest() :
                                 p.x >= size.width - 1 ? std::numeric_limits<double>::max() : p.x,
                         p.y <= 1 ? std::numeric_limits<double>::lowest() :
                                 p.y >= size.height - 1 ? std::numeric_limits<double>::max() : p.y};
        };
        std::vector<Point> extended;
        extended.reserve(boundary.size() * 2);
        for (size_t i = 0; i < boundary.size(); ++i) {
            const auto& current = boundary[i];
            if (!onEdge(current)) {
                extended.push_back(current);
                continue;
            }
            const bool previousOnEdge = onEdge(boundary[(i + boundary.size() - 1) % boundary.size()]);
            const bool nextOnEdge = onEdge(boundary[(i + 1) % boundary.size()]);
            if (!previousOnEdge) extended.push_back(current);
            if (!previousOnEdge || nextOnEdge) extended.push_back(project(current));
            if (previousOnEdge && !nextOnEdge) extended.push_back(current);
        }
        boundary = std::move(extended);
    }

    bool beginSelection(double x, double y, bool forceRectangle = false) {
        const auto hit = hitPage(x, y);
        if (!hit || current || eraseGesture || selectionGesture || pinching) {
            return false;
        }
        SelectionGesture gesture;
        gesture.page = hit->page;
        gesture.targetPage = hit->page;
        gesture.start = gesture.latest = hit->local;
        gesture.previousSelection = selected;
        auto bounds = selectionBox();
        if (bounds && bounds->page == hit->page && !forceRectangle) {
            const auto handles = selectionHandles();
            for (int n = 0; n < 9; ++n) {
                if (std::hypot(handles[n].x-hit->local.x, handles[n].y-hit->local.y) <= 5/zoom) {
                    gesture.handle = n;
                    break;
                }
            }
        }
        const auto padding = 5 / zoom;
        gesture.moving = !forceRectangle && (gesture.handle >= 0 || (bounds && bounds->page == hit->page && hit->local.x >= bounds->x0 - padding &&
                         hit->local.x <= bounds->x1 + padding && hit->local.y >= bounds->y0 - padding &&
                         hit->local.y <= bounds->y1 + padding));
        if (!gesture.moving && brush.tool == Tool::SmartSelect && !forceRectangle) {
            if (auto picked = pickStroke(hit->page, hit->local)) {
                selected = {*picked};
                bounds = selectionBox();
                gesture.moving = true;
            }
        }
        if (gesture.moving) {
            gesture.indices = selected;
            gesture.bounds = gesture.handle >= 0 ? handleBounds(*bounds) : *bounds;
        } else {
            selected.clear();
            gesture.lasso = brush.tool == Tool::SelectLasso && !forceRectangle;
            gesture.boundary.push_back(hit->local);
            gesture.additive = forceRectangle && bounds && bounds->page == hit->page;
        }
        selectActivePage(hit->page);
        selectionGesture = std::move(gesture);
        return true;
    }

    void moveSelectionPointer(double x, double y) {
        if (!selectionGesture || !std::isfinite(x) || !std::isfinite(y)) {
            return;
        }
        auto& g = *selectionGesture;
        if (g.moving && g.handle < 0) {
            if (const auto hit = hitPage(x, y)) g.targetPage = hit->page;
        }
        g.latest = pageLocalPoint(g.targetPage, x, y);
        if (g.moving) {
            const auto previous = g.transform;
            g.transform = {};
            if (g.handle < 0) {
                g.transform.x0 = g.latest.x - g.start.x;
                g.transform.y0 = g.latest.y - g.start.y;
            } else if (g.handle == 8) {
                const auto cx = (g.bounds.x0+g.bounds.x1)/2, cy = (g.bounds.y0+g.bounds.y1)/2;
                const auto angle = std::atan2(g.latest.y-cy, g.latest.x-cx) - std::atan2(g.start.y-cy, g.start.x-cx);
                const auto cosine = std::cos(angle), sine = std::sin(angle);
                g.transform = {cosine,sine,-sine,cosine,cx-cosine*cx+sine*cy,cy-sine*cx-cosine*cy,1};
            } else {
                constexpr int sx[] = {-1,0,1,1,1,0,-1,-1};
                constexpr int sy[] = {-1,-1,-1,0,1,1,1,0};
                const auto xs = sx[g.handle], ys = sy[g.handle];
                const auto width = std::max(g.bounds.x1-g.bounds.x0,5/zoom);
                const auto height = std::max(g.bounds.y1-g.bounds.y0,5/zoom);
                const auto ax = xs < 0 ? g.bounds.x1 : g.bounds.x0;
                const auto ay = ys < 0 ? g.bounds.y1 : g.bounds.y0;
                const auto vx = xs*width, vy = ys*height;
                const auto projection = (xs ? (g.latest.x-ax)*vx : 0) + (ys ? (g.latest.y-ay)*vy : 0);
                auto factor = projection/(vx*vx+vy*vy);
                const auto constrained = xs && ys ? std::min(width, height) : (xs ? width : height);
                factor = std::copysign(std::max(std::abs(factor), (5 / zoom) / constrained), factor);
                const auto fx = xs ? factor : 1, fy = ys ? factor : 1;
                g.transform = {fx,0,0,fy,ax*(1-fx),ay*(1-fy),std::sqrt(std::abs(fx*fy))};
            }
            const auto& m = g.transform;
            if (!std::isfinite(m.xx) || !std::isfinite(m.xy) || !std::isfinite(m.yx) || !std::isfinite(m.yy) ||
                !std::isfinite(m.x0) || !std::isfinite(m.y0) || !std::isfinite(m.widthScale) || m.widthScale <= 0)
                g.transform = previous;
        } else if (g.lasso && g.boundary.size() < 1000000 &&
                   std::hypot(g.latest.x-g.boundary.back().x,g.latest.y-g.boundary.back().y) >= .3) {
            g.boundary.push_back(g.latest);
        }
    }

    void finishSelection() {
        if (!selectionGesture) {
            return;
        }
        auto g = std::move(*selectionGesture);
        selectionGesture.reset();
        if (g.moving) {
            Edit edit;
            if (!g.transform.identity() || g.targetPage != g.page) {
                std::vector<Ink> transferred;
                for (auto index: g.indices) {
                    auto moved = ink.at(index);
                    for (auto& point: moved.points) {
                        point = g.transform.apply(point);
                    }
                    moved.brush.width *= g.transform.widthScale;
                    if (!std::isfinite(moved.brush.width) || moved.brush.width <= 0 ||
                        !std::all_of(moved.points.begin(), moved.points.end(), [](const Point& p) {
                            return std::isfinite(p.x) && std::isfinite(p.y);
                        })) return;
                    moved.page = g.targetPage;
                    if (g.targetPage == g.page) {
                        edit.changes.push_back({index, {ink[index]}, index, {std::move(moved)}});
                    } else {
                        moved.layerOrder = nextLayerOrder(g.targetPage) + transferred.size();
                        const auto removed = transferred.size();
                        edit.changes.push_back({index, {ink[index]}, index-removed, {}});
                        transferred.push_back(std::move(moved));
                    }
                }
                if (!transferred.empty()) edit.changes.push_back({ink.size(), {}, ink.size()-transferred.size(), std::move(transferred)});
            }
            if (!edit.changes.empty()) {
                edit.selectionBefore = edit.selectionAfter = selected;
                if (g.targetPage != g.page) {
                    edit.selectionAfter = std::vector<size_t>{};
                    for (size_t n = ink.size()-selected.size(); n < ink.size(); ++n) edit.selectionAfter->push_back(n);
                }
                apply(edit, true);
                record(std::move(edit));
                selectActivePage(g.targetPage);
            }
        } else {
            const auto size = pageSize(g.page);
            const auto x0 = std::min(g.start.x, g.latest.x), x1 = std::max(g.start.x, g.latest.x);
            const auto y0 = std::min(g.start.y, g.latest.y), y1 = std::max(g.start.y, g.latest.y);
            if (g.additive) selected = g.previousSelection;
            if (g.lasso) extendLassoAtPageEdges(g.boundary, size);
            // RectangularSelector extends at page edges (1 pt). Stroke selection
            // tests every point, rather than accepting a crossing segment.
            for (size_t i = 0; i < ink.size(); ++i) {
                if (ink[i].page == g.page && !ink[i].points.empty() &&
                    std::all_of(ink[i].points.begin(), ink[i].points.end(), [&](const Point& p) {
                        if (g.lasso) return polygonContains(g.boundary, p);
                        return (x0 <= 1 || p.x >= x0) && (x1 >= size.width - 1 || p.x <= x1) &&
                               (y0 <= 1 || p.y >= y0) && (y1 >= size.height - 1 || p.y <= y1);
                    })) {
                    selected.push_back(i);
                }
            }
            std::sort(selected.begin(),selected.end());
            selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
        }
    }

    void selectAllOnPage() {
        clearSelection();
        for (size_t i = 0; i < ink.size(); ++i) {
            if (ink[i].page == page && !ink[i].points.empty()) {
                selected.push_back(i);
            }
        }
    }

    void deleteSelection() {
        cancelSelection();
        if (selected.empty()) {
            return;
        }
        Edit edit;
        edit.selectionBefore = selected;
        edit.selectionAfter = std::vector<size_t>{};
        for (size_t n = 0; n < selected.size(); ++n) {
            const auto i = selected[n];
            edit.changes.push_back({i, {ink.at(i)}, i - n, {}});
        }
        apply(edit, true);
        record(std::move(edit));
    }

    static bool polygonContains(const std::vector<Point>& polygon, Point p) {
        if (polygon.size() < 3) return false;
        int winding = 0;
        for (size_t i = 0; i < polygon.size(); ++i) {
            const auto a = polygon[i], b = polygon[(i+1)%polygon.size()];
            const long double cross = (static_cast<long double>(b.x)-a.x)*(static_cast<long double>(p.y)-a.y)-
                                      (static_cast<long double>(p.x)-a.x)*(static_cast<long double>(b.y)-a.y);
            if (cross == 0 && p.x >= std::min(a.x,b.x) && p.x <= std::max(a.x,b.x) &&
                p.y >= std::min(a.y,b.y) && p.y <= std::max(a.y,b.y)) return true;
            if (a.y <= p.y && b.y > p.y && cross > 0) ++winding;
            if (a.y > p.y && b.y <= p.y && cross < 0) --winding;
        }
        return winding != 0;
    }

    std::vector<Ink> copySelection() const {
        std::vector<Ink> copied;
        for (auto index: selected) copied.push_back(ink.at(index));
        return copied;
    }

    bool pasteStrokes(std::vector<Ink> pasted, int targetPage, Point center) {
        if (pasted.empty() || targetPage < 0 || targetPage >= pageCount()) return false;
        if (!std::isfinite(center.x) || !std::isfinite(center.y)) return false;
        for (const auto& stroke: pasted) {
            if (stroke.points.empty() || !std::isfinite(stroke.brush.width) || stroke.brush.width <= 0 || isSelectionTool(stroke.brush.tool)) return false;
            for (const auto& p: stroke.points) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.pressure) || p.pressure <= 0) return false;
            }
        }
        const auto previousSelection = selected;
        SelectionBox b{targetPage, pasted[0].points[0].x,pasted[0].points[0].y,pasted[0].points[0].x,pasted[0].points[0].y};
        for (const auto& stroke: pasted) for (const auto& p: stroke.points) {
            b.x0=std::min(b.x0,p.x);b.y0=std::min(b.y0,p.y);b.x1=std::max(b.x1,p.x);b.y1=std::max(b.y1,p.y);
        }
        const auto dx = std::max(0.0,center.x-(b.x1-b.x0)/2)-b.x0;
        const auto dy = std::max(0.0,center.y-(b.y1-b.y0)/2)-b.y0;
        auto layerOrder = nextLayerOrder(targetPage);
        for (auto& stroke: pasted) {
            stroke.page=targetPage;
            stroke.layerOrder = layerOrder++;
            for (auto& p: stroke.points) {
                p.x+=dx;p.y+=dy;
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
            }
        }
        clearSelection();
        Edit edit;
        edit.changes.push_back({ink.size(), {}, ink.size(), std::move(pasted)});
        edit.selectionBefore = previousSelection;
        edit.selectionAfter = std::vector<size_t>{};
        for (size_t i=0;i<edit.changes[0].after.size();++i) edit.selectionAfter->push_back(ink.size()+i);
        apply(edit,true);record(std::move(edit));selectActivePage(targetPage);
        return true;
    }

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
        clearSelection();
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

    double steppedZoom(bool zoomIn, bool scroll) const {
        return behavior::steppedZoom(zoom, zoomIn, scroll, zoomStep, scrollZoomStep);
    }

    Point documentPoint(double x, double y, double pressure = 1) const {
        return {(x - offsetX) / zoom, (y - offsetY) / zoom,
                std::isfinite(pressure) ? std::max(minimumPressure, pressure) : 1.0};
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
        shiftTexts(texts, insertedIndex);
        for (auto& edit: undoStates) {
            for (auto& change: edit.changes) {
                shiftStrokes(change.before);
                shiftStrokes(change.after);
            }
            for (auto& change: edit.textChanges) {
                shiftTexts(change.before, insertedIndex);
                shiftTexts(change.after, insertedIndex);
            }
        }
        for (auto& edit: redoStates) {
            for (auto& change: edit.changes) {
                shiftStrokes(change.before);
                shiftStrokes(change.after);
            }
            for (auto& change: edit.textChanges) {
                shiftTexts(change.before, insertedIndex);
                shiftTexts(change.after, insertedIndex);
            }
        }
        selectActivePage(insertedIndex);
    }

    bool deletePageAt(int removeIndex) {
        if (pageCount() <= 1 || removeIndex < 0 || removeIndex >= pageCount()) {
            return false;
        }
        cancel();
        clearSelection();
        pages.erase(pages.begin() + removeIndex);
        std::erase_if(ink, [&](const Ink& stroke) { return stroke.page == removeIndex; });
        for (auto& stroke: ink) {
            if (stroke.page > removeIndex) {
                --stroke.page;
            }
        }
        std::erase_if(texts, [&](const TextItem& text) { return text.page == removeIndex; });
        for (auto& text: texts) {
            if (text.page > removeIndex) {
                --text.page;
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
        if (brush.tool == Tool::Text) {
            return false;
        }
        if (isSelectionTool(brush.tool) && !eraserEnd) {
            return false;
        }
        clearSelection();
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
            current->layerOrder = nextLayerOrder(current->page);
            Edit edit;
            edit.changes.push_back({ink.size(), {}, ink.size(), {*current}});
            record(std::move(edit));
            ink.push_back(std::move(*current));
            current.reset();
        }
    }

    bool insertText(TextItem item) {
        if (!validTextPage(item) || current || eraseGesture || selectionGesture) {
            return false;
        }
        item.layerOrder = nextLayerOrder(item.page);
        Edit edit;
        edit.textChanges.push_back({texts.size(), {}, texts.size(), {std::move(item)}});
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    bool replaceText(size_t index, TextItem item) {
        if (index >= texts.size() || !validTextPage(item) || current || eraseGesture || selectionGesture ||
            item.page != texts[index].page) {
            return false;
        }
        item.layerOrder = texts[index].layerOrder;
        Edit edit;
        edit.textChanges.push_back({index, {texts[index]}, index, {std::move(item)}});
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    bool deleteText(size_t index) {
        if (index >= texts.size() || current || eraseGesture || selectionGesture) {
            return false;
        }
        Edit edit;
        edit.textChanges.push_back({index, {texts[index]}, index, {}});
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    void cancel() {
        cancelSelection();
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
        clearSelection();
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

    struct TextChange {
        size_t beforeIndex = 0;
        std::vector<TextItem> before;
        size_t afterIndex = 0;
        std::vector<TextItem> after;
    };

    struct Edit {
        std::vector<Change> changes;
        std::vector<TextChange> textChanges;
        size_t revision = 0;
        std::optional<std::vector<size_t>> selectionBefore = std::nullopt;
        std::optional<std::vector<size_t>> selectionAfter = std::nullopt;
    };

    bool validTextPage(const TextItem& item) const {
        return item.page >= 0 && item.page < pageCount() && std::isfinite(item.fontSize) && item.fontSize > 0 &&
               std::isfinite(item.wrapWidth) && std::isfinite(item.xx) && std::isfinite(item.yx) &&
               std::isfinite(item.xy) && std::isfinite(item.yy) && std::isfinite(item.x) &&
               std::isfinite(item.y);
    }

    size_t nextLayerOrder(int pageIndex) const {
        size_t next = 0;
        for (const auto& stroke: ink) {
            if (stroke.page == pageIndex) {
                next = std::max(next, stroke.layerOrder + 1);
            }
        }
        for (const auto& text: texts) {
            if (text.page == pageIndex) {
                next = std::max(next, text.layerOrder + 1);
            }
        }
        return next;
    }

    static void shiftTexts(std::vector<TextItem>& items, int insertedIndex) {
        for (auto& item: items) {
            if (item.page >= insertedIndex) {
                ++item.page;
            }
        }
    }

    struct SelectionGesture {
        int page = 0;
        Point start{}, latest{};
        bool moving = false;
        bool lasso = false, additive = false;
        int handle = -1, targetPage = 0;
        std::vector<size_t> previousSelection;
        std::vector<size_t> indices;
        SelectionBox bounds{};
        SelectionTransform transform;
        std::vector<Point> boundary;
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
            return Ink{stroke.page, std::move(pts), stroke.brush, stroke.pressureSensitive, stroke.layerOrder};
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
                out.remaining.push_back(
                        {stroke.page, std::move(pts), stroke.brush, stroke.pressureSensitive, stroke.layerOrder});
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
        // Keep ink and text deltas, rather than a full document copy for each edit.
        undoStates.push_back(std::move(edit));
    }

    void apply(const Edit& edit, bool forward) {
        for (auto it = edit.textChanges.rbegin(); it != edit.textChanges.rend(); ++it) {
            if (forward) {
                const auto pos = texts.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex);
                texts.erase(pos, pos + static_cast<std::ptrdiff_t>(it->before.size()));
                texts.insert(texts.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex), it->after.begin(),
                             it->after.end());
            } else {
                const auto pos = texts.begin() + static_cast<std::ptrdiff_t>(it->afterIndex);
                texts.erase(pos, pos + static_cast<std::ptrdiff_t>(it->after.size()));
                texts.insert(texts.begin() + static_cast<std::ptrdiff_t>(it->afterIndex), it->before.begin(),
                             it->before.end());
            }
        }
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
        selected = (forward ? edit.selectionAfter : edit.selectionBefore).value_or(std::vector<size_t>{});
    }

    Device owner = Device::Mouse;
    size_t nextRevision = 0;
    std::vector<Edit> undoStates;
    std::vector<Edit> redoStates;
    std::optional<EraseGesture> eraseGesture;
    std::optional<SelectionGesture> selectionGesture;
};

}  // namespace prototype
