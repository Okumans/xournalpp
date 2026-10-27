#include "CanvasState.h"

namespace prototype {

bool CanvasState::selectLayer(int pageIndex, size_t layerId) {
    if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLayers.size() || layerId == 0 ||
        layerId > pageLayers[static_cast<size_t>(pageIndex)].layers.size()) {
        return false;
    }
    cancel();
    clearSelection();
    pageLayers[static_cast<size_t>(pageIndex)].selectedLayer = layerId;
    return true;
}

bool CanvasState::setLayerVisible(int pageIndex, size_t layerId, bool visible) {
    if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLayers.size()) {
        return false;
    }
    auto before = captureLayerMutation(pageIndex);
    auto& pageState = pageLayers[static_cast<size_t>(pageIndex)];
    if (layerId == 0) {
        if (pageState.backgroundVisible == visible) {
            return false;
        }
        pageState.backgroundVisible = visible;
    } else {
        if (layerId > pageState.layers.size() || pageState.layers[layerId - 1].visible == visible) {
            return false;
        }
        pageState.layers[layerId - 1].visible = visible;
    }
    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::renameLayer(int pageIndex, size_t layerId, const std::string& name) {
    if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLayers.size()) {
        return false;
    }
    auto before = captureLayerMutation(pageIndex);
    auto& pageState = pageLayers[static_cast<size_t>(pageIndex)];
    if (layerId == 0) {
        if (pageState.backgroundName && *pageState.backgroundName == name) {
            return false;
        }
        pageState.backgroundName = name;
    } else {
        if (layerId > pageState.layers.size() || pageState.layers[layerId - 1].name == name) {
            return false;
        }
        pageState.layers[layerId - 1].name = name;
    }
    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::setAllLayersVisible(int pageIndex, bool visible) {
    if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLayers.size()) {
        return false;
    }
    auto before = captureLayerMutation(pageIndex);
    auto& pageState = pageLayers[static_cast<size_t>(pageIndex)];
    const bool changed = std::any_of(pageState.layers.begin(), pageState.layers.end(),
                                     [visible](const CanvasLayer& layer) { return layer.visible != visible; });
    if (!changed) {
        return false;
    }
    for (auto& layer: pageState.layers) {
        layer.visible = visible;
    }
    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::addLayerAboveCurrent() {
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size()) {
        return false;
    }
    const size_t pageIndex = static_cast<size_t>(page);
    auto& pageState = pageLayers[pageIndex];
    const size_t selectedLayer = pageState.selectedLayer;
    if (pageState.layers.size() >= 1000 || selectedLayer == 0 || selectedLayer > pageState.layers.size()) {
        return false;
    }
    auto before = captureLayerMutation(page);
    const size_t insertion = selectedLayer;
    const size_t origin = nextLayerOrigin(pageState);
    pageState.layers.insert(pageState.layers.begin() + static_cast<std::ptrdiff_t>(insertion),
                            CanvasLayer{"Layer " + std::to_string(pageState.layers.size() + 1), true, {origin}});
    pageState.selectedLayer = insertion + 1;
    for (auto& stroke: ink) {
        if (stroke.page == page && stroke.layerId >= pageState.selectedLayer) {
            ++stroke.layerId;
        }
    }
    for (auto& text: texts) {
        if (text.page == page && text.layerId >= pageState.selectedLayer) {
            ++text.layerId;
        }
    }
    for (auto& image: images) {
        if (image.page == page && image.layerId >= pageState.selectedLayer) {
            ++image.layerId;
        }
    }
    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::addLayerBelowCurrent() {
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size()) {
        return false;
    }
    const size_t pageIndex = static_cast<size_t>(page);
    auto& pageState = pageLayers[pageIndex];
    const size_t selectedLayer = pageState.selectedLayer;
    if (pageState.layers.size() >= 1000 || selectedLayer == 0 || selectedLayer > pageState.layers.size()) {
        return false;
    }

    auto before = captureLayerMutation(page);
    const size_t insertion = selectedLayer - 1;
    const size_t origin = nextLayerOrigin(pageState);
    pageState.layers.insert(pageState.layers.begin() + static_cast<std::ptrdiff_t>(insertion),
                            CanvasLayer{"Layer " + std::to_string(pageState.layers.size() + 1), true, {origin}});
    pageState.selectedLayer = selectedLayer;
    for (auto& stroke: ink) {
        if (stroke.page == page && stroke.layerId >= selectedLayer) {
            ++stroke.layerId;
        }
    }
    for (auto& text: texts) {
        if (text.page == page && text.layerId >= selectedLayer) {
            ++text.layerId;
        }
    }
    for (auto& image: images) {
        if (image.page == page && image.layerId >= selectedLayer) {
            ++image.layerId;
        }
    }

    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::duplicateCurrentLayer() {
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size()) {
        return false;
    }
    const size_t pageIndex = static_cast<size_t>(page);
    auto before = captureLayerMutation(page);
    auto& pageState = pageLayers[pageIndex];
    const size_t sourceId = pageState.selectedLayer;
    if (sourceId == 0 || sourceId > pageState.layers.size() || pageState.layers.size() >= 1000) {
        return false;
    }

    const CanvasLayer duplicate = pageState.layers[sourceId - 1];
    pageState.layers.insert(pageState.layers.begin() + static_cast<std::ptrdiff_t>(sourceId), duplicate);
    pageState.selectedLayer = sourceId + 1;

    std::vector<Ink> duplicateInk;
    std::vector<TextItem> duplicateTexts;
    std::vector<CanvasImage> duplicateImages;
    for (const auto& stroke: ink) {
        if (stroke.page == page && stroke.layerId == sourceId) {
            auto copy = stroke;
            copy.layerId = sourceId + 1;
            duplicateInk.push_back(std::move(copy));
        }
    }
    for (const auto& text: texts) {
        if (text.page == page && text.layerId == sourceId) {
            auto copy = text;
            copy.layerId = sourceId + 1;
            duplicateTexts.push_back(std::move(copy));
        }
    }
    for (const auto& image: images) {
        if (image.page == page && image.layerId == sourceId) {
            auto copy = image;
            copy.layerId = sourceId + 1;
            duplicateImages.push_back(std::move(copy));
        }
    }
    for (auto& stroke: ink) {
        if (stroke.page == page && stroke.layerId > sourceId) {
            ++stroke.layerId;
        }
    }
    for (auto& text: texts) {
        if (text.page == page && text.layerId > sourceId) {
            ++text.layerId;
        }
    }
    for (auto& image: images) {
        if (image.page == page && image.layerId > sourceId) {
            ++image.layerId;
        }
    }
    ink.insert(ink.end(), std::make_move_iterator(duplicateInk.begin()), std::make_move_iterator(duplicateInk.end()));
    texts.insert(texts.end(), std::make_move_iterator(duplicateTexts.begin()),
                 std::make_move_iterator(duplicateTexts.end()));
    images.insert(images.end(), std::make_move_iterator(duplicateImages.begin()),
                  std::make_move_iterator(duplicateImages.end()));

    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::moveCurrentLayer(bool up) {
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size()) {
        return false;
    }
    const size_t pageIndex = static_cast<size_t>(page);
    auto before = captureLayerMutation(page);
    auto& pageState = pageLayers[pageIndex];
    const size_t currentId = pageState.selectedLayer;
    if (currentId == 0 || currentId > pageState.layers.size() || (up && currentId == pageState.layers.size()) ||
        (!up && currentId == 1)) {
        return false;
    }

    const size_t neighborId = up ? currentId + 1 : currentId - 1;
    std::swap(pageState.layers[currentId - 1], pageState.layers[neighborId - 1]);
    pageState.selectedLayer = neighborId;
    const auto remap = [this, currentId, neighborId](auto& objects) {
        for (auto& object: objects) {
            if (object.page != page) {
                continue;
            }
            if (object.layerId == currentId) {
                object.layerId = neighborId;
            } else if (object.layerId == neighborId) {
                object.layerId = currentId;
            }
        }
    };
    remap(ink);
    remap(texts);
    remap(images);

    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::deleteCurrentLayer() {
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size()) {
        return false;
    }
    const size_t pageIndex = static_cast<size_t>(page);
    auto before = captureLayerMutation(page);
    auto& pageState = pageLayers[pageIndex];
    const size_t deletedId = pageState.selectedLayer;
    const size_t oldCount = pageState.layers.size();
    if (deletedId == 0 || deletedId > oldCount) {
        return false;
    }
    pageState.layers.erase(pageState.layers.begin() + static_cast<std::ptrdiff_t>(deletedId - 1));
    if (pageState.layers.empty()) {
        pageState.layers.push_back({"Layer 1", true, {nextLayerOrigin(pageState)}});
    }
    pageState.selectedLayer = std::min(deletedId, pageState.layers.size());
    remapObjectsAfterLayerRemoval(page, deletedId);
    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

