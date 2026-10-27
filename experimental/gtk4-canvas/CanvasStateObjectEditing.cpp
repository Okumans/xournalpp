#include "CanvasState.h"

namespace prototype {

bool CanvasState::setSelectedInkFill(int penOpacity, int highlighterOpacity) {
    if (penOpacity < -1 || penOpacity > 255 || highlighterOpacity < -1 || highlighterOpacity > 255) {
        return false;
    }
    std::vector<std::pair<size_t, Ink>> replacements;
    for (const size_t index: selected) {
        if (index >= ink.size() || !isFillableTool(ink[index].brush.tool)) {
            continue;
        }
        auto replacement = ink[index];
        replacement.fill = replacement.brush.tool == Tool::Pen ? penOpacity : highlighterOpacity;
        if (replacement.fill != ink[index].fill) {
            replacements.emplace_back(index, std::move(replacement));
        }
    }
    return applyInkEdits(std::move(replacements), {});
}

bool CanvasState::setSelectedInkLineStyle(const LineDashPattern& lineDashes) {
    if (!validLineDashPattern(lineDashes)) {
        return false;
    }
    std::vector<std::pair<size_t, Ink>> replacements;
    for (const size_t index: selected) {
        if (index >= ink.size() || ink[index].brush.tool != Tool::Pen || ink[index].brush.lineDashes == lineDashes) {
            continue;
        }
        auto replacement = ink[index];
        replacement.brush.lineDashes = lineDashes;
        replacements.emplace_back(index, std::move(replacement));
    }
    return applyInkEdits(std::move(replacements), {});
}

bool CanvasState::applyInkEdits(std::vector<std::pair<size_t, Ink>> replacements, std::vector<Ink> additions) {
    if (current || eraseGesture || selectionGesture || (replacements.empty() && additions.empty())) {
        return false;
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (size_t i = 0; i < replacements.size(); ++i) {
        const auto index = replacements[i].first;
        if (index >= ink.size() || (i && replacements[i - 1].first == index)) {
            return false;
        }
        auto& replacement = replacements[i].second;
        const auto& original = ink[index];
        if (replacement.page != original.page || replacement.layerId != original.layerId ||
            replacement.originLayer != original.originLayer || replacement.layerOrder != original.layerOrder ||
            replacement.points.size() != original.points.size() || replacement.brush.tool != original.brush.tool ||
            replacement.brush.width != original.brush.width || replacement.fill < -1 || replacement.fill > 255 ||
            (replacement.fill >= 0 && !isFillableTool(replacement.brush.tool)) ||
            replacement.capStyle != original.capStyle || !validLineDashPattern(replacement.brush.lineDashes)) {
            return false;
        }
    }

    std::map<std::pair<int, size_t>, size_t> nextOrders;
    for (auto& addition: additions) {
        if (addition.page < 0 || addition.page >= pageCount() || addition.points.size() < 2 ||
            !std::isfinite(addition.brush.width) || addition.brush.width <= 0 || addition.fill < -1 ||
            addition.fill > 255 || addition.capStyle < 0 || addition.capStyle > 2 ||
            (addition.fill >= 0 && !isFillableTool(addition.brush.tool)) ||
            !validLineDashPattern(addition.brush.lineDashes)) {
            return false;
        }
        for (const auto& point: addition.points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.pressure) ||
                point.pressure <= 0) {
                return false;
            }
        }
        for (const auto channel: addition.brush.color) {
            if (!std::isfinite(channel) || channel < 0 || channel > 1) {
                return false;
            }
        }
        addition.layerId = activeLayerId(addition.page);
        addition.originLayer = activeLayerOrigin(addition.page);
        const auto key = std::pair{addition.page, addition.originLayer};
        auto order = nextOrders.try_emplace(key, nextOriginOrder(addition.page, addition.originLayer)).first;
        addition.layerOrder = order->second++;
    }

