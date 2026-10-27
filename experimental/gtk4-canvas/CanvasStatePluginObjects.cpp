#include "CanvasState.h"

namespace prototype {

void CanvasState::ensurePluginReferences() {
    std::uintptr_t greatestReference = 0;
    const auto noteReferences = [&greatestReference](const auto& objects) {
        for (const auto& object: objects) {
            greatestReference = std::max(greatestReference, object.pluginReference);
        }
    };
    noteReferences(ink);
    noteReferences(texts);
    noteReferences(images);
    if (greatestReference >= nextPluginReference) {
        nextPluginReference =
                greatestReference == std::numeric_limits<std::uintptr_t>::max() ? 0 : greatestReference + 1;
    }

    std::unordered_set<std::uintptr_t> references;
    const auto ensureUnique = [this, &references](auto& objects) {
        for (auto& object: objects) {
            if (object.pluginReference == 0 || !references.insert(object.pluginReference).second) {
                object.pluginReference = allocatePluginReference();
                references.insert(object.pluginReference);
            }
        }
    };
    ensureUnique(ink);
    ensureUnique(texts);
    ensureUnique(images);
}

void CanvasState::addPluginReferencesToSelection(const std::vector<std::uintptr_t>& requestedReferences) {
    if (requestedReferences.empty()) {
        return;
    }
    ensurePluginReferences();
    cancelSelection();
    cancelVerticalSpace();

    const std::unordered_set<std::uintptr_t> references(requestedReferences.begin(), requestedReferences.end());
    const size_t layerId = activeLayerId(page);
    const auto addMatches = [this, &references, layerId](const auto& objects, auto& selection) {
        std::erase_if(selection, [&objects](size_t index) { return index >= objects.size(); });
        for (size_t index = 0; index < objects.size(); ++index) {
            const auto& object = objects[index];
            if (object.page == page && object.layerId == layerId && references.contains(object.pluginReference)) {
                selection.push_back(index);
            }
        }
        std::sort(selection.begin(), selection.end(), [&objects](size_t lhs, size_t rhs) {
            if (objects[lhs].layerOrder != objects[rhs].layerOrder) {
                return objects[lhs].layerOrder < objects[rhs].layerOrder;
            }
            return lhs < rhs;
        });
        selection.erase(std::unique(selection.begin(), selection.end()), selection.end());
    };
    addMatches(ink, selected);
    addMatches(texts, selectedTexts);
    addMatches(images, selectedImages);
    selectionFrame.reset();
}

bool CanvasState::addPluginStrokes(std::vector<Ink> additions, int undoPolicy) {
    if (additions.empty()) {
        return true;
    }
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size() || undoPolicy < 0 || undoPolicy > 2) {
        return false;
    }

    ensurePluginReferences();
    const auto pageIndex = page;
    const auto layerId = activeLayerId(pageIndex);
    const auto origin = activeLayerOrigin(pageIndex);
    size_t nextOrder = nextOriginOrder(pageIndex, origin);
    for (auto& stroke: additions) {
        if (stroke.points.size() < 2 || !std::isfinite(stroke.brush.width) || stroke.brush.width <= 0 ||
            stroke.fill < -1 || stroke.fill > 255 || (stroke.fill >= 0 && !isFillableTool(stroke.brush.tool))) {
            return false;
        }
        for (const auto& point: stroke.points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.pressure) ||
                point.pressure <= 0) {
                return false;
            }
        }
        stroke.page = pageIndex;
        stroke.layerId = layerId;
        stroke.originLayer = origin;
        stroke.layerOrder = nextOrder++;
        stroke.pluginReference = allocatePluginReference();
    }

    const auto preserveSelection = [this](Edit& edit) {
        edit.selectionBefore = selected;
        edit.selectionAfter = selected;
        edit.textSelectionBefore = selectedTexts;
        edit.textSelectionAfter = selectedTexts;
        edit.imageSelectionBefore = selectedImages;
        edit.imageSelectionAfter = selectedImages;
        edit.hasSelectionFrameState = true;
        edit.selectionFrameBefore = selectionFrame;
        edit.selectionFrameAfter = selectionFrame;
    };

    if (undoPolicy == 2) {
        for (auto& stroke: additions) {
            stroke.rasterCacheKey = 0;
            ink.push_back(std::move(stroke));
        }
        // These strokes are intentionally irreversible. Drop older history
        // so later undo operations cannot apply stale object indices, then
        // keep the mutation visible to dirty-state and autosave tracking.
        undoStates.clear();
        redoStates.clear();
        revisionBase = ++nextRevision;
        return true;
    }
    if (undoPolicy == 0) {
        Edit edit;
        edit.changes.push_back({ink.size(), {}, ink.size(), std::move(additions)});
        preserveSelection(edit);
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    for (auto& stroke: additions) {
        Edit edit;
        edit.changes.push_back({ink.size(), {}, ink.size(), {std::move(stroke)}});
        preserveSelection(edit);
        apply(edit, true);
        record(std::move(edit));
    }
    return true;
}

