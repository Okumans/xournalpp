#include "CanvasState.h"

namespace prototype {

PageSize CanvasState::pageSize(int index) const {
    if (pages.empty() || index < 0 || static_cast<size_t>(index) >= pages.size()) {
        return {pageWidth, pageHeight};
    }
    return pages[static_cast<size_t>(index)];
}

void CanvasState::selectActivePage(int target) {
    if (!pages.empty()) {
        page = std::clamp(target, 0, pageCount() - 1);
        pageWidth = pages[static_cast<size_t>(page)].width;
        pageHeight = pages[static_cast<size_t>(page)].height;
    } else if (target >= 0) {
        page = target;
    }
}

void CanvasState::setPages(std::vector<PageSize> nextPages) {
    cancel();
    clearSelection();
    pages = std::move(nextPages);
    ++pageStructureGeneration;
    if (pageLayers.size() > pages.size()) {
        pageLayers.resize(pages.size());
    }
    while (pageLayers.size() < pages.size()) {
        pageLayers.emplace_back();
    }
    if (!pages.empty()) {
        selectActivePage(std::clamp(page, 0, pageCount() - 1));
    }
    rebuildPageLayout();
}

void CanvasState::setLayoutSettings(const CanvasLayoutSettings& settings) {
    layoutSettings = settings;
    rebuildPageLayout();
}

void CanvasState::rebuildPageLayout() { pageLayout.configure(pages, layoutSettings, pageGap); }

double CanvasState::layoutWidth() const {
    if (pages.empty()) {
        return pageWidth;
    }
    return pageLayout.layoutWidth();
}

double CanvasState::layoutHeight() const {
    if (pages.empty()) {
        return pageHeight;
    }
    return pageLayout.layoutHeight();
}

double CanvasState::pageLeft(int index) const {
    if (pages.empty()) {
        return 0.0;
    }
    return pageLayout.pageLeft(index, pages);
}

double CanvasState::pageTop(int index) const { return pageLayout.pageTop(index, pages); }

std::vector<int> CanvasState::visiblePageIndices(double viewportWidth, double viewportHeight, double marginX,
                                                 double marginY) const {
    if (!std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) || viewportWidth <= 0 || viewportHeight <= 0 ||
        !std::isfinite(marginX) || !std::isfinite(marginY) || marginX < 0 || marginY < 0 || !std::isfinite(zoom) ||
        zoom <= 0 || !std::isfinite(offsetX) || !std::isfinite(offsetY)) {
        return {};
    }
    const double left = (-marginX - offsetX) / zoom;
    const double top = (-marginY - offsetY) / zoom;
    const double right = (viewportWidth + marginX - offsetX) / zoom;
    const double bottom = (viewportHeight + marginY - offsetY) / zoom;
    if (pages.empty()) {
        const auto size = pageSize(0);
        if (left < size.width && right > 0 && top < size.height && bottom > 0) {
            return {0};
        }
        return {};
    }
    return pageLayout.pagesIntersecting(left, top, right, bottom, pages);
}

double CanvasState::activeEraserWidth() const { return brush.tool == Tool::Eraser ? brush.width : eraserWidth; }

double CanvasState::steppedZoom(bool zoomIn, bool scroll) const {
    return behavior::steppedZoom(zoom, zoomIn, scroll, zoomStep, scrollZoomStep);
}

Point CanvasState::documentPoint(double x, double y, double pressure) const {
    return {(x - offsetX) / zoom, (y - offsetY) / zoom,
            std::isfinite(pressure) ? std::max(minimumPressure, pressure) : 1.0};
}

Point CanvasState::pageLocalPoint(int pageIndex, double x, double y, double pressure) const {
    const auto doc = documentPoint(x, y, pressure);
    return {doc.x - pageLeft(pageIndex), doc.y - pageTop(pageIndex), doc.pressure};
}

Point CanvasState::pageScreenPoint(int pageIndex, double localX, double localY, double pressure) const {
    return {offsetX + (pageLeft(pageIndex) + localX) * zoom, offsetY + (pageTop(pageIndex) + localY) * zoom, pressure};
}

bool CanvasState::contains(const Point& p) const {
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0 && p.y >= 0 && p.x <= pageWidth && p.y <= pageHeight;
}

bool CanvasState::containsOnPage(int pageIndex, const Point& local) const {
    const auto sz = pageSize(pageIndex);
    return std::isfinite(local.x) && std::isfinite(local.y) && local.x >= 0 && local.y >= 0 && local.x <= sz.width &&
           local.y <= sz.height;
}

std::optional<PageHit> CanvasState::hitPage(double x, double y, double pressure) const {
    if (pages.empty()) {
        const auto p = documentPoint(x, y, pressure);
        if (contains(p)) {
            return PageHit{page, p};
        }
        return std::nullopt;
    }
    const auto doc = documentPoint(x, y, pressure);
    if (!std::isfinite(doc.x) || !std::isfinite(doc.y)) {
        return std::nullopt;
    }
    const auto index = pageLayout.pageAt(doc.x, doc.y, pages);
    if (!index) {
        return std::nullopt;
    }
    return PageHit{*index, {doc.x - pageLeft(*index), doc.y - pageTop(*index), doc.pressure}};
}

std::optional<PageHit> CanvasState::hitSelectionHandle(double x, double y) const {
    const auto bounds = selectionBox();
    if (!bounds || bounds->page < 0 || bounds->page >= pageCount()) {
        return std::nullopt;
    }
    const auto local = pageLocalPoint(bounds->page, x, y);
    const double radius = 5 / std::max(0.1, zoom);
    for (const auto& handle: selectionHandles()) {
        if (std::hypot(handle.x - local.x, handle.y - local.y) <= radius) {
            return PageHit{bounds->page, local};
        }
    }
    return std::nullopt;
}

}  // namespace prototype
