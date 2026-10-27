#include "CanvasState.h"

namespace prototype {

std::vector<Ink> CanvasState::copySelection() const {
    std::vector<Ink> copied;
    for (auto index: selected) copied.push_back(ink.at(index));
    return copied;
}

std::vector<TextItem> CanvasState::copyTextSelection() const {
    std::vector<TextItem> copied;
    copied.reserve(selectedTexts.size());
    for (const auto index: selectedTexts) {
        copied.push_back(texts.at(index));
    }
    std::stable_sort(copied.begin(), copied.end(),
                     [](const TextItem& a, const TextItem& b) { return a.layerOrder < b.layerOrder; });
    return copied;
}

std::vector<EditableObject> CanvasState::copySelectedObjects() const {
    std::vector<EditableObject> copied;
    copied.reserve(selected.size() + selectedTexts.size() + selectedImages.size());
    for (const auto index: selected) {
        copied.emplace_back(ink.at(index));
    }
    for (const auto index: selectedTexts) {
        copied.emplace_back(texts.at(index));
    }
    for (const auto index: selectedImages) {
        copied.emplace_back(images.at(index));
    }

    const auto stackingOrder = [this](const EditableObject& object) {
        return std::visit(
                [this](const auto& item) {
                    size_t originOrder = 0;
                    if (item.layerId > 0 && item.page >= 0 && item.page < pageCount()) {
                        const auto& pageLayers = layersForPage(item.page).layers;
                        if (item.layerId <= pageLayers.size()) {
                            const auto& origins = pageLayers[item.layerId - 1].origins;
                            const auto origin = std::find(origins.begin(), origins.end(), item.originLayer);
                            originOrder = origin == origins.end() ? origins.size() :
                                                                    static_cast<size_t>(origin - origins.begin());
                        }
                    }
                    return std::tuple{item.page, item.layerId, originOrder, item.layerOrder};
                },
                object);
    };
    std::stable_sort(copied.begin(), copied.end(),
                     [&](const auto& lhs, const auto& rhs) { return stackingOrder(lhs) < stackingOrder(rhs); });
    return copied;
}

bool CanvasState::pasteStrokes(std::vector<Ink> pasted, int targetPage, Point center) {
    if (pasted.empty() || targetPage < 0 || targetPage >= pageCount())
        return false;
    if (!std::isfinite(center.x) || !std::isfinite(center.y))
        return false;
    for (const auto& stroke: pasted) {
        if (stroke.points.empty() || !std::isfinite(stroke.brush.width) || stroke.brush.width <= 0 ||
            isSelectionTool(stroke.brush.tool))
            return false;
        for (const auto& p: stroke.points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.pressure) || p.pressure <= 0)
                return false;
        }
    }
    const auto previousSelection = selected;
    const auto previousSelectionFrame = selectionFrame;
    SelectionBox b{targetPage, pasted[0].points[0].x, pasted[0].points[0].y, pasted[0].points[0].x,
                   pasted[0].points[0].y};
    for (const auto& stroke: pasted)
        for (const auto& p: stroke.points) {
            b.x0 = std::min(b.x0, p.x);
            b.y0 = std::min(b.y0, p.y);
            b.x1 = std::max(b.x1, p.x);
            b.y1 = std::max(b.y1, p.y);
        }
    const auto dx = std::max(0.0, center.x - (b.x1 - b.x0) / 2) - b.x0;
    const auto dy = std::max(0.0, center.y - (b.y1 - b.y0) / 2) - b.y0;
    auto layerOrder = nextOriginOrder(targetPage, activeLayerOrigin(targetPage));
    for (auto& stroke: pasted) {
        stroke.page = targetPage;
        stroke.layerId = activeLayerId(targetPage);
        stroke.originLayer = activeLayerOrigin(targetPage);
        stroke.layerOrder = layerOrder++;
        for (auto& p: stroke.points) {
            p.x += dx;
            p.y += dy;
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                return false;
        }
    }
    clearSelection();
    Edit edit;
    edit.changes.push_back({ink.size(), {}, ink.size(), std::move(pasted)});
    edit.selectionBefore = previousSelection;
    edit.selectionAfter = std::vector<size_t>{};
    for (size_t i = 0; i < edit.changes[0].after.size(); ++i) edit.selectionAfter->push_back(ink.size() + i);
    edit.hasSelectionFrameState = true;
    edit.selectionFrameBefore = previousSelectionFrame;
    apply(edit, true);
    record(std::move(edit));
    selectActivePage(targetPage);
    return true;
}

bool CanvasState::pasteTexts(std::vector<TextItem> pasted, int targetPage, Point center) {
    if (pasted.empty() || pasted.size() > 100000 || targetPage < 0 || targetPage >= pageCount() || current ||
        eraseGesture || selectionGesture || !std::isfinite(center.x) || !std::isfinite(center.y)) {
        return false;
    }

    SelectionBox bounds{targetPage, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto& item: pasted) {
        if (item.text.empty() || item.fontName.empty() || !validTextPage(item)) {
            return false;
        }
        for (const auto& point: textCorners(item)) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                return false;
            }
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
    }
    if (!std::isfinite(bounds.x1 - bounds.x0) || !std::isfinite(bounds.y1 - bounds.y0)) {
        return false;
    }

    const double dx = center.x - (bounds.x0 / 2.0 + bounds.x1 / 2.0);
    const double dy = center.y - (bounds.y0 / 2.0 + bounds.y1 / 2.0);
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return false;
    }

    const auto layerId = activeLayerId(targetPage);
    const auto originLayer = activeLayerOrigin(targetPage);
    auto layerOrder = nextOriginOrder(targetPage, originLayer);
    for (auto& item: pasted) {
        item.page = targetPage;
        item.layerId = layerId;
        item.originLayer = originLayer;
        item.layerOrder = layerOrder++;
        item.x += dx;
        item.y += dy;
        if (!validTextPage(item) || !std::isfinite(item.x) || !std::isfinite(item.y)) {
            return false;
        }
    }

    Edit edit;
    edit.textChanges.push_back({texts.size(), {}, texts.size(), std::move(pasted)});
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{};
    for (size_t index = 0; index < edit.textChanges.front().after.size(); ++index) {
        edit.textSelectionAfter->push_back(texts.size() + index);
    }
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{};
    edit.hasSelectionFrameState = true;
    edit.selectionFrameBefore = selectionFrame;
    apply(edit, true);
    record(std::move(edit));
    selectActivePage(targetPage);
    return true;
}

