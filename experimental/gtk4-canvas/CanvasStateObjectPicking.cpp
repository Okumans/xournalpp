#include "CanvasState.h"

namespace prototype {

std::optional<SelectableObject> CanvasState::pickSmartSelectObject(int pageIndex, Point point) const {
    return pickSelectableObject(pageIndex, point);
}

std::optional<size_t> CanvasState::pickStroke(int pageIndex, Point point) const {
    const size_t active = activeLayerId(pageIndex);
    if (!layerVisible(pageIndex, active)) {
        return std::nullopt;
    }
    for (size_t i = ink.size(); i-- > 0;) {
        const auto& stroke = ink[i];
        if (stroke.page != pageIndex || stroke.layerId != active)
            continue;
        for (size_t n = 0; n < stroke.points.size(); ++n) {
            const auto& a = stroke.points[n ? n - 1 : 0];
            const auto& b = stroke.points[n];
            const auto dx = b.x - a.x, dy = b.y - a.y;
            const auto length2 = dx * dx + dy * dy;
            const auto t = length2 ? std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length2, 0.0, 1.0) : 0;
            const auto pressure = stroke.pressureSensitive ? a.pressure + t * (b.pressure - a.pressure) : 1;
            if (std::hypot(point.x - a.x - t * dx, point.y - a.y - t * dy) <= .5 + stroke.brush.width * pressure / 2)
                return i;
        }
    }
    return std::nullopt;
}

std::optional<size_t> CanvasState::pickText(int pageIndex, Point point) const {
    const size_t active = activeLayerId(pageIndex);
    if (!layerVisible(pageIndex, active)) {
        return std::nullopt;
    }
    std::optional<size_t> best;
    size_t bestOrder = 0;
    for (size_t index = 0; index < texts.size(); ++index) {
        const auto& item = texts[index];
        if (item.page != pageIndex || item.layerId != active || item.text.empty()) {
            continue;
        }
        const double determinant = item.xx * item.yy - item.xy * item.yx;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
            continue;
        }
        const double dx = point.x - item.x;
        const double dy = point.y - item.y;
        const double localX = (item.yy * dx - item.xy * dy) / determinant;
        const double localY = (-item.yx * dx + item.xx * dy) / determinant;
        if (localX < 0 || localX > item.layoutWidth || localY < 0 || localY > item.layoutHeight) {
            continue;
        }
        if (!best || item.layerOrder >= bestOrder) {
            best = index;
            bestOrder = item.layerOrder;
        }
    }
    return best;
}

std::optional<size_t> CanvasState::pickLink(int pageIndex, Point point) const {
    const size_t active = activeLayerId(pageIndex);
    if (!layerVisible(pageIndex, active)) {
        return std::nullopt;
    }
    std::optional<size_t> first;
    size_t firstOrder = std::numeric_limits<size_t>::max();
    for (size_t index = 0; index < texts.size(); ++index) {
        const auto& item = texts[index];
        if (!item.isLink || item.page != pageIndex || item.layerId != active || item.text.empty() ||
            item.layerOrder >= firstOrder) {
            continue;
        }
        const double determinant = item.xx * item.yy - item.xy * item.yx;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
            continue;
        }
        const double dx = point.x - item.x;
        const double dy = point.y - item.y;
        const double localX = (item.yy * dx - item.xy * dy) / determinant;
        const double localY = (-item.yx * dx + item.xx * dy) / determinant;
        if (localX < 0 || localX > item.layoutWidth || localY < 0 || localY > item.layoutHeight) {
            continue;
        }
        first = index;
        firstOrder = item.layerOrder;
    }
    return first;
}

std::optional<size_t> CanvasState::pickImage(int pageIndex, Point point) const {
    const size_t active = activeLayerId(pageIndex);
    if (!layerVisible(pageIndex, active)) {
        return std::nullopt;
    }
    std::optional<size_t> best;
    size_t bestOrder = 0;
    for (size_t index = 0; index < images.size(); ++index) {
        const auto& image = images[index];
        if (image.page != pageIndex || image.layerId != active || !validCanvasImage(image)) {
            continue;
        }
        const auto bounds = canvasImageBounds(image);
        const auto& m = image.transform;
        const double determinant = m.xx * m.yy - m.xy * m.yx;
        const double dx = point.x - m.x;
        const double dy = point.y - m.y;
        const double localX = (m.yy * dx - m.xy * dy) / determinant;
        const double localY = (-m.yx * dx + m.xx * dy) / determinant;
        if (point.x < bounds.x0 || point.x > bounds.x1 || point.y < bounds.y0 || point.y > bounds.y1 || localX < 0 ||
            localX > canvasImageWidth(image) || localY < 0 || localY > canvasImageHeight(image)) {
            continue;
        }
        if (!best || image.layerOrder >= bestOrder) {
            best = index;
            bestOrder = image.layerOrder;
        }
    }
    return best;
}