    Edit edit;
    edit.selectionBefore = edit.selectionAfter = selected;
    edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
    edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
    edit.changes.reserve(replacements.size() + !additions.empty());
    for (auto& [index, replacement]: replacements) {
        edit.changes.push_back({index, {ink[index]}, index, {std::move(replacement)}});
    }
    if (!additions.empty()) {
        edit.changes.push_back({ink.size(), {}, ink.size(), std::move(additions)});
    }
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::insertText(TextItem item) {
    if (!validTextPage(item) || current || eraseGesture || selectionGesture) {
        return false;
    }
    item.layerId = activeLayerId(item.page);
    item.originLayer = activeLayerOrigin(item.page);
    item.layerOrder = nextOriginOrder(item.page, item.originLayer);
    Edit edit;
    const auto index = texts.size();
    edit.textChanges.push_back({index, {}, index, {std::move(item)}});
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{index};
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{};
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::replaceText(size_t index, TextItem item) {
    if (index >= texts.size() || !validTextPage(item) || current || eraseGesture || selectionGesture ||
        item.page != texts[index].page) {
        return false;
    }
    item.layerOrder = texts[index].layerOrder;
    item.layerId = texts[index].layerId;
    item.originLayer = texts[index].originLayer;
    item.pluginReference = item.isLink == texts[index].isLink ? texts[index].pluginReference : 0;
    Edit edit;
    edit.textChanges.push_back({index, {texts[index]}, index, {std::move(item)}});
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{index};
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{};
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::replaceTextWithImage(size_t index, CanvasImage image) {
    if (index >= texts.size() || !validCanvasImage(image) || current || eraseGesture || selectionGesture ||
        image.page != texts[index].page) {
        return false;
    }

    const auto& source = texts[index];
    image.layerOrder = source.layerOrder;
    image.layerId = source.layerId;
    image.originLayer = source.originLayer;

    Edit edit;
    edit.textChanges.push_back({index, {source}, index, {}});
    edit.imageChanges.push_back({images.size(), {}, images.size(), {std::move(image)}});
    edit.selectionBefore = edit.selectionAfter = selected;
    edit.textSelectionBefore = selectedTexts;
    auto remainingTexts = selectedTexts;
    std::erase(remainingTexts, index);
    for (auto& selectedIndex: remainingTexts) {
        if (selectedIndex > index) {
            --selectedIndex;
        }
    }
    edit.textSelectionAfter = std::move(remainingTexts);
    edit.imageSelectionBefore = selectedImages;
    auto selectedAfter = selectedImages;
    selectedAfter.push_back(images.size());
    edit.imageSelectionAfter = std::move(selectedAfter);

    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::recolorSelectedTexts(uint32_t color) {
    return updateSelectedTexts([color](TextItem& item) {
        item.color = color;
        for (auto& run: item.styleRuns) {
            run.color = color;
        }
    });
}

bool CanvasState::recolorSelection(const std::array<double, 3>& color, uint32_t textColor) {
    if (current || eraseGesture || selectionGesture || !hasSelection()) {
        return false;
    }
    for (const auto channel: color) {
        if (!std::isfinite(channel) || channel < 0 || channel > 1) {
            return false;
        }
    }

    Edit edit;
    edit.selectionBefore = edit.selectionAfter = selected;
    edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
    edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
    auto inkIndices = selected;
    std::sort(inkIndices.begin(), inkIndices.end());
    inkIndices.erase(std::unique(inkIndices.begin(), inkIndices.end()), inkIndices.end());
    for (const auto index: inkIndices) {
        if (index >= ink.size()) {
            return false;
        }
        auto replacement = ink[index];
        replacement.brush.color = color;
        if (replacement.brush.color != ink[index].brush.color) {
            edit.changes.push_back({index, {ink[index]}, index, {std::move(replacement)}});
        }
    }

    auto textIndices = selectedTexts;
    std::sort(textIndices.begin(), textIndices.end());
    textIndices.erase(std::unique(textIndices.begin(), textIndices.end()), textIndices.end());
    for (const auto index: textIndices) {
        if (index >= texts.size()) {
            return false;
        }
        auto replacement = texts[index];
        replacement.color = textColor;
        for (auto& run: replacement.styleRuns) {
            run.color = textColor;
        }
        if (replacement != texts[index]) {
            edit.textChanges.push_back({index, {texts[index]}, index, {std::move(replacement)}});
        }
    }

    if (edit.changes.empty() && edit.textChanges.empty()) {
        return false;
    }
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::deleteText(size_t index) {
    if (index >= texts.size() || current || eraseGesture || selectionGesture) {
        return false;
    }
    Edit edit;
    edit.textChanges.push_back({index, {texts[index]}, index, {}});
    edit.selectionBefore = selected;
    edit.selectionAfter = selected;
    edit.textSelectionBefore = selectedTexts;
    auto remainingTexts = selectedTexts;
    std::erase(remainingTexts, index);
    for (auto& selectedIndex: remainingTexts) {
        if (selectedIndex > index) {
            --selectedIndex;
        }
    }
    edit.textSelectionAfter = std::move(remainingTexts);
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = selectedImages;
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::insertImage(CanvasImage image) {
    if (!validCanvasImage(image) || image.page >= pageCount() || current || eraseGesture || selectionGesture) {
        return false;
    }
    image.layerId = activeLayerId(image.page);
    image.originLayer = activeLayerOrigin(image.page);
    image.layerOrder = nextOriginOrder(image.page, image.originLayer);
    Edit edit;
    edit.imageChanges.push_back({images.size(), {}, images.size(), {image}});
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{};
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{images.size()};
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::pasteImages(std::vector<CanvasImage> pasted, int targetPage, Point center) {
    if (pasted.empty() || pasted.size() > maxCanvasImageObjects || targetPage < 0 || targetPage >= pageCount() ||
        current || eraseGesture || selectionGesture || !std::isfinite(center.x) || !std::isfinite(center.y)) {
        return false;
    }

    auto bounds = canvasImageBounds(pasted.front());
    if (!validCanvasImage(pasted.front())) {
        return false;
    }
    for (const auto& image: pasted) {
        if (!validCanvasImage(image)) {
            return false;
        }
        for (const auto& point: canvasImageCorners(image)) {
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
    for (auto& image: pasted) {
        image.page = targetPage;
        image.layerId = layerId;
        image.originLayer = originLayer;
        image.layerOrder = layerOrder++;
        image.transform.x += dx;
        image.transform.y += dy;
        if (!validCanvasImage(image) || !std::isfinite(image.transform.x) || !std::isfinite(image.transform.y)) {
            return false;
        }
    }

    Edit edit;
    edit.imageChanges.push_back({images.size(), {}, images.size(), std::move(pasted)});
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{};
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{};
    for (size_t index = 0; index < edit.imageChanges.front().after.size(); ++index) {
        edit.imageSelectionAfter->push_back(images.size() + index);
    }
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::replaceImage(size_t index, CanvasImage image) {
    if (index >= images.size() || !validCanvasImage(image) || current || eraseGesture || selectionGesture ||
        image.page != images[index].page) {
        return false;
    }
    image.layerOrder = images[index].layerOrder;
    image.layerId = images[index].layerId;
    image.originLayer = images[index].originLayer;
    image.pluginReference = images[index].pluginReference;
    Edit edit;
    edit.imageChanges.push_back({index, {images[index]}, index, {std::move(image)}});
    edit.selectionBefore = edit.selectionAfter = selected;
    edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
    edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::replaceImageGroup(std::vector<std::pair<size_t, std::vector<CanvasImage>>> replacements) {
    if (replacements.empty() || current || eraseGesture || selectionGesture ||
        replacements.size() != selectedImages.size()) {
        return false;
    }
    std::sort(replacements.begin(), replacements.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    auto selectedIndices = selectedImages;
    std::sort(selectedIndices.begin(), selectedIndices.end());
    selectedIndices.erase(std::unique(selectedIndices.begin(), selectedIndices.end()), selectedIndices.end());

    const auto sameImage = [](const CanvasImage& lhs, const CanvasImage& rhs) {
        return lhs.page == rhs.page && lhs.layerOrder == rhs.layerOrder && lhs.layerId == rhs.layerId &&
               lhs.originLayer == rhs.originLayer && lhs.pixelWidth == rhs.pixelWidth &&
               lhs.pixelHeight == rhs.pixelHeight && lhs.kind == rhs.kind && lhs.nativeWidth == rhs.nativeWidth &&
               lhs.nativeHeight == rhs.nativeHeight && lhs.encodedData == rhs.encodedData &&
               lhs.latexSource == rhs.latexSource && lhs.transform.xx == rhs.transform.xx &&
               lhs.transform.yx == rhs.transform.yx && lhs.transform.xy == rhs.transform.xy &&
               lhs.transform.yy == rhs.transform.yy && lhs.transform.x == rhs.transform.x &&
               lhs.transform.y == rhs.transform.y;
    };

    size_t resultCount = images.size();
    for (size_t index = 0; index < replacements.size(); ++index) {
        auto& [sourceIndex, outputs] = replacements[index];
        if (sourceIndex >= images.size() || outputs.empty() ||
            !std::binary_search(selectedIndices.begin(), selectedIndices.end(), sourceIndex) ||
            (index > 0 && replacements[index - 1].first == sourceIndex)) {
            return false;
        }
        if (outputs.size() > maxCanvasImageObjects || resultCount > maxCanvasImageObjects - (outputs.size() - 1)) {
            return false;
        }
        resultCount += outputs.size() - 1;
        const auto& original = images[sourceIndex];
        for (auto& output: outputs) {
            if (!validCanvasImage(output) || output.page != original.page) {
                return false;
            }
            output.layerId = original.layerId;
            output.originLayer = original.originLayer;
        }
    }

    std::map<std::pair<int, size_t>, size_t> nextOrders;
    for (auto& [sourceIndex, outputs]: replacements) {
        const auto key = std::pair{images[sourceIndex].page, images[sourceIndex].originLayer};
        auto order = nextOrders.try_emplace(key, nextOriginOrder(key.first, key.second)).first;
        for (auto& output: outputs) {
            output.layerOrder = order->second++;
        }
    }

    Edit edit;
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{};
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{};
    std::ptrdiff_t indexDelta = 0;
    for (auto& [sourceIndex, outputs]: replacements) {
        const bool unchanged = outputs.size() == 1 && sameImage(outputs.front(), images[sourceIndex]);
        const auto finalIndex = static_cast<size_t>(static_cast<std::ptrdiff_t>(sourceIndex) + indexDelta);
        if (unchanged) {
            edit.imageSelectionAfter->push_back(finalIndex);
            continue;
        }
        edit.imageChanges.push_back({sourceIndex, {images[sourceIndex]}, finalIndex, std::move(outputs)});
        const auto& inserted = edit.imageChanges.back().after;
        for (size_t offset = 0; offset < inserted.size(); ++offset) {
            edit.imageSelectionAfter->push_back(finalIndex + offset);
        }
        indexDelta += static_cast<std::ptrdiff_t>(inserted.size()) - 1;
    }
    if (edit.imageChanges.empty()) {
        return true;
    }
    apply(edit, true);
    record(std::move(edit));
    return true;
}

bool CanvasState::deleteImage(size_t index) {
    if (index >= images.size() || current || eraseGesture || selectionGesture) {
        return false;
    }
    Edit edit;
    edit.imageChanges.push_back({index, {images[index]}, index, {}});
    edit.selectionBefore = edit.selectionAfter = selected;
    edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
    edit.imageSelectionBefore = selectedImages;
    auto remainingImages = selectedImages;
    std::erase(remainingImages, index);
    for (auto& selectedIndex: remainingImages) {
        if (selectedIndex > index) {
            --selectedIndex;
        }
    }
    edit.imageSelectionAfter = std::move(remainingImages);
    apply(edit, true);
    record(std::move(edit));
    return true;
}

}  // namespace prototype