bool CanvasState::pasteObjects(std::vector<EditableObject> pasted, int targetPage, Point center) {
    constexpr size_t maxClipboardObjects = 100000;
    if (pasted.empty() || pasted.size() > maxClipboardObjects || targetPage < 0 || targetPage >= pageCount() ||
        current || eraseGesture || selectionGesture || !std::isfinite(center.x) || !std::isfinite(center.y)) {
        return false;
    }

    SelectionBox bounds{targetPage, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                        -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    const auto includePoint = [&](Point point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            return false;
        }
        bounds.x0 = std::min(bounds.x0, point.x);
        bounds.y0 = std::min(bounds.y0, point.y);
        bounds.x1 = std::max(bounds.x1, point.x);
        bounds.y1 = std::max(bounds.y1, point.y);
        return true;
    };

    for (const auto& object: pasted) {
        const bool valid = std::visit(
                [&](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, Ink>) {
                        if (item.points.empty() || !std::isfinite(item.brush.width) || item.brush.width <= 0 ||
                            isSelectionTool(item.brush.tool)) {
                            return false;
                        }
                        return std::all_of(item.points.begin(), item.points.end(), [&](const Point& point) {
                            return std::isfinite(point.pressure) && point.pressure > 0 && includePoint(point);
                        });
                    } else if constexpr (std::is_same_v<T, TextItem>) {
                        auto candidate = item;
                        candidate.page = targetPage;
                        if (candidate.text.empty() || candidate.fontName.empty() || !validTextPage(candidate)) {
                            return false;
                        }
                        const auto corners = textCorners(candidate);
                        return std::all_of(corners.begin(), corners.end(), includePoint);
                    } else {
                        auto candidate = item;
                        candidate.page = targetPage;
                        if (!validCanvasImage(candidate)) {
                            return false;
                        }
                        const auto corners = canvasImageCorners(candidate);
                        return std::all_of(corners.begin(), corners.end(), includePoint);
                    }
                },
                object);
        if (!valid) {
            return false;
        }
    }
    if (!std::isfinite(bounds.x1 - bounds.x0) || !std::isfinite(bounds.y1 - bounds.y0)) {
        return false;
    }

    const double dx = center.x - (bounds.x0 / 2.0 + bounds.x1 / 2.0);
    const double dy = center.y - (bounds.y0 / 2.0 + bounds.y1 / 2.0);
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return false;
    }

    const auto layerId = activeLayerId(targetPage);
    const auto originLayer = activeLayerOrigin(targetPage);
    auto layerOrder = nextOriginOrder(targetPage, originLayer);
    if (pasted.size() > std::numeric_limits<size_t>::max() - layerOrder) {
        return false;
    }

    std::vector<Ink> strokes;
    std::vector<TextItem> textItems;
    std::vector<CanvasImage> imageItems;
    for (auto& object: pasted) {
        const bool valid = std::visit(
                [&](auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    item.page = targetPage;
                    item.layerId = layerId;
                    item.originLayer = originLayer;
                    item.layerOrder = layerOrder++;
                    if constexpr (std::is_same_v<T, Ink>) {
                        for (auto& point: item.points) {
                            point.x += dx;
                            point.y += dy;
                            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                                return false;
                            }
                        }
                        strokes.push_back(std::move(item));
                    } else if constexpr (std::is_same_v<T, TextItem>) {
                        item.x += dx;
                        item.y += dy;
                        if (!std::isfinite(item.x) || !std::isfinite(item.y) || !validTextPage(item)) {
                            return false;
                        }
                        textItems.push_back(std::move(item));
                    } else {
                        item.transform.x += dx;
                        item.transform.y += dy;
                        if (!validCanvasImage(item)) {
                            return false;
                        }
                        imageItems.push_back(std::move(item));
                    }
                    return true;
                },
                object);
        if (!valid) {
            return false;
        }
    }

    Edit edit;
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{};
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{};
    if (!strokes.empty()) {
        edit.changes.push_back({ink.size(), {}, ink.size(), std::move(strokes)});
        for (size_t index = 0; index < edit.changes.back().after.size(); ++index) {
            edit.selectionAfter->push_back(ink.size() + index);
        }
    }
    if (!textItems.empty()) {
        edit.textChanges.push_back({texts.size(), {}, texts.size(), std::move(textItems)});
        for (size_t index = 0; index < edit.textChanges.back().after.size(); ++index) {
            edit.textSelectionAfter->push_back(texts.size() + index);
        }
    }
    if (!imageItems.empty()) {
        edit.imageChanges.push_back({images.size(), {}, images.size(), std::move(imageItems)});
        for (size_t index = 0; index < edit.imageChanges.back().after.size(); ++index) {
            edit.imageSelectionAfter->push_back(images.size() + index);
        }
    }
    edit.hasSelectionFrameState = true;
    edit.selectionFrameBefore = selectionFrame;
    apply(edit, true);
    record(std::move(edit));
    selectActivePage(targetPage);
    return true;
}

}  // namespace prototype