bool CanvasState::addPluginTexts(std::vector<TextItem> additions, int undoPolicy) {
    if (additions.empty()) {
        return true;
    }
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size() || undoPolicy < 0 || undoPolicy > 2 || current ||
        eraseGesture || selectionGesture) {
        return false;
    }

    ensurePluginReferences();
    const auto pageIndex = page;
    const auto layerId = activeLayerId(pageIndex);
    const auto origin = activeLayerOrigin(pageIndex);
    size_t nextOrder = nextOriginOrder(pageIndex, origin);
    for (auto& item: additions) {
        item.page = pageIndex;
        item.layerId = layerId;
        item.originLayer = origin;
        item.layerOrder = nextOrder++;
        item.pluginReference = allocatePluginReference();
        if (!validTextPage(item) || item.alignment < 0 || item.alignment > 2) {
            return false;
        }
    }

    const auto preserveSelection = [this](Edit& edit) {
        edit.selectionBefore = edit.selectionAfter = selected;
        edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
        edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
        edit.hasSelectionFrameState = true;
        edit.selectionFrameBefore = selectionFrame;
        edit.selectionFrameAfter = selectionFrame;
    };

    if (undoPolicy == 2) {
        for (auto& item: additions) {
            texts.push_back(std::move(item));
        }
        // Non-undoable additions form a dirty revision boundary so older
        // edits cannot replay against changed text indices.
        undoStates.clear();
        redoStates.clear();
        revisionBase = ++nextRevision;
        return true;
    }
    if (undoPolicy == 0) {
        Edit edit;
        edit.textChanges.push_back({texts.size(), {}, texts.size(), std::move(additions)});
        preserveSelection(edit);
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    for (auto& item: additions) {
        Edit edit;
        edit.textChanges.push_back({texts.size(), {}, texts.size(), {std::move(item)}});
        preserveSelection(edit);
        apply(edit, true);
        record(std::move(edit));
    }
    return true;
}

bool CanvasState::addPluginImages(std::vector<CanvasImage> additions, int undoPolicy) {
    if (additions.empty()) {
        return true;
    }
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size() || undoPolicy < 0 || undoPolicy > 2 || current ||
        eraseGesture || selectionGesture || images.size() > maxCanvasImageObjects ||
        additions.size() > maxCanvasImageObjects - images.size()) {
        return false;
    }

    ensurePluginReferences();
    const auto pageIndex = page;
    const auto layerId = activeLayerId(pageIndex);
    const auto origin = activeLayerOrigin(pageIndex);
    size_t nextOrder = nextOriginOrder(pageIndex, origin);
    for (auto& image: additions) {
        image.page = pageIndex;
        image.layerId = layerId;
        image.originLayer = origin;
        image.layerOrder = nextOrder++;
        image.pluginReference = allocatePluginReference();
        if (!validCanvasImage(image)) {
            return false;
        }
    }

    const size_t first = images.size();
    const auto newSelection = [first](size_t count) {
        std::vector<size_t> selection;
        selection.reserve(count);
        for (size_t index = first; index < first + count; ++index) {
            selection.push_back(index);
        }
        return selection;
    };
    const auto selectNewImages = [this, &newSelection](size_t count) {
        selected.clear();
        selectedTexts.clear();
        selectedImages = newSelection(count);
        selectionFrame.reset();
    };

    if (undoPolicy == 2) {
        for (auto& image: additions) {
            images.push_back(std::move(image));
        }
        selectNewImages(additions.size());
        // Non-undoable image batches form a dirty revision boundary so
        // older edits cannot replay against changed image indices.
        undoStates.clear();
        redoStates.clear();
        revisionBase = ++nextRevision;
        return true;
    }
    if (undoPolicy == 0) {
        Edit edit;
        edit.imageChanges.push_back({first, {}, first, std::move(additions)});
        edit.selectionBefore = selected;
        edit.selectionAfter = std::vector<size_t>{};
        edit.textSelectionBefore = selectedTexts;
        edit.textSelectionAfter = std::vector<size_t>{};
        edit.imageSelectionBefore = selectedImages;
        edit.imageSelectionAfter = newSelection(edit.imageChanges.front().after.size());
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    size_t insertedCount = 0;
    for (auto& image: additions) {
        Edit edit;
        edit.imageChanges.push_back({images.size(), {}, images.size(), {std::move(image)}});
        edit.selectionBefore = selected;
        edit.selectionAfter = std::vector<size_t>{};
        edit.textSelectionBefore = selectedTexts;
        edit.textSelectionAfter = std::vector<size_t>{};
        edit.imageSelectionBefore = selectedImages;
        edit.imageSelectionAfter = newSelection(++insertedCount);
        apply(edit, true);
        record(std::move(edit));
    }
    return true;
}

}  // namespace prototype