bool CanvasState::mergeCurrentLayerDown() {
    if (page < 0 || static_cast<size_t>(page) >= pageLayers.size()) {
        return false;
    }
    const size_t pageIndex = static_cast<size_t>(page);
    auto before = captureLayerMutation(page);
    auto& pageState = pageLayers[pageIndex];
    const size_t topId = pageState.selectedLayer;
    if (topId <= 1 || topId > pageState.layers.size()) {
        return false;
    }
    auto& below = pageState.layers[topId - 2];
    auto& top = pageState.layers[topId - 1];
    below.origins.insert(below.origins.end(), top.origins.begin(), top.origins.end());
    below.visible = below.visible || top.visible;
    if (below.name.empty() || below.name.rfind("Layer ", 0) == 0) {
        below.name = top.name;
    }
    pageState.layers.erase(pageState.layers.begin() + static_cast<std::ptrdiff_t>(topId - 1));
    pageState.selectedLayer = topId - 1;
    for (auto& stroke: ink) {
        if (stroke.page != page) {
            continue;
        }
        if (stroke.layerId == topId) {
            stroke.layerId = topId - 1;
        } else if (stroke.layerId > topId) {
            --stroke.layerId;
        }
    }
    for (auto& text: texts) {
        if (text.page != page) {
            continue;
        }
        if (text.layerId == topId) {
            text.layerId = topId - 1;
        } else if (text.layerId > topId) {
            --text.layerId;
        }
    }
    for (auto& image: images) {
        if (image.page != page) {
            continue;
        }
        if (image.layerId == topId) {
            image.layerId = topId - 1;
        } else if (image.layerId > topId) {
            --image.layerId;
        }
    }
    recordLayerMutation(std::move(before));
    clearSelection();
    return true;
}