bool CanvasState::selectObjectAt(int pageIndex, Point point, bool additive) {
    if (pageIndex < 0 || pageIndex >= pageCount() || !std::isfinite(point.x) || !std::isfinite(point.y)) {
        return false;
    }
    if (additive) {
        const auto bounds = selectionBox();
        if (bounds && bounds->page != pageIndex) {
            return false;
        }
    }

    const auto object = pickSelectableObject(pageIndex, point);
    if (!additive) {
        selected.clear();
        selectedTexts.clear();
        selectedImages.clear();
        selectionFrame.reset();
    }
    if (!object) {
        selectActivePage(pageIndex);
        return false;
    }
    if (additive) {
        selectionFrame.reset();
    }

    auto addIndex = [](std::vector<size_t>& indices, size_t index) {
        const auto position = std::lower_bound(indices.begin(), indices.end(), index);
        if (position == indices.end() || *position != index) {
            indices.insert(position, index);
        }
    };
    switch (object->kind) {
        case SelectableObjectKind::Stroke:
            addIndex(selected, object->index);
            break;
        case SelectableObjectKind::Text:
            addIndex(selectedTexts, object->index);
            break;
        case SelectableObjectKind::Image:
            addIndex(selectedImages, object->index);
            break;
    }
    selectActivePage(pageIndex);
    return true;
}

std::optional<SelectableObject> CanvasState::pickSelectableObject(int pageIndex, Point point) const {
    const size_t layerId = activeLayerId(pageIndex);
    if (!layerVisible(pageIndex, layerId) || layerId == 0) {
        return std::nullopt;
    }

    const auto& layer = layersForPage(pageIndex).layers.at(layerId - 1);
    std::unordered_map<size_t, size_t> originOrders;
    originOrders.reserve(layer.origins.size());
    for (size_t i = 0; i < layer.origins.size(); ++i) {
        originOrders.try_emplace(layer.origins[i], i);
    }
    const auto originOrder = [&](size_t origin) {
        const auto it = originOrders.find(origin);
        return it == originOrders.end() ? layer.origins.size() : it->second;
    };

    using ObjectOrder = std::tuple<size_t, size_t, int, size_t>;
    constexpr double actionRadius = 0.5;
    double minimumDistance = actionRadius;
    std::optional<SelectableObject> best;
    ObjectOrder bestOrder{};
    for (size_t i = 0; i < ink.size(); ++i) {
        const auto& stroke = ink[i];
        if (stroke.page != pageIndex || stroke.layerId != layerId || stroke.points.empty()) {
            continue;
        }

        const auto bounds = inkBounds(stroke);
        // shortcut: this still scans points; cache bounds if profiling justifies invalidation work.
        if (bounds && (point.x < bounds->x0 - actionRadius || point.x > bounds->x1 + actionRadius ||
                       point.y < bounds->y0 - actionRadius || point.y > bounds->y1 + actionRadius)) {
            continue;
        }

        const double distance = strokeDistance(stroke, point);
        const ObjectOrder order{originOrder(stroke.originLayer), stroke.layerOrder,
                                static_cast<int>(SelectableObjectKind::Stroke), i};
        if (distance < minimumDistance || (best && distance == minimumDistance && order > bestOrder)) {
            minimumDistance = distance;
            best = SelectableObject{SelectableObjectKind::Stroke, i};
            bestOrder = order;
        }
    }

    const auto consider = [&](SelectableObjectKind kind, size_t index, size_t origin, size_t layerOrder,
                              double distance) {
        const ObjectOrder order{originOrder(origin), layerOrder, static_cast<int>(kind), index};
        if (distance < minimumDistance || (best && distance == minimumDistance && order > bestOrder)) {
            minimumDistance = distance;
            best = SelectableObject{kind, index};
            bestOrder = order;
        }
    };
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (image.page == pageIndex && image.layerId == layerId && validCanvasImage(image)) {
            consider(SelectableObjectKind::Image, i, image.originLayer, image.layerOrder,
                     quadrilateralDistance(canvasImageCorners(image), point));
        }
    }

    for (size_t i = 0; i < texts.size(); ++i) {
        const auto& text = texts[i];
        if (text.page == pageIndex && text.layerId == layerId && !text.text.empty() && validTextPage(text)) {
            consider(SelectableObjectKind::Text, i, text.originLayer, text.layerOrder,
                     quadrilateralDistance(textCorners(text), point));
        }
    }
    return best;
}

}  // namespace prototype
