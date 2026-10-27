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

void CanvasState::fit(double width, double height) {
    cancel();
    if (!pages.empty()) {
        selectActivePage(std::clamp(page, 0, pageCount() - 1));
    }
    const auto sz = pageSize(page);
    zoom = std::clamp(std::min(std::max(1.0, width - 64) / sz.width, std::max(1.0, height - 48) / sz.height), 0.1, 8.0);
    if (pages.empty()) {
        offsetX = (width - pageWidth * zoom) / 2;
        offsetY = (height - pageHeight * zoom) / 2;
    } else {
        offsetX = (width - layoutWidth() * zoom) / 2;
        if (pageLayout.columnCount() > 1) {
            offsetX = (width - sz.width * zoom) / 2 - pageLeft(page) * zoom;
            offsetY = (height - sz.height * zoom) / 2 - pageTop(page) * zoom;
        } else {
            offsetY = (height - sz.height * zoom) / 2 - pageTop(page) * zoom;
        }
    }
}

void CanvasState::fitPresentation(double width, double height) {
    cancel();
    if (!pages.empty()) {
        selectActivePage(std::clamp(page, 0, pageCount() - 1));
    }
    const auto size = pageSize(page);
    const double fitWidth = std::max(1.0, width) / (size.width + 14.0);
    const double fitHeight = std::max(1.0, height) / (size.height + 14.0);
    zoom = std::clamp(std::min(fitWidth, fitHeight), 0.1, 8.0);
    offsetX = (width - size.width * zoom) / 2.0 - pageLeft(page) * zoom;
    offsetY = (height - size.height * zoom) / 2.0 - pageTop(page) * zoom;
}

void CanvasState::scrollBy(double dx, double dy, bool discrete) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return;
    }
    offsetX -= behavior::scrollDeltaToPixels(dx, discrete);
    offsetY -= behavior::scrollDeltaToPixels(dy, discrete);
}

bool CanvasState::hasHorizontalScrollRoom(double viewportWidth) const {
    return std::isfinite(viewportWidth) && viewportWidth > 0 && std::isfinite(zoom) && zoom > 0 &&
           layoutWidth() * zoom > viewportWidth;
}

void CanvasState::constrainScrollOffsets(double viewportWidth, double viewportHeight) {
    if (!std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) || viewportWidth <= 0 || viewportHeight <= 0 ||
        !std::isfinite(zoom) || zoom <= 0) {
        return;
    }

    const auto constrainAxis = [](double offset, double viewportLength, double contentLength) {
        if (contentLength <= viewportLength) {
            return (viewportLength - contentLength) / 2.0;
        }
        return std::clamp(offset, viewportLength - contentLength, 0.0);
    };

    offsetX = constrainAxis(offsetX, viewportWidth, layoutWidth() * zoom);
    offsetY = constrainAxis(offsetY, viewportHeight, layoutHeight() * zoom);
}

void CanvasState::scrollToPage(int targetPage, double viewportWidth, double viewportHeight) {
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
    if (pageLayout.columnCount() > 1) {
        const double left = pageLeft(page) * zoom;
        const double top = pageTop(page) * zoom;
        offsetX = sz.width * zoom + 64 <= viewportWidth ? (viewportWidth - sz.width * zoom) / 2 - left : 24.0 - left;
        offsetY = sz.height * zoom + 48 <= viewportHeight ? (viewportHeight - sz.height * zoom) / 2 - top : 24.0 - top;
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

void CanvasState::syncActivePage(double viewportWidth, double viewportHeight) {
    if (pages.empty() || viewportWidth <= 0 || viewportHeight <= 0) {
        return;
    }
    int bestPage = -1;
    double bestArea = 0.0;
    for (const int i: visiblePageIndices(viewportWidth, viewportHeight)) {
        const auto& sz = pages[static_cast<size_t>(i)];
        const double x0 = std::max(0.0, offsetX + pageLeft(i) * zoom);
        const double y0 = std::max(0.0, offsetY + pageTop(i) * zoom);
        const double x1 = std::min(viewportWidth, offsetX + (pageLeft(i) + sz.width) * zoom);
        const double y1 = std::min(viewportHeight, offsetY + (pageTop(i) + sz.height) * zoom);
        if (x1 > x0 && y1 > y0) {
            const double area = (x1 - x0) * (y1 - y0);
            if (area > bestArea || (area == bestArea && (bestPage < 0 || i < bestPage))) {
                bestArea = area;
                bestPage = i;
            }
        }
    }
    if (bestPage >= 0) {
        selectActivePage(bestPage);
    }
}

}  // namespace prototype