void CanvasState::recordLayerMutation(LayerMutation mutation) {
    const size_t pageIndex = static_cast<size_t>(mutation.page);
    mutation.afterLayers = pageLayers.at(pageIndex);
    for (size_t i = 0; i < ink.size(); ++i) {
        if (ink[i].page == mutation.page) {
            mutation.afterInk.push_back({i, ink[i]});
        }
    }
    for (size_t i = 0; i < texts.size(); ++i) {
        if (texts[i].page == mutation.page) {
            mutation.afterTexts.push_back({i, texts[i]});
        }
    }
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].page == mutation.page) {
            mutation.afterImages.push_back({i, images[i]});
        }
    }
    Edit edit;
    edit.layerMutation = std::move(mutation);
    record(std::move(edit));
}

void CanvasState::remapObjectsAfterLayerRemoval(int pageIndex, size_t deletedId) {
    std::erase_if(ink, [pageIndex, deletedId](const Ink& item) {
        return item.page == pageIndex && item.layerId == deletedId;
    });
    for (auto& stroke: ink) {
        if (stroke.page == pageIndex && stroke.layerId > deletedId) {
            --stroke.layerId;
        }
    }
    std::erase_if(texts, [pageIndex, deletedId](const TextItem& item) {
        return item.page == pageIndex && item.layerId == deletedId;
    });
    for (auto& text: texts) {
        if (text.page == pageIndex && text.layerId > deletedId) {
            --text.layerId;
        }
    }
    std::erase_if(images, [pageIndex, deletedId](const CanvasImage& image) {
        return image.page == pageIndex && image.layerId == deletedId;
    });
    for (auto& image: images) {
        if (image.page == pageIndex && image.layerId > deletedId) {
            --image.layerId;
        }
    }
}

}  // namespace prototype
