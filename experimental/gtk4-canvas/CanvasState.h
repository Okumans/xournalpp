#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "BehaviorDefaults.h"
#include "CanvasDrawing.h"
#include "CanvasImage.h"
#include "CanvasLayers.h"
#include "CanvasText.h"

namespace prototype {

struct PageSize {
    double width = 595;
    double height = 842;
};

struct PageHit {
    int page = 0;
    Point local{};
};

enum class Tool {
    Pen,
    Highlighter,
    Eraser,
    SelectRectangle,
    SelectLasso,
    SmartSelect,
    Text,
    SelectObject,
    Hand,
    SelectPdfTextLinear,
    SelectPdfTextRect,
    VerticalSpace,
    Link,
    LaserPointerPen,
    LaserPointerHighlighter,
    PlayObject,
    FloatingToolbox,
    Latex,
};
inline bool isLaserPointerTool(Tool tool) {
    return tool == Tool::LaserPointerPen || tool == Tool::LaserPointerHighlighter;
}
inline bool isFillableTool(Tool tool) { return tool == Tool::Pen || tool == Tool::Highlighter; }
inline bool isSelectionTool(Tool tool) {
    return tool == Tool::SelectRectangle || tool == Tool::SelectLasso || tool == Tool::SmartSelect ||
           tool == Tool::SelectObject || tool == Tool::SelectPdfTextLinear || tool == Tool::SelectPdfTextRect;
}
inline bool isPdfTextSelectionTool(Tool tool) {
    return tool == Tool::SelectPdfTextLinear || tool == Tool::SelectPdfTextRect;
}

enum class SelectableObjectKind { Stroke, Text, Image };

struct SelectableObject {
    SelectableObjectKind kind;
    size_t index;
};
enum class EraserMode { Standard, Whiteout, DeleteStroke };

inline constexpr const std::array<double, 5>& widthsForTool(Tool tool) {
    switch (tool) {
        case Tool::Highlighter:
            return behavior::highlighterWidths;
        case Tool::Eraser:
            return behavior::eraserWidths;
        case Tool::LaserPointerPen:
            return behavior::laserPenWidths;
        case Tool::LaserPointerHighlighter:
            return behavior::laserHighlighterWidths;
        case Tool::Pen:
        default:
            return behavior::penWidths;
    }
}

struct Brush {
    Tool tool = Tool::Pen;
    std::array<double, 3> color = behavior::defaultPenColor;
    double width = behavior::penWidths[static_cast<size_t>(behavior::ToolSize::Medium)];
    bool fillEnabled = false;
    int fillOpacity = 128;
};

struct Ink {
    int page;
    std::vector<Point> points;
    Brush brush;
    bool pressureSensitive = false;
    size_t layerOrder = 0;
    size_t layerId = 1;
    size_t originLayer = newLayerOrigin;
    int fill = -1;
    int capStyle = 0;
    std::uintptr_t pluginReference = 0;
};

struct LaserTrail {
    int page = 0;
    std::vector<Ink> strokes;
    uint8_t alpha = 255;
    std::optional<std::chrono::steady_clock::time_point> fadeStart;
};

struct SelectionBox {
    int page;
    double x0, y0, x1, y1;
};

using EditableObject = std::variant<Ink, TextItem, CanvasImage>;

struct VerticalSpacePreview {
    int page = 0;
    size_t layerId = 1;
    double startY = 0;
    double endY = 0;
    bool moveAbove = false;
    std::vector<size_t> inkIndices;
    std::vector<size_t> textIndices;
    std::vector<size_t> imageIndices;
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
    std::uint64_t pageStructureGeneration = 0;
    std::vector<PageSize> pages;
    std::vector<PageLayers> pageLayers{PageLayers{}};
    Brush brush;
    DrawingMode drawingMode = DrawingMode::Freehand;
    EraserMode eraserMode = EraserMode::Standard;
    double eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::Medium)];
    bool pressureSensitivity = true;
    double minimumPressure = behavior::minimumPressure;
    double pressureMultiplier = behavior::pressureMultiplier;
    double shapeRecognizerMinSize = 40;
    bool snapRotationEnabled = true;
    double snapRotationTolerance = 0.30;
    bool snapGridEnabled = true;
    double snapGridSize = behavior::snapGridSize;
    double snapGridTolerance = behavior::snapGridTolerance;
    double edgePanSpeed = 20.0;
    double edgePanMaxMultiplier = 5.0;
    double zoomStep = behavior::zoomStep;
    double scrollZoomStep = behavior::scrollZoomStep;
    unsigned int laserFadeOutTimeMs = behavior::laserPointerFadeOutTimeMs;
    std::string textFontName = "Sans";
    double textFontSize = 12;
    int textAlignment = 0;
    bool textJustify = false;
    std::array<double, 3> textColor{0, 0, 0};
    std::vector<Ink> ink;
    std::vector<TextItem> texts;
    std::vector<CanvasImage> images;
    std::optional<Ink> current;
    std::optional<Ink> currentLaser;
    std::vector<LaserTrail> laserTrails;
    std::vector<Point> rawCurrent;
    std::vector<Point> splineKnots;
    std::vector<Point> splineTangents;
    std::optional<Point> splineHover;
    bool currentShift = false;
    bool currentControl = false;
    bool currentAlt = false;
    std::vector<size_t> selected;
    std::vector<size_t> selectedTexts;
    std::vector<size_t> selectedImages;
    bool selectionAcrossLayers = false;
    bool pinching = false;
    long long scrollBlockedUntil = 0;

    int pageCount() const { return pages.empty() ? 1 : static_cast<int>(pages.size()); }

    const PageLayers& layersForPage(int pageIndex) const {
        static const PageLayers empty{};
        return pageIndex >= 0 && static_cast<size_t>(pageIndex) < pageLayers.size() ?
                       pageLayers[static_cast<size_t>(pageIndex)] : empty;
    }

    size_t activeLayerId(int pageIndex) const { return layersForPage(pageIndex).selectedLayer; }

    // Lua exposes these as opaque lightuserdata, matching the GTK 3 API's
    // reference shape. The integer token is never dereferenced, so vector
    // growth and object moves cannot invalidate a reference.
    void ensurePluginReferences() {
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
            nextPluginReference = greatestReference == std::numeric_limits<std::uintptr_t>::max() ?
                                          0 :
                                          greatestReference + 1;
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

    void addPluginReferencesToSelection(const std::vector<std::uintptr_t>& requestedReferences) {
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

    size_t activeLayerOrigin(int pageIndex) const {
        const auto& pageState = layersForPage(pageIndex);
        if (pageState.selectedLayer == 0 || pageState.selectedLayer > pageState.layers.size()) {
            return 0;
        }
        const auto& origins = pageState.layers[pageState.selectedLayer - 1].origins;
        return origins.empty() ? newLayerOrigin : origins.back();
    }

    bool layerVisible(int pageIndex, size_t layerId) const {
        const auto& pageState = layersForPage(pageIndex);
        if (layerId == 0) {
            return pageState.backgroundVisible;
        }
        return layerId <= pageState.layers.size() && pageState.layers[layerId - 1].visible;
    }

    void setLayerDocument(const std::vector<PageLayers>& documentLayers) {
        if (documentLayers.size() != pageLayers.size()) {
            throw std::runtime_error("Layer metadata page count does not match the document");
        }
        pageLayers = documentLayers;
        for (auto& item: ink) {
            if (item.page >= 0 && static_cast<size_t>(item.page) < pageLayers.size()) {
                const auto& layers = pageLayers[static_cast<size_t>(item.page)].layers;
                item.layerId = item.layerId == legacyTopLayer ? layers.size() :
                                                                  std::min(item.layerId, layers.size());
                if (item.layerId == 0) {
                    item.layerId = layers.size();
                }
                if (item.originLayer == newLayerOrigin && item.layerId > 0) {
                    item.originLayer = layers[item.layerId - 1].origins.back();
                }
            }
        }
        for (auto& item: texts) {
            if (item.page >= 0 && static_cast<size_t>(item.page) < pageLayers.size()) {
                const auto& layers = pageLayers[static_cast<size_t>(item.page)].layers;
                item.layerId = item.layerId == legacyTopLayer ? layers.size() :
                                                                  std::min(item.layerId, layers.size());
                if (item.layerId == 0) {
                    item.layerId = layers.size();
                }
                if (item.originLayer == newLayerOrigin && item.layerId > 0) {
                    item.originLayer = layers[item.layerId - 1].origins.back();
                }
            }
        }
    }

    bool selectLayer(int pageIndex, size_t layerId) {
        if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLayers.size() || layerId == 0 ||
            layerId > pageLayers[static_cast<size_t>(pageIndex)].layers.size()) {
            return false;
        }
        cancel();
        clearSelection();
        pageLayers[static_cast<size_t>(pageIndex)].selectedLayer = layerId;
        return true;
    }

    bool setLayerVisible(int pageIndex, size_t layerId, bool visible) {
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

    bool renameLayer(int pageIndex, size_t layerId, const std::string& name) {
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

    bool setBackgroundName(int pageIndex, const std::string& name) {
        if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLayers.size()) {
            return false;
        }
        pageLayers[static_cast<size_t>(pageIndex)].backgroundName = name;
        return true;
    }

    bool resizePage(int pageIndex, double width, double height) {
        if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= static_cast<size_t>(pageCount()) ||
            !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            return false;
        }
        const auto before = pageSize(pageIndex);
        if (before.width == width && before.height == height) {
            return false;
        }
        Edit edit;
        edit.pageSizeChange = PageSizeChange{pageIndex, before, {width, height}};
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    bool addPluginStrokes(std::vector<Ink> additions, int undoPolicy) {
        if (additions.empty()) {
            return true;
        }
        if (page < 0 || static_cast<size_t>(page) >= pageLayers.size() ||
            undoPolicy < 0 || undoPolicy > 2) {
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

    bool addPluginTexts(std::vector<TextItem> additions, int undoPolicy) {
        if (additions.empty()) {
            return true;
        }
        if (page < 0 || static_cast<size_t>(page) >= pageLayers.size() ||
            undoPolicy < 0 || undoPolicy > 2 || current || eraseGesture || selectionGesture) {
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

    bool addPluginImages(std::vector<CanvasImage> additions, int undoPolicy) {
        if (additions.empty()) {
            return true;
        }
        if (page < 0 || static_cast<size_t>(page) >= pageLayers.size() ||
            undoPolicy < 0 || undoPolicy > 2 || current || eraseGesture || selectionGesture ||
            images.size() > maxCanvasImageObjects || additions.size() > maxCanvasImageObjects - images.size()) {
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

    bool changePageBackground(int pageIndex, const std::string& format, const std::string& config = {}) {
        if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pageLayers.size() || format.empty()) {
            return false;
        }
        auto& pageState = pageLayers[static_cast<size_t>(pageIndex)];
        if (pageState.backgroundFormat == format && pageState.backgroundConfig == config) {
            return false;
        }
        auto before = captureLayerMutation(pageIndex);
        pageState.backgroundFormat = format;
        pageState.backgroundConfig = config;
        recordLayerMutation(std::move(before));
        clearSelection();
        if (backgroundChangedCallback) {
            backgroundChangedCallback(pageIndex);
        }
        return true;
    }

    std::function<void(int)> backgroundChangedCallback;

    bool setAllLayersVisible(int pageIndex, bool visible) {
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

    bool addLayerAboveCurrent() {
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

    bool addLayerBelowCurrent() {
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

    bool duplicateCurrentLayer() {
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
        ink.insert(ink.end(), std::make_move_iterator(duplicateInk.begin()),
                   std::make_move_iterator(duplicateInk.end()));
        texts.insert(texts.end(), std::make_move_iterator(duplicateTexts.begin()),
                     std::make_move_iterator(duplicateTexts.end()));
        images.insert(images.end(), std::make_move_iterator(duplicateImages.begin()),
                      std::make_move_iterator(duplicateImages.end()));

        recordLayerMutation(std::move(before));
        clearSelection();
        return true;
    }

    bool moveCurrentLayer(bool up) {
        if (page < 0 || static_cast<size_t>(page) >= pageLayers.size()) {
            return false;
        }
        const size_t pageIndex = static_cast<size_t>(page);
        auto before = captureLayerMutation(page);
        auto& pageState = pageLayers[pageIndex];
        const size_t currentId = pageState.selectedLayer;
        if (currentId == 0 || currentId > pageState.layers.size() ||
            (up && currentId == pageState.layers.size()) || (!up && currentId == 1)) {
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

    bool deleteCurrentLayer() {
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

    bool mergeCurrentLayerDown() {
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

    std::optional<SelectionBox> selectionBox() const {
        if (selectionGesture && selectionGesture->moving) {
            const auto& g = *selectionGesture;
            const auto corners = transformedCorners(g.bounds, g.frameTransform);
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
        if (selectionFrameMatchesCurrentSelection()) {
            return selectionFrame->bounds;
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
        for (auto index: selectedTexts) {
            if (index >= texts.size()) {
                continue;
            }
            for (const auto& point: textCorners(texts[index])) {
                if (!box) {
                    box = SelectionBox{texts[index].page, point.x, point.y, point.x, point.y};
                } else {
                    box->x0 = std::min(box->x0, point.x);
                    box->y0 = std::min(box->y0, point.y);
                    box->x1 = std::max(box->x1, point.x);
                    box->y1 = std::max(box->y1, point.y);
                }
            }
        }
        for (auto index: selectedImages) {
            if (index >= images.size()) {
                continue;
            }
            for (const auto& point: canvasImageCorners(images[index])) {
                if (!box) {
                    box = SelectionBox{images[index].page, point.x, point.y, point.x, point.y};
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

    bool movingSelection() const { return selectionGesture && selectionGesture->moving; }

    bool selectionFrameMatchesCurrentSelection() const {
        return selectionFrame && selectionFrame->inkIndices == selected &&
               selectionFrame->textIndices == selectedTexts && selectionFrame->imageIndices == selectedImages;
    }

    double selectionRotation() const {
        return selectionFrameMatchesCurrentSelection() ? selectionFrame->rotation : 0;
    }

    bool selectionCanEdgePan() const {
        return selectionGesture && selectionGesture->moving && selectionGesture->handle < 0;
    }

    const std::vector<Ink>& committedInk() const { return ink; }

    static std::array<Point, 4> transformedCorners(const SelectionBox& b, const SelectionTransform& matrix = {}) {
        return {matrix.apply({b.x0, b.y0}), matrix.apply({b.x1, b.y0}),
                matrix.apply({b.x1, b.y1}), matrix.apply({b.x0, b.y1})};
    }

    SelectionTransform selectionMatrix(size_t index) const {
        if (selectionGesture && selectionGesture->moving && std::binary_search(selected.begin(), selected.end(), index)) {
            return selectionGesture->transform;
        }
        if (verticalSpaceAffectsInk(index)) {
            return {1, 0, 0, 1, 0, verticalSpace->endY - verticalSpace->startY, 1};
        }
        return {};
    }

    int displayPage(size_t index) const {
        if (selectionGesture && selectionGesture->moving && std::binary_search(selected.begin(), selected.end(), index)) {
            return selectionGesture->targetPage;
        }
        return ink.at(index).page;
    }

    TextItem displayText(size_t index) const {
        auto item = texts.at(index);
        if (verticalSpaceAffectsText(index)) {
            item.y += verticalSpace->endY - verticalSpace->startY;
        }
        if (!selectionGesture || !selectionGesture->moving ||
            !std::binary_search(selectionGesture->textIndices.begin(), selectionGesture->textIndices.end(), index)) {
            return item;
        }

        item = transformText(item, selectionGesture->transform);
        if (selectionGesture->targetPage != selectionGesture->page) {
            item.page = selectionGesture->targetPage;
            item.layerId = activeLayerId(item.page);
            item.originLayer = activeLayerOrigin(item.page);
            item.layerOrder = nextOriginOrder(item.page, item.originLayer);
        }
        return item;
    }

    CanvasImage displayImage(size_t index) const {
        auto image = images.at(index);
        if (verticalSpaceAffectsImage(index)) {
            image.transform.y += verticalSpace->endY - verticalSpace->startY;
        }
        if (!selectionGesture || !selectionGesture->moving ||
            !std::binary_search(selectionGesture->imageIndices.begin(), selectionGesture->imageIndices.end(), index)) {
            return image;
        }

        image = transformImage(std::move(image), selectionGesture->transform);
        if (selectionGesture->targetPage != selectionGesture->page) {
            image.page = selectionGesture->targetPage;
            image.layerId = activeLayerId(image.page);
            image.originLayer = activeLayerOrigin(image.page);
            const auto position = std::lower_bound(selectionGesture->imageIndices.begin(),
                                                   selectionGesture->imageIndices.end(), index) -
                                  selectionGesture->imageIndices.begin();
            image.layerOrder = nextOriginOrder(image.page, image.originLayer) + static_cast<size_t>(position);
        }
        return image;
    }

    int displayTextPage(size_t index) const { return displayText(index).page; }

    size_t displayTextLayerId(size_t index) const { return displayText(index).layerId; }

    size_t displayTextLayerOrigin(size_t index) const { return displayText(index).originLayer; }

    size_t displayLayerId(size_t index) const {
        if (selectionGesture && selectionGesture->moving &&
            std::binary_search(selected.begin(), selected.end(), index) &&
            selectionGesture->targetPage != ink.at(index).page) {
            return activeLayerId(selectionGesture->targetPage);
        }
        return ink.at(index).layerId;
    }

    size_t displayLayerOrigin(size_t index) const {
        if (selectionGesture && selectionGesture->moving &&
            std::binary_search(selected.begin(), selected.end(), index) &&
            selectionGesture->targetPage != ink.at(index).page) {
            return activeLayerOrigin(selectionGesture->targetPage);
        }
        return ink.at(index).originLayer;
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
                               transformedCorners(selectionGesture->bounds, selectionGesture->frameTransform) :
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

    const VerticalSpacePreview* verticalSpacePreview() const {
        return verticalSpace ? &*verticalSpace : nullptr;
    }

    bool verticalSpaceAffectsInk(size_t index) const {
        return verticalSpace && std::binary_search(verticalSpace->inkIndices.begin(), verticalSpace->inkIndices.end(),
                                                   index);
    }

    bool verticalSpaceAffectsText(size_t index) const {
        return verticalSpace && std::binary_search(verticalSpace->textIndices.begin(),
                                                   verticalSpace->textIndices.end(), index);
    }

    bool verticalSpaceAffectsImage(size_t index) const {
        return verticalSpace && std::binary_search(verticalSpace->imageIndices.begin(),
                                                   verticalSpace->imageIndices.end(), index);
    }

    bool beginVerticalSpace(double x, double y, bool moveAbove = false) {
        const auto hit = hitPage(x, y);
        if (!hit || current || eraseGesture || selectionGesture || verticalSpace || pinching) {
            return false;
        }

        selectActivePage(hit->page);
        VerticalSpacePreview preview;
        preview.page = hit->page;
        preview.layerId = activeLayerId(hit->page);
        preview.startY = preview.endY = snapVertical(hit->page, hit->local.y);
        preview.moveAbove = moveAbove;
        collectVerticalSpaceObjects(preview);
        verticalSpace = std::move(preview);
        return true;
    }

    void moveVerticalSpace(double x, double y, bool moveAbove) {
        if (!verticalSpace || !std::isfinite(x) || !std::isfinite(y)) {
            return;
        }
        setVerticalSpaceDirection(moveAbove);
        const auto local = pageLocalPoint(verticalSpace->page, x, y);
        verticalSpace->endY = snapVertical(verticalSpace->page, local.y);
    }

    void setVerticalSpaceDirection(bool moveAbove) {
        if (verticalSpace && verticalSpace->moveAbove != moveAbove) {
            verticalSpace->moveAbove = moveAbove;
            collectVerticalSpaceObjects(*verticalSpace);
        }
    }

    bool finishVerticalSpace() {
        if (!verticalSpace) {
            return false;
        }
        auto preview = std::move(*verticalSpace);
        verticalSpace.reset();
        const double deltaY = preview.endY - preview.startY;
        if (deltaY == 0 || (preview.inkIndices.empty() && preview.textIndices.empty() &&
                            preview.imageIndices.empty())) {
            return false;
        }

        Edit edit;
        edit.selectionBefore = edit.selectionAfter = selected;
        edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
        edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
        for (const auto index: preview.inkIndices) {
            if (index >= ink.size()) {
                return false;
            }
            auto moved = ink[index];
            for (auto& point: moved.points) {
                point.y += deltaY;
                if (!std::isfinite(point.y)) {
                    return false;
                }
            }
            edit.changes.push_back({index, {ink[index]}, index, {std::move(moved)}});
        }
        for (const auto index: preview.textIndices) {
            if (index >= texts.size()) {
                return false;
            }
            auto moved = texts[index];
            moved.y += deltaY;
            if (!validTextPage(moved)) {
                return false;
            }
            edit.textChanges.push_back({index, {texts[index]}, index, {std::move(moved)}});
        }
        for (const auto index: preview.imageIndices) {
            if (index >= images.size()) {
                return false;
            }
            auto moved = images[index];
            moved.transform.y += deltaY;
            if (!validCanvasImage(moved)) {
                return false;
            }
            edit.imageChanges.push_back({index, {images[index]}, index, {std::move(moved)}});
        }
        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    void cancelVerticalSpace() { verticalSpace.reset(); }

    void cancelSelection() {
        if (selectionGesture) {
            selected = std::move(selectionGesture->previousSelection);
            selectedTexts = std::move(selectionGesture->previousTextSelection);
            selectedImages = std::move(selectionGesture->previousImageSelection);
            selectionFrame = std::move(selectionGesture->previousSelectionFrame);
            selectionGesture.reset();
        }
    }

    void clearSelection() {
        cancelSelection();
        cancelVerticalSpace();
        selected.clear();
        selectedTexts.clear();
        selectedImages.clear();
        selectionFrame.reset();
    }

    bool hasSelection() const { return !selected.empty() || !selectedTexts.empty() || !selectedImages.empty(); }

    size_t selectionCount() const { return selected.size() + selectedTexts.size() + selectedImages.size(); }

    std::optional<SelectableObject> pickSmartSelectObject(int pageIndex, Point point) const {
        return pickSelectableObject(pageIndex, point);
    }

    std::optional<size_t> pickStroke(int pageIndex, Point point) const {
        const size_t active = activeLayerId(pageIndex);
        if (!layerVisible(pageIndex, active)) {
            return std::nullopt;
        }
        for (size_t i = ink.size(); i-- > 0;) {
            const auto& stroke = ink[i];
            if (stroke.page != pageIndex || stroke.layerId != active) continue;
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

    std::optional<size_t> pickText(int pageIndex, Point point) const {
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

    std::optional<size_t> pickLink(int pageIndex, Point point) const {
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
        auto hit = hitPage(x, y);
        if (!hit && !forceRectangle) {
            hit = hitSelectionHandle(x, y);
        }
        if (!hit || current || eraseGesture || selectionGesture || pinching) {
            return false;
        }
        if (selectionFrame && !selectionFrameMatchesCurrentSelection()) {
            selectionFrame.reset();
        }
        SelectionGesture gesture;
        gesture.page = hit->page;
        gesture.targetPage = hit->page;
        gesture.start = gesture.latest = hit->local;
        gesture.previousSelection = selected;
        gesture.previousTextSelection = selectedTexts;
        gesture.previousImageSelection = selectedImages;
        gesture.previousSelectionFrame = selectionFrame;
        gesture.baseRotation = selectionFrame ? selectionFrame->rotation : 0;
        gesture.rotationTarget = gesture.baseRotation;
        gesture.multiLayer = selectionAcrossLayers;
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
        if (brush.tool == Tool::SelectObject) {
            if (gesture.moving) {
                gesture.indices = selected;
                gesture.textIndices = selectedTexts;
                gesture.imageIndices = selectedImages;
                gesture.bounds = gesture.handle >= 0 ? handleBounds(*bounds) : *bounds;
                selectionGesture = std::move(gesture);
            } else {
                selectObjectAt(hit->page, hit->local, forceRectangle);
            }
            selectActivePage(hit->page);
            return true;
        }
        if (!gesture.moving && brush.tool == Tool::SmartSelect && !forceRectangle) {
            const auto pickedObject = pickSmartSelectObject(hit->page, hit->local);
            if (pickedObject) {
                selected.clear();
                selectedTexts.clear();
                selectedImages.clear();
                selectionFrame.reset();
                gesture.baseRotation = 0;
                gesture.rotationTarget = 0;
                switch (pickedObject->kind) {
                    case SelectableObjectKind::Stroke:
                        selected = {pickedObject->index};
                        break;
                    case SelectableObjectKind::Text:
                        selectedTexts = {pickedObject->index};
                        break;
                    case SelectableObjectKind::Image:
                        selectedImages = {pickedObject->index};
                        break;
                }
                bounds = selectionBox();
                gesture.moving = true;
            }
        }
        if (gesture.moving) {
            gesture.indices = selected;
            gesture.textIndices = selectedTexts;
            gesture.imageIndices = selectedImages;
            gesture.bounds = gesture.handle >= 0 ? handleBounds(*bounds) : *bounds;
            gesture.snapAnchor = closestSelectionCorner(*bounds, gesture.start);
        } else {
            selected.clear();
            selectedTexts.clear();
            selectedImages.clear();
            selectionFrame.reset();
            gesture.lasso = brush.tool == Tool::SelectLasso && !forceRectangle;
            gesture.boundary.push_back(hit->local);
            gesture.additive = forceRectangle && bounds && bounds->page == hit->page;
        }
        selectActivePage(hit->page);
        selectionGesture = std::move(gesture);
        return true;
    }

    void moveSelectionPointer(double x, double y, bool altHeld = false) {
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
            const auto previousFrame = g.frameTransform;
            g.transform = {};
            g.frameTransform = {};
            if (g.handle < 0) {
                const double dx = g.latest.x - g.start.x;
                const double dy = g.latest.y - g.start.y;
                const Point candidate{g.snapAnchor.x + dx, g.snapAnchor.y + dy};
                const Point snapped = snapSelectionMove(g.targetPage, candidate, altHeld);
                g.transform.x0 = snapped.x - g.snapAnchor.x;
                g.transform.y0 = snapped.y - g.snapAnchor.y;
                g.frameTransform = g.transform;
            } else if (g.handle == 8) {
                const auto cx = (g.bounds.x0+g.bounds.x1)/2, cy = (g.bounds.y0+g.bounds.y1)/2;
                auto angle = std::atan2(g.latest.y - cy, g.latest.x - cx) -
                             std::atan2(g.start.y - cy, g.start.x - cx);
                if (!g.imageIndices.empty() && snapRotationEnabled != altHeld) {
                    angle = canvasImageRotationDelta(std::atan2(g.start.y - cy, g.start.x - cx),
                                                     std::atan2(g.latest.y - cy, g.latest.x - cx), true,
                                                     snapRotationTolerance);
                }
                g.rotationTarget = g.baseRotation + angle;
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
                const auto rawFactor = factor;
                auto fx = xs ? factor : 1;
                auto fy = ys ? factor : 1;
                g.frameTransform = {fx, 0, 0, fy, ax * (1 - fx), ay * (1 - fy),
                                    std::sqrt(std::abs(fx * fy))};

                const auto candidateEdgeX = g.frameTransform.xx * (xs < 0 ? g.bounds.x0 : g.bounds.x1) +
                                            g.frameTransform.x0;
                const auto candidateEdgeY = g.frameTransform.yy * (ys < 0 ? g.bounds.y0 : g.bounds.y1) +
                                            g.frameTransform.y0;
                const auto snappedX = snapSelectionResizeAxis(g.targetPage, candidateEdgeX, true, altHeld);
                const auto snappedY = snapSelectionResizeAxis(g.targetPage, candidateEdgeY, false, altHeld);
                const auto dx = snappedX - candidateEdgeX;
                const auto dy = snappedY - candidateEdgeY;
                const auto currentWidth = (g.bounds.x1 - g.bounds.x0) * g.frameTransform.xx;
                const auto currentHeight = (g.bounds.y1 - g.bounds.y0) * g.frameTransform.yy;
                const auto snapFactorX = xs && std::abs(currentWidth) > 5 / zoom ?
                                                 (currentWidth + dx * xs) / currentWidth : 1.0;
                const auto snapFactorY = ys && std::abs(currentHeight) > 5 / zoom ?
                                                 (currentHeight + dy * ys) / currentHeight : 1.0;
                auto correction = (((std::abs(dx) < std::abs(dy)) && snapFactorX != 1) || snapFactorY == 1) ?
                                          snapFactorX : snapFactorY;
                if (std::abs(currentWidth * correction) < 5 / zoom ||
                    std::abs(currentHeight * correction) < 5 / zoom) {
                    correction = 1;
                }
                factor = rawFactor * correction;
                fx = xs ? factor : 1;
                fy = ys ? factor : 1;
                g.frameTransform = {fx, 0, 0, fy, ax * (1 - fx), ay * (1 - fy),
                                    std::sqrt(std::abs(fx * fy))};

                const auto cx = (g.bounds.x0 + g.bounds.x1) / 2;
                const auto cy = (g.bounds.y0 + g.bounds.y1) / 2;
                const auto cosine = std::cos(g.baseRotation);
                const auto sine = std::sin(g.baseRotation);
                const auto pivotX = cx + cosine * (ax - cx) - sine * (ay - cy);
                const auto pivotY = cy + sine * (ax - cx) + cosine * (ay - cy);
                const auto xx = cosine * cosine * fx + sine * sine * fy;
                const auto xy = cosine * sine * (fx - fy);
                const auto yx = xy;
                const auto yy = sine * sine * fx + cosine * cosine * fy;
                g.transform = {xx, yx, xy, yy, pivotX - xx * pivotX - xy * pivotY,
                               pivotY - yx * pivotX - yy * pivotY, std::sqrt(std::abs(fx * fy))};
            }
            const auto& m = g.transform;
            if (!std::isfinite(m.xx) || !std::isfinite(m.xy) || !std::isfinite(m.yx) || !std::isfinite(m.yy) ||
                !std::isfinite(m.x0) || !std::isfinite(m.y0) || !std::isfinite(m.widthScale) || m.widthScale <= 0)
                g.transform = previous;
            const auto& frame = g.frameTransform;
            if (!std::isfinite(frame.xx) || !std::isfinite(frame.xy) || !std::isfinite(frame.yx) ||
                !std::isfinite(frame.yy) || !std::isfinite(frame.x0) || !std::isfinite(frame.y0) ||
                !std::isfinite(frame.widthScale) || frame.widthScale <= 0) {
                g.frameTransform = previousFrame;
            }
        } else if (g.lasso && g.boundary.size() < 1000000 &&
                   std::hypot(g.latest.x-g.boundary.back().x,g.latest.y-g.boundary.back().y) >= .3) {
            g.boundary.push_back(g.latest);
        }
    }

    bool edgePanSelectionAtPointer(double x, double y, double viewportWidth, double viewportHeight,
                                   bool altHeld = false) {
        if (!selectionCanEdgePan() || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) || viewportWidth <= 0 ||
            viewportHeight <= 0 || !std::isfinite(zoom) || zoom <= 0 || !std::isfinite(edgePanSpeed) ||
            !std::isfinite(edgePanMaxMultiplier) || edgePanSpeed <= 0 || edgePanMaxMultiplier <= 0) {
            return false;
        }
        const auto bounds = selectionBox();
        if (!bounds) {
            return false;
        }

        const auto pageIndex = bounds->page;
        const auto screenX = offsetX + (pageLeft(pageIndex) + bounds->x0) * zoom;
        const auto screenY = offsetY + (pageTop(pageIndex) + bounds->y0) * zoom;
        const auto screenWidth = (bounds->x1 - bounds->x0) * zoom;
        const auto screenHeight = (bounds->y1 - bounds->y0) * zoom;

        const auto scrollAmount = [&](double viewportLength, double bboxMin, double bboxLength, double pointer,
                                      double layoutLength, double& offset) {
            if (!std::isfinite(viewportLength) || !std::isfinite(bboxMin) || !std::isfinite(bboxLength) ||
                !std::isfinite(pointer) || !std::isfinite(layoutLength) || viewportLength <= 0 || bboxLength <= 0) {
                return;
            }
            const auto bboxMax = bboxMin + bboxLength;
            const auto isLargeSelection = bboxLength > viewportLength;
            double multiplier = 0;
            int direction = 0;
            if (isLargeSelection) {
                const auto pointerDiff = pointer - viewportLength / 2.0;
                multiplier = edgePanMaxMultiplier * std::abs(pointerDiff) / viewportLength;
                if (pointerDiff > 0.05 * viewportLength) {
                    direction = 1;
                } else if (pointerDiff < -0.05 * viewportLength) {
                    direction = -1;
                }
            } else if (bboxMax > viewportLength) {
                direction = 1;
                multiplier = edgePanMaxMultiplier * std::min(bboxLength, bboxMax - viewportLength) / bboxLength;
            } else if (bboxMin < 0) {
                direction = -1;
                multiplier = edgePanMaxMultiplier * std::min(bboxLength, -bboxMin) / bboxLength;
            }
            if (!direction || layoutLength <= viewportLength) {
                return;
            }

            constexpr double timerRate = 30.0;
            const auto requestedScroll = zoom * direction *
                                         (viewportLength * edgePanSpeed / (100.0 * timerRate) * multiplier);
            const auto nextOffset = std::clamp(offset - requestedScroll, viewportLength - layoutLength, 0.0);
            offset = nextOffset;
        };

        const auto oldOffsetX = offsetX;
        const auto oldOffsetY = offsetY;
        scrollAmount(viewportWidth, screenX, screenWidth, x, layoutWidth() * zoom, offsetX);
        scrollAmount(viewportHeight, screenY, screenHeight, y, layoutHeight() * zoom, offsetY);
        if (offsetX == oldOffsetX && offsetY == oldOffsetY) {
            return false;
        }

        // Re-evaluate at the stationary pointer so the preview follows the content while the viewport scrolls.
        moveSelectionPointer(x, y, altHeld);
        return true;
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
                        moved.layerId = activeLayerId(g.targetPage);
                        moved.originLayer = activeLayerOrigin(g.targetPage);
                        moved.layerOrder = nextOriginOrder(g.targetPage, moved.originLayer) + transferred.size();
                        const auto removed = transferred.size();
                        edit.changes.push_back({index, {ink[index]}, index-removed, {}});
                        transferred.push_back(std::move(moved));
                    }
                }
                if (!transferred.empty()) edit.changes.push_back({ink.size(), {}, ink.size()-transferred.size(), std::move(transferred)});

                std::vector<TextItem> transferredTexts;
                for (auto index: g.textIndices) {
                    auto moved = transformText(texts.at(index), g.transform);
                    if (!validTextPage(moved)) {
                        return;
                    }
                    moved.page = g.targetPage;
                    if (g.targetPage == g.page) {
                        edit.textChanges.push_back({index, {texts[index]}, index, {std::move(moved)}});
                    } else {
                        moved.layerId = activeLayerId(g.targetPage);
                        moved.originLayer = activeLayerOrigin(g.targetPage);
                        moved.layerOrder = nextOriginOrder(g.targetPage, moved.originLayer) + transferredTexts.size();
                        const auto removed = transferredTexts.size();
                        edit.textChanges.push_back({index, {texts[index]}, index - removed, {}});
                        transferredTexts.push_back(std::move(moved));
                    }
                }
                if (!transferredTexts.empty()) {
                    edit.textChanges.push_back({texts.size(), {}, texts.size() - transferredTexts.size(),
                                                std::move(transferredTexts)});
                }

                std::vector<CanvasImage> transferredImages;
                for (auto index: g.imageIndices) {
                    auto moved = transformImage(images.at(index), g.transform);
                    moved.page = g.targetPage;
                    if (!validCanvasImage(moved) || moved.page < 0 || moved.page >= pageCount()) {
                        return;
                    }
                    if (g.targetPage == g.page) {
                        edit.imageChanges.push_back({index, {images[index]}, index, {std::move(moved)}});
                    } else {
                        moved.layerId = activeLayerId(g.targetPage);
                        moved.originLayer = activeLayerOrigin(g.targetPage);
                        moved.layerOrder = nextOriginOrder(g.targetPage, moved.originLayer) + transferredImages.size();
                        const auto removed = transferredImages.size();
                        edit.imageChanges.push_back({index, {images[index]}, index - removed, {}});
                        transferredImages.push_back(std::move(moved));
                    }
                }
                if (!transferredImages.empty()) {
                    edit.imageChanges.push_back({images.size(), {}, images.size() - transferredImages.size(),
                                                 std::move(transferredImages)});
                }
            }
            if (!edit.changes.empty() || !edit.textChanges.empty() || !edit.imageChanges.empty()) {
                edit.selectionBefore = edit.selectionAfter = selected;
                edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
                edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
                if (g.targetPage != g.page) {
                    edit.selectionAfter = std::vector<size_t>{};
                    for (size_t n = ink.size()-selected.size(); n < ink.size(); ++n) edit.selectionAfter->push_back(n);
                    edit.textSelectionAfter = std::vector<size_t>{};
                    for (size_t n = texts.size() - selectedTexts.size(); n < texts.size(); ++n) {
                        edit.textSelectionAfter->push_back(n);
                    }
                    edit.imageSelectionAfter = std::vector<size_t>{};
                    for (size_t n = images.size() - selectedImages.size(); n < images.size(); ++n) {
                        edit.imageSelectionAfter->push_back(n);
                    }
                }
                if (selectionFrame || g.handle == 8 || std::abs(g.baseRotation) > 1e-12) {
                    const auto corners = transformedCorners(g.bounds, g.frameTransform);
                    SelectionFrameState nextFrame;
                    nextFrame.bounds = {g.targetPage, corners[0].x, corners[0].y, corners[0].x, corners[0].y};
                    for (const auto& point: corners) {
                        nextFrame.bounds.x0 = std::min(nextFrame.bounds.x0, point.x);
                        nextFrame.bounds.y0 = std::min(nextFrame.bounds.y0, point.y);
                        nextFrame.bounds.x1 = std::max(nextFrame.bounds.x1, point.x);
                        nextFrame.bounds.y1 = std::max(nextFrame.bounds.y1, point.y);
                    }
                    nextFrame.rotation = g.handle == 8 ? g.rotationTarget : g.baseRotation;
                    nextFrame.inkIndices = edit.selectionAfter.value_or(selected);
                    nextFrame.textIndices = edit.textSelectionAfter.value_or(selectedTexts);
                    nextFrame.imageIndices = edit.imageSelectionAfter.value_or(selectedImages);
                    edit.hasSelectionFrameState = true;
                    edit.selectionFrameBefore = selectionFrame;
                    edit.selectionFrameAfter = std::move(nextFrame);
                }
                apply(edit, true);
                record(std::move(edit));
                selectActivePage(g.targetPage);
            }
        } else {
            selectionFrame.reset();
            const auto size = pageSize(g.page);
            const auto x0 = std::min(g.start.x, g.latest.x), x1 = std::max(g.start.x, g.latest.x);
            const auto y0 = std::min(g.start.y, g.latest.y), y1 = std::max(g.start.y, g.latest.y);
            if (g.additive) selected = g.previousSelection;
            if (g.additive) selectedTexts = g.previousTextSelection;
            if (g.lasso) extendLassoAtPageEdges(g.boundary, size);
            // RectangularSelector extends at page edges (1 pt). Stroke selection
            // tests every point, rather than accepting a crossing segment.
            const auto collectLayer = [&](size_t layerId) {
                std::vector<size_t> layerSelection;
                for (size_t i = 0; i < ink.size(); ++i) {
                    if (ink[i].page == g.page && ink[i].layerId == layerId &&
                        layerVisible(g.page, ink[i].layerId) && !ink[i].points.empty() &&
                        std::all_of(ink[i].points.begin(), ink[i].points.end(), [&](const Point& p) {
                            if (g.lasso) return polygonContains(g.boundary, p);
                            return (x0 <= 1 || p.x >= x0) && (x1 >= size.width - 1 || p.x <= x1) &&
                                   (y0 <= 1 || p.y >= y0) && (y1 >= size.height - 1 || p.y <= y1);
                        })) {
                        layerSelection.push_back(i);
                    }
                }
                selected.insert(selected.end(), layerSelection.begin(), layerSelection.end());

                std::vector<size_t> textLayerSelection;
                for (size_t i = 0; i < texts.size(); ++i) {
                    const auto& text = texts[i];
                    if (text.page != g.page || text.layerId != layerId || text.text.empty() ||
                        !layerVisible(g.page, text.layerId)) {
                        continue;
                    }
                    const auto corners = textCorners(text);
                    const bool contained = std::all_of(corners.begin(), corners.end(), [&](const Point& point) {
                        if (g.lasso) {
                            return polygonContains(g.boundary, point);
                        }
                        return (x0 <= 1 || point.x >= x0) && (x1 >= size.width - 1 || point.x <= x1) &&
                               (y0 <= 1 || point.y >= y0) && (y1 >= size.height - 1 || point.y <= y1);
                    });
                    if (contained) {
                        textLayerSelection.push_back(i);
                    }
                }
                selectedTexts.insert(selectedTexts.end(), textLayerSelection.begin(), textLayerSelection.end());

                std::vector<size_t> imageLayerSelection;
                for (size_t i = 0; i < images.size(); ++i) {
                    const auto& image = images[i];
                    if (image.page != g.page || image.layerId != layerId || !layerVisible(g.page, image.layerId) ||
                        !validCanvasImage(image)) {
                        continue;
                    }
                    const auto corners = canvasImageCorners(image);
                    const bool contained = std::all_of(corners.begin(), corners.end(), [&](const Point& point) {
                        if (g.lasso) {
                            return polygonContains(g.boundary, point);
                        }
                        return (x0 <= 1 || point.x >= x0) && (x1 >= size.width - 1 || point.x <= x1) &&
                               (y0 <= 1 || point.y >= y0) && (y1 >= size.height - 1 || point.y <= y1);
                    });
                    if (contained) {
                        imageLayerSelection.push_back(i);
                    }
                }
                selectedImages.insert(selectedImages.end(), imageLayerSelection.begin(), imageLayerSelection.end());
                return !layerSelection.empty() || !textLayerSelection.empty() || !imageLayerSelection.empty();
            };
            if (g.multiLayer) {
                const auto& layers = layersForPage(g.page).layers;
                for (size_t layer = layers.size(); layer > 0; --layer) {
                    if (layers[layer - 1].visible && collectLayer(layer)) {
                        break;
                    }
                }
            } else {
                collectLayer(activeLayerId(g.page));
            }
            std::sort(selected.begin(),selected.end());
            selected.erase(std::unique(selected.begin(),selected.end()),selected.end());
            std::sort(selectedTexts.begin(), selectedTexts.end());
            selectedTexts.erase(std::unique(selectedTexts.begin(), selectedTexts.end()), selectedTexts.end());
            std::sort(selectedImages.begin(), selectedImages.end());
            selectedImages.erase(std::unique(selectedImages.begin(), selectedImages.end()), selectedImages.end());
        }
    }

    void selectAllOnPage() {
        clearSelection();
        for (size_t i = 0; i < ink.size(); ++i) {
            if (ink[i].page == page && ink[i].layerId == activeLayerId(page) &&
                layerVisible(page, ink[i].layerId) && !ink[i].points.empty()) {
                selected.push_back(i);
            }
        }
        for (size_t i = 0; i < texts.size(); ++i) {
            if (texts[i].page == page && texts[i].layerId == activeLayerId(page) &&
                layerVisible(page, texts[i].layerId) && !texts[i].text.empty()) {
                selectedTexts.push_back(i);
            }
        }
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i].page == page && images[i].layerId == activeLayerId(page) &&
                layerVisible(page, images[i].layerId) && validCanvasImage(images[i])) {
                selectedImages.push_back(i);
            }
        }
    }

    void deleteSelection() {
        cancelSelection();
        if (!hasSelection()) {
            return;
        }
        Edit edit;
        edit.selectionBefore = selected;
        edit.selectionAfter = std::vector<size_t>{};
        edit.textSelectionBefore = selectedTexts;
        edit.textSelectionAfter = std::vector<size_t>{};
        edit.imageSelectionBefore = selectedImages;
        edit.imageSelectionAfter = std::vector<size_t>{};
        edit.hasSelectionFrameState = true;
        edit.selectionFrameBefore = selectionFrame;
        for (size_t n = 0; n < selected.size(); ++n) {
            const auto i = selected[n];
            edit.changes.push_back({i, {ink.at(i)}, i - n, {}});
        }
        for (size_t n = 0; n < selectedTexts.size(); ++n) {
            const auto i = selectedTexts[n];
            edit.textChanges.push_back({i, {texts.at(i)}, i - n, {}});
        }
        for (size_t n = 0; n < selectedImages.size(); ++n) {
            const auto i = selectedImages[n];
            edit.imageChanges.push_back({i, {images.at(i)}, i - n, {}});
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

    std::vector<TextItem> copyTextSelection() const {
        std::vector<TextItem> copied;
        copied.reserve(selectedTexts.size());
        for (const auto index: selectedTexts) {
            copied.push_back(texts.at(index));
        }
        std::stable_sort(copied.begin(), copied.end(), [](const TextItem& a, const TextItem& b) {
            return a.layerOrder < b.layerOrder;
        });
        return copied;
    }

    std::vector<EditableObject> copySelectedObjects() const {
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
        std::stable_sort(copied.begin(), copied.end(), [&](const auto& lhs, const auto& rhs) {
            return stackingOrder(lhs) < stackingOrder(rhs);
        });
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
        const auto previousSelectionFrame = selectionFrame;
        SelectionBox b{targetPage, pasted[0].points[0].x,pasted[0].points[0].y,pasted[0].points[0].x,pasted[0].points[0].y};
        for (const auto& stroke: pasted) for (const auto& p: stroke.points) {
            b.x0=std::min(b.x0,p.x);b.y0=std::min(b.y0,p.y);b.x1=std::max(b.x1,p.x);b.y1=std::max(b.y1,p.y);
        }
        const auto dx = std::max(0.0,center.x-(b.x1-b.x0)/2)-b.x0;
        const auto dy = std::max(0.0,center.y-(b.y1-b.y0)/2)-b.y0;
        auto layerOrder = nextOriginOrder(targetPage, activeLayerOrigin(targetPage));
        for (auto& stroke: pasted) {
            stroke.page=targetPage;
            stroke.layerId = activeLayerId(targetPage);
            stroke.originLayer = activeLayerOrigin(targetPage);
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
        edit.hasSelectionFrameState = true;
        edit.selectionFrameBefore = previousSelectionFrame;
        apply(edit,true);record(std::move(edit));selectActivePage(targetPage);
        return true;
    }

    bool pasteTexts(std::vector<TextItem> pasted, int targetPage, Point center) {
        if (pasted.empty() || pasted.size() > 100000 || targetPage < 0 || targetPage >= pageCount() || current ||
            eraseGesture || selectionGesture || !std::isfinite(center.x) || !std::isfinite(center.y)) {
            return false;
        }

        SelectionBox bounds{targetPage, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()};
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

    bool pasteObjects(std::vector<EditableObject> pasted, int targetPage, Point center) {
        constexpr size_t maxClipboardObjects = 100000;
        if (pasted.empty() || pasted.size() > maxClipboardObjects || targetPage < 0 || targetPage >= pageCount() ||
            current || eraseGesture || selectionGesture || !std::isfinite(center.x) || !std::isfinite(center.y)) {
            return false;
        }

        SelectionBox bounds{targetPage, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()};
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

    std::optional<PageHit> hitSelectionHandle(double x, double y) const {
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

    void scrollBy(double dx, double dy, bool discrete) {
        if (!std::isfinite(dx) || !std::isfinite(dy)) {
            return;
        }
        offsetX -= behavior::scrollDeltaToPixels(dx, discrete);
        offsetY -= behavior::scrollDeltaToPixels(dy, discrete);
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
        while (pageLayers.size() < pages.size()) {
            pageLayers.emplace_back();
        }
        const int clamped = std::clamp(afterIndex, 0, pageCount() - 1);
        const int insertedIndex = clamped + 1;
        cancel();
        clearLaserPointers();
        pages.insert(pages.begin() + insertedIndex, size);
        ++pageStructureGeneration;
        pageLayers.insert(pageLayers.begin() + insertedIndex, PageLayers{});
        auto shiftStrokes = [&](std::vector<Ink>& strokes) {
            for (auto& stroke: strokes) {
                if (stroke.page >= insertedIndex) {
                    ++stroke.page;
                }
            }
        };
        shiftStrokes(ink);
        shiftTexts(texts, insertedIndex);
        shiftImages(images, insertedIndex);
        for (auto& edit: undoStates) {
            for (auto& change: edit.changes) {
                shiftStrokes(change.before);
                shiftStrokes(change.after);
            }
            for (auto& change: edit.textChanges) {
                shiftTexts(change.before, insertedIndex);
                shiftTexts(change.after, insertedIndex);
            }
            for (auto& change: edit.imageChanges) {
                shiftImages(change.before, insertedIndex);
                shiftImages(change.after, insertedIndex);
            }
            shiftLayerMutationPage(edit.layerMutation, insertedIndex);
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
            for (auto& change: edit.imageChanges) {
                shiftImages(change.before, insertedIndex);
                shiftImages(change.after, insertedIndex);
            }
            shiftLayerMutationPage(edit.layerMutation, insertedIndex);
        }
        selectActivePage(insertedIndex);
    }

    bool replaceNextPageWithClone(int sourcePage, PageSize replacementSize) {
        if (sourcePage < 0 || sourcePage + 1 >= pageCount() ||
            static_cast<size_t>(sourcePage) >= pageLayers.size()) {
            return false;
        }

        const int insertedPage = sourcePage + 1;
        auto clonedLayers = pageLayers[static_cast<size_t>(sourcePage)];
        clonedLayers.backgroundVisible = true;
        clonedLayers.backgroundName.reset();
        clonedLayers.backgroundFormat = ":pdf";
        clonedLayers.backgroundConfig.clear();
        clonedLayers.gridXOffset = clonedLayers.gridYOffset = 0;
        clonedLayers.gridXStep = clonedLayers.gridYStep = 0;
        for (auto& layer: clonedLayers.layers) {
            layer.visible = true;
        }
        std::vector<Ink> clonedInk;
        std::vector<TextItem> clonedTexts;
        std::vector<CanvasImage> clonedImages;
        for (const auto& stroke: ink) {
            if (stroke.page == sourcePage) {
                auto copy = stroke;
                copy.page = insertedPage;
                clonedInk.push_back(std::move(copy));
            }
        }
        for (const auto& text: texts) {
            if (text.page == sourcePage) {
                auto copy = text;
                copy.page = insertedPage;
                clonedTexts.push_back(std::move(copy));
            }
        }
        for (const auto& image: images) {
            if (image.page == sourcePage) {
                auto copy = image;
                copy.page = insertedPage;
                clonedImages.push_back(std::move(copy));
            }
        }

        insertPageAfter(sourcePage, replacementSize);
        pageLayers[static_cast<size_t>(insertedPage)] = clonedLayers;
        ink.insert(ink.end(), std::make_move_iterator(clonedInk.begin()), std::make_move_iterator(clonedInk.end()));
        texts.insert(texts.end(), std::make_move_iterator(clonedTexts.begin()), std::make_move_iterator(clonedTexts.end()));
        images.insert(images.end(), std::make_move_iterator(clonedImages.begin()),
                      std::make_move_iterator(clonedImages.end()));
        if (!deletePageAt(insertedPage + 1)) {
            return false;
        }
        selectActivePage(insertedPage);
        return true;
    }

    bool deletePageAt(int removeIndex) {
        if (pageCount() <= 1 || removeIndex < 0 || removeIndex >= pageCount()) {
            return false;
        }
        cancel();
        clearLaserPointers();
        clearSelection();
        pages.erase(pages.begin() + removeIndex);
        ++pageStructureGeneration;
        if (static_cast<size_t>(removeIndex) < pageLayers.size()) {
            pageLayers.erase(pageLayers.begin() + removeIndex);
        }
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
        std::erase_if(images, [&](const CanvasImage& image) { return image.page == removeIndex; });
        for (auto& image: images) {
            if (image.page > removeIndex) {
                --image.page;
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

    bool beginLaser(Device source, double x, double y, double pressure, const Brush& laserBrush,
                    bool pressureSensitive) {
        if (!isLaserPointerTool(laserBrush.tool) || laserOwner || current || currentLaser || eraseGesture ||
            selectionGesture || pinching) {
            return false;
        }
        const auto hit = hitPage(x, y, pressure);
        if (!hit) {
            return false;
        }
        auto trail = std::find_if(laserTrails.begin(), laserTrails.end(),
                                  [&](const LaserTrail& item) { return item.page == hit->page; });
        if (trail == laserTrails.end()) {
            laserTrails.push_back({hit->page, {}, 255, std::nullopt});
            trail = std::prev(laserTrails.end());
        }
        trail->alpha = 255;
        trail->fadeStart.reset();
        selectActivePage(hit->page);
        currentLaser = Ink{hit->page, {hit->local}, laserBrush, false};
        currentLaser->pressureSensitive = laserBrush.tool == Tool::LaserPointerPen && pressureSensitive;
        currentLaser->layerId = activeLayerId(hit->page);
        currentLaser->originLayer = activeLayerOrigin(hit->page);
        laserOwner = source;
        return true;
    }

    void appendLaser(Device source, double x, double y, double pressure) {
        if (!currentLaser || !laserOwner || source != *laserOwner || !std::isfinite(x) || !std::isfinite(y)) {
            return;
        }
        const auto point = pageLocalPoint(currentLaser->page, x, y, pressure);
        if (!containsOnPage(currentLaser->page, point)) {
            return;
        }
        if (std::hypot(point.x - currentLaser->points.back().x, point.y - currentLaser->points.back().y) <
            behavior::motionThreshold) {
            return;
        }
        currentLaser->points.push_back(point);
    }

    void finishLaser(Device source, std::optional<double> releasePressure = std::nullopt,
                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        if (!currentLaser || !laserOwner || source != *laserOwner) {
            return;
        }
        auto trail = std::find_if(laserTrails.begin(), laserTrails.end(),
                                  [&](const LaserTrail& item) { return item.page == currentLaser->page; });
        if (trail == laserTrails.end()) {
            laserTrails.push_back({currentLaser->page, {}, 255, std::nullopt});
            trail = std::prev(laserTrails.end());
        }
        if (currentLaser->points.size() == 1) {
            if (releasePressure && currentLaser->pressureSensitive && std::isfinite(*releasePressure)) {
                currentLaser->points.front().pressure =
                        std::max(currentLaser->points.front().pressure, *releasePressure);
            }
            currentLaser->points.push_back(currentLaser->points.front());
        }
        trail->strokes.push_back(std::move(*currentLaser));
        trail->alpha = 255;
        trail->fadeStart = now + std::chrono::milliseconds(laserFadeOutTimeMs);
        currentLaser.reset();
        laserOwner.reset();
    }

    void cancelLaser(Device source, std::chrono::steady_clock::time_point now =
                                             std::chrono::steady_clock::now()) {
        if (!laserOwner || source != *laserOwner) {
            return;
        }
        const int pageIndex = currentLaser ? currentLaser->page : -1;
        currentLaser.reset();
        laserOwner.reset();
        auto trail = std::find_if(laserTrails.begin(), laserTrails.end(),
                                  [&](const LaserTrail& item) { return item.page == pageIndex; });
        if (trail == laserTrails.end()) {
            return;
        }
        if (trail->strokes.empty()) {
            laserTrails.erase(trail);
        } else {
            trail->alpha = 255;
            trail->fadeStart = now + std::chrono::milliseconds(laserFadeOutTimeMs);
        }
    }

    bool hasLaserFades() const {
        return std::any_of(laserTrails.begin(), laserTrails.end(),
                           [](const LaserTrail& trail) { return trail.fadeStart.has_value(); });
    }

    bool advanceLaserFades(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        bool changed = false;
        for (auto& trail: laserTrails) {
            if (!trail.fadeStart || now < *trail.fadeStart) {
                continue;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *trail.fadeStart);
            const auto steps = static_cast<unsigned long long>(elapsed.count()) / behavior::laserPointerFadeStepMs;
            const auto faded = std::min<unsigned long long>(255, steps * behavior::laserPointerFadeAlphaStep);
            const auto nextAlpha = static_cast<uint8_t>(255 - faded);
            changed = changed || nextAlpha != trail.alpha;
            trail.alpha = nextAlpha;
        }
        const auto oldSize = laserTrails.size();
        std::erase_if(laserTrails, [](const LaserTrail& trail) { return trail.alpha == 0; });
        return changed || oldSize != laserTrails.size();
    }

    void clearLaserPointers() {
        currentLaser.reset();
        laserTrails.clear();
        laserOwner.reset();
    }

    bool begin(Device source, double x, double y, double pressure = 1, bool eraserEnd = false,
               bool shift = false, bool control = false, bool alt = false) {
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
            if (isFillableTool(brush.tool) && brush.fillEnabled) {
                current->fill = std::clamp(brush.fillOpacity, 0, 255);
            }
        }
        current->layerId = activeLayerId(hit->page);
        current->originLayer = activeLayerOrigin(hit->page);
        rawCurrent = {hit->local};
        if (!useEraser && drawingMode == DrawingMode::Spline) {
            splineKnots = {hit->local};
            splineTangents = {{0, 0}};
            splineHover = hit->local;
            current->points = splineBezierPoints(splineKnots, splineTangents);
        }
        currentShift = shift;
        currentControl = control;
        currentAlt = alt;
        return true;
    }

    void append(Device source, double x, double y, double pressure = 1, bool shift = false, bool control = false,
                bool alt = false) {
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
            rawCurrent.push_back(p);
            currentShift = shift;
            currentControl = control;
            currentAlt = alt;
            if (current->brush.tool == Tool::Eraser || drawingMode == DrawingMode::Freehand ||
                drawingMode == DrawingMode::ShapeRecognizer) {
                current->points.push_back(p);
            } else {
                current->points = makeDrawingPath(drawingMode, rawCurrent.front(), p, current->brush.width, shift,
                                                  control, alt, rawCurrent);
            }
        }
    }

    void refreshCurrentShape(bool shift, bool control, bool alt) {
        if (!current || current->brush.tool == Tool::Eraser || rawCurrent.empty()) return;
        currentShift = shift;
        currentControl = control;
        currentAlt = alt;
        if (drawingMode == DrawingMode::Spline) {
            refreshSplinePreview();
        } else if (drawingMode != DrawingMode::Freehand && drawingMode != DrawingMode::ShapeRecognizer) {
            current->points = makeDrawingPath(drawingMode, rawCurrent.front(), rawCurrent.back(), current->brush.width,
                                              shift, control, alt, rawCurrent);
        }
    }

    bool addSplineKnot(double x, double y, double pressure = 1) {
        if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty()) return false;
        const auto hit = hitPage(x, y);
        if (!hit || hit->page != current->page) return false;
        auto point = hit->local;
        point.pressure = std::isfinite(pressure) && pressure >= 0 ? pressure : 1;
        splineKnots.push_back(point);
        splineTangents.push_back({0, 0});
        splineHover = point;
        refreshSplinePreview();
        return true;
    }

    void moveSplineTangent(double x, double y, double pressure = 1) {
        if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty()) return;
        const auto point = pageLocalPoint(current->page, x, y);
        if (!containsOnPage(current->page, point)) return;
        auto& knot = splineKnots.back();
        splineTangents.back() = {point.x - knot.x, point.y - knot.y};
        splineHover = Point{point.x, point.y, std::isfinite(pressure) && pressure >= 0 ? pressure : 1};
        refreshSplinePreview();
    }

    void updateSplineHover(double x, double y) {
        if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty()) return;
        const auto point = pageLocalPoint(current->page, x, y);
        if (containsOnPage(current->page, point)) {
            splineHover = point;
            refreshSplinePreview();
        }
    }

    bool removeLastSplineKnot() {
        if (!current || drawingMode != DrawingMode::Spline || splineKnots.size() <= 1) return false;
        splineKnots.pop_back();
        splineTangents.pop_back();
        splineHover = splineKnots.back();
        refreshSplinePreview();
        return true;
    }

    bool closeSplinePath() {
        if (!current || drawingMode != DrawingMode::Spline || splineKnots.size() < 2) return false;
        const auto& first = splineKnots.front();
        const auto& last = splineKnots.back();
        if (std::hypot(first.x - last.x, first.y - last.y) > 1e-8) {
            splineKnots.push_back(first);
            splineTangents.push_back(splineTangents.front());
        }
        splineHover.reset();
        refreshSplinePreview();
        return true;
    }

    bool moveLastSplineKnot(double dx, double dy) {
        if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty()) return false;
        splineKnots.back().x += dx;
        splineKnots.back().y += dy;
        splineHover = splineKnots.back();
        refreshSplinePreview();
        return true;
    }

    bool rotateLastSplineTangent(double degrees) {
        if (!current || drawingMode != DrawingMode::Spline || splineTangents.empty()) return false;
        const double angle = degrees * M_PI / 180.0;
        const auto old = splineTangents.back();
        splineTangents.back() = {std::cos(angle) * old.x + std::sin(angle) * old.y,
                                 -std::sin(angle) * old.x + std::cos(angle) * old.y};
        refreshSplinePreview();
        return true;
    }

    bool scaleLastSplineTangent(double factor) {
        if (!current || drawingMode != DrawingMode::Spline || splineTangents.empty()) return false;
        splineTangents.back().x *= factor;
        splineTangents.back().y *= factor;
        refreshSplinePreview();
        return true;
    }

    void finish(Device source, std::optional<double> releasePressure = std::nullopt) {
        if (current && source == owner) {
            if (current->brush.tool != Tool::Eraser && drawingMode == DrawingMode::Spline) {
                if (splineKnots.size() < 2) {
                    current.reset();
                    rawCurrent.clear();
                    splineKnots.clear();
                    splineTangents.clear();
                    splineHover.reset();
                    return;
                }
                current->points = splineBezierPoints(splineKnots, splineTangents);
            }
            if (current->brush.tool != Tool::Eraser && drawingMode == DrawingMode::ShapeRecognizer) {
                if (auto recognized = recognizeShape(rawCurrent, shapeRecognizerMinSize)) {
                    current->points = std::move(*recognized);
                }
            }
            if (current->brush.tool != Tool::Eraser && drawingMode != DrawingMode::Freehand &&
                current->points.size() < 2) {
                current.reset();
                rawCurrent.clear();
                return;
            }
            if (current->points.size() == 1) {
                const auto point = current->points.front();
                if (releasePressure && std::isfinite(*releasePressure) && current->brush.tool == Tool::Pen) {
                    current->points.front().pressure = std::max(point.pressure, *releasePressure);
                }
                current->points.push_back(point);
            }
            if ((current->brush.tool == Tool::Pen || current->brush.tool == Tool::Highlighter ||
                 current->brush.tool == Tool::Eraser) && current->pluginReference == 0) {
                current->pluginReference = allocatePluginReference();
            }
            current->layerOrder = nextOriginOrder(current->page, current->originLayer);
            Edit edit;
            edit.changes.push_back({ink.size(), {}, ink.size(), {*current}});
            record(std::move(edit));
            ink.push_back(std::move(*current));
            current.reset();
            rawCurrent.clear();
            splineKnots.clear();
            splineTangents.clear();
            splineHover.reset();
        }
    }

    bool hasSelectedFillableInk() const {
        return std::any_of(selected.begin(), selected.end(), [&](size_t index) {
            return index < ink.size() && isFillableTool(ink[index].brush.tool);
        });
    }

    bool setSelectedInkFill(int penOpacity, int highlighterOpacity) {
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

    bool applyInkEdits(std::vector<std::pair<size_t, Ink>> replacements, std::vector<Ink> additions) {
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
                replacement.capStyle != original.capStyle) {
                return false;
            }
        }

        std::map<std::pair<int, size_t>, size_t> nextOrders;
        for (auto& addition: additions) {
            if (addition.page < 0 || addition.page >= pageCount() || addition.points.size() < 2 ||
                !std::isfinite(addition.brush.width) || addition.brush.width <= 0 || addition.fill < -1 ||
                addition.fill > 255 || addition.capStyle < 0 || addition.capStyle > 2 ||
                (addition.fill >= 0 && !isFillableTool(addition.brush.tool))) {
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

    bool insertText(TextItem item) {
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

    bool replaceText(size_t index, TextItem item) {
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

    bool replaceTextWithImage(size_t index, CanvasImage image) {
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

    template <typename Update>
    bool updateSelectedTexts(Update&& update) {
        if (selectedTexts.empty() || current || eraseGesture || selectionGesture) {
            return false;
        }

        auto indices = selectedTexts;
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

        Edit edit;
        edit.selectionBefore = edit.selectionAfter = selected;
        edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
        edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
        for (const auto index: indices) {
            if (index >= texts.size()) {
                return false;
            }
            auto replacement = texts[index];
            update(replacement);
            if (!validTextPage(replacement) || replacement.page != texts[index].page) {
                return false;
            }
            if (replacement != texts[index]) {
                edit.textChanges.push_back({index, {texts[index]}, index, {std::move(replacement)}});
            }
        }
        if (edit.textChanges.empty()) {
            return false;
        }

        apply(edit, true);
        record(std::move(edit));
        return true;
    }

    bool recolorSelectedTexts(uint32_t color) {
        return updateSelectedTexts([color](TextItem& item) {
            item.color = color;
            for (auto& run: item.styleRuns) {
                run.color = color;
            }
        });
    }

    bool recolorSelection(const std::array<double, 3>& color, uint32_t textColor) {
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

    bool deleteText(size_t index) {
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

    bool insertImage(CanvasImage image) {
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

    bool pasteImages(std::vector<CanvasImage> pasted, int targetPage, Point center) {
        if (pasted.empty() || pasted.size() > maxCanvasImageObjects || targetPage < 0 ||
            targetPage >= pageCount() || current || eraseGesture || selectionGesture ||
            !std::isfinite(center.x) || !std::isfinite(center.y)) {
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
            if (!validCanvasImage(image) || !std::isfinite(image.transform.x) ||
                !std::isfinite(image.transform.y)) {
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

    bool replaceImage(size_t index, CanvasImage image) {
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

    bool replaceImageGroup(std::vector<std::pair<size_t, std::vector<CanvasImage>>> replacements) {
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
                   lhs.pixelHeight == rhs.pixelHeight && lhs.kind == rhs.kind &&
                   lhs.nativeWidth == rhs.nativeWidth && lhs.nativeHeight == rhs.nativeHeight &&
                   lhs.encodedData == rhs.encodedData && lhs.latexSource == rhs.latexSource &&
                   lhs.transform.xx == rhs.transform.xx && lhs.transform.yx == rhs.transform.yx &&
                   lhs.transform.xy == rhs.transform.xy && lhs.transform.yy == rhs.transform.yy &&
                   lhs.transform.x == rhs.transform.x && lhs.transform.y == rhs.transform.y;
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

    bool deleteImage(size_t index) {
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

    std::optional<size_t> pickImage(int pageIndex, Point point) const {
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
            if (point.x < bounds.x0 || point.x > bounds.x1 || point.y < bounds.y0 || point.y > bounds.y1 ||
                localX < 0 || localX > canvasImageWidth(image) || localY < 0 ||
                localY > canvasImageHeight(image)) {
                continue;
            }
            if (!best || image.layerOrder >= bestOrder) {
                best = index;
                bestOrder = image.layerOrder;
            }
        }
        return best;
    }

    bool selectObjectAt(int pageIndex, Point point, bool additive = false) {
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

    std::optional<SelectableObject> pickSelectableObject(int pageIndex, Point point) const {
        const size_t layerId = activeLayerId(pageIndex);
        if (!layerVisible(pageIndex, layerId) || layerId == 0) {
            return std::nullopt;
        }

        const auto& layer = layersForPage(pageIndex).layers.at(layerId - 1);
        struct OrderedObject {
            size_t originOrder;
            size_t layerOrder;
            SelectableObject object;
        };
        std::vector<OrderedObject> objects;
        const auto addObject = [&](size_t origin, size_t order, SelectableObject object) {
            const auto it = std::find(layer.origins.begin(), layer.origins.end(), origin);
            const auto originOrder = it == layer.origins.end() ? layer.origins.size() :
                                                                  static_cast<size_t>(it - layer.origins.begin());
            objects.push_back({originOrder, order, object});
        };
        for (size_t i = 0; i < ink.size(); ++i) {
            const auto& stroke = ink[i];
            if (stroke.page == pageIndex && stroke.layerId == layerId && !stroke.points.empty()) {
                addObject(stroke.originLayer, stroke.layerOrder, {SelectableObjectKind::Stroke, i});
            }
        }
        for (size_t i = 0; i < texts.size(); ++i) {
            const auto& text = texts[i];
            if (text.page == pageIndex && text.layerId == layerId && !text.text.empty() && validTextPage(text)) {
                addObject(text.originLayer, text.layerOrder, {SelectableObjectKind::Text, i});
            }
        }
        for (size_t i = 0; i < images.size(); ++i) {
            const auto& image = images[i];
            if (image.page == pageIndex && image.layerId == layerId && validCanvasImage(image)) {
                addObject(image.originLayer, image.layerOrder, {SelectableObjectKind::Image, i});
            }
        }
        std::stable_sort(objects.begin(), objects.end(), [](const OrderedObject& a, const OrderedObject& b) {
            return std::tie(a.originOrder, a.layerOrder) < std::tie(b.originOrder, b.layerOrder);
        });

        constexpr double actionRadius = 0.5;
        double minimumDistance = actionRadius;
        std::optional<SelectableObject> best;
        for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
            double distance = std::numeric_limits<double>::infinity();
            switch (it->object.kind) {
                case SelectableObjectKind::Stroke:
                    distance = strokeDistance(ink[it->object.index], point);
                    break;
                case SelectableObjectKind::Text:
                    distance = quadrilateralDistance(textCorners(texts[it->object.index]), point);
                    break;
                case SelectableObjectKind::Image:
                    distance = quadrilateralDistance(canvasImageCorners(images[it->object.index]), point);
                    break;
            }
            if (distance == 0) {
                return it->object;
            }
            if (distance < minimumDistance) {
                minimumDistance = distance;
                best = it->object;
            }
        }
        return best;
    }

    void cancel() {
        cancelSelection();
        cancelVerticalSpace();
        finishErase();
        if (laserOwner) {
            cancelLaser(*laserOwner);
        }
        current.reset();
        rawCurrent.clear();
        splineKnots.clear();
        splineTangents.clear();
        splineHover.reset();
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
    size_t revision() const { return undoStates.empty() ? revisionBase : undoStates.back().revision; }

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
    void refreshSplinePreview() {
        if (current && drawingMode == DrawingMode::Spline) {
            current->points = splineBezierPoints(splineKnots, splineTangents, splineHover);
        }
    }

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

    struct ImageChange {
        size_t beforeIndex = 0;
        std::vector<CanvasImage> before;
        size_t afterIndex = 0;
        std::vector<CanvasImage> after;
    };

    struct PageSizeChange {
        int page = 0;
        PageSize before;
        PageSize after;
    };

    template <typename T>
    struct IndexedObject {
        size_t index;
        T value;
    };

    struct LayerMutation {
        int page = 0;
        PageLayers beforeLayers;
        PageLayers afterLayers;
        std::vector<IndexedObject<Ink>> beforeInk;
        std::vector<IndexedObject<Ink>> afterInk;
        std::vector<IndexedObject<TextItem>> beforeTexts;
        std::vector<IndexedObject<TextItem>> afterTexts;
        std::vector<IndexedObject<CanvasImage>> beforeImages;
        std::vector<IndexedObject<CanvasImage>> afterImages;
    };

    struct SelectionFrameState {
        SelectionBox bounds{};
        double rotation = 0;
        std::vector<size_t> inkIndices;
        std::vector<size_t> textIndices;
        std::vector<size_t> imageIndices;
    };

    struct Edit {
        std::vector<Change> changes;
        std::vector<TextChange> textChanges;
        std::vector<ImageChange> imageChanges;
        std::optional<LayerMutation> layerMutation;
        std::optional<PageSizeChange> pageSizeChange;
        size_t revision = 0;
        std::optional<std::vector<size_t>> selectionBefore = std::nullopt;
        std::optional<std::vector<size_t>> selectionAfter = std::nullopt;
        std::optional<std::vector<size_t>> textSelectionBefore = std::nullopt;
        std::optional<std::vector<size_t>> textSelectionAfter = std::nullopt;
        std::optional<std::vector<size_t>> imageSelectionBefore = std::nullopt;
        std::optional<std::vector<size_t>> imageSelectionAfter = std::nullopt;
        bool hasSelectionFrameState = false;
        std::optional<SelectionFrameState> selectionFrameBefore;
        std::optional<SelectionFrameState> selectionFrameAfter;
    };

    bool validTextPage(const TextItem& item) const {
        return item.page >= 0 && item.page < pageCount() && std::isfinite(item.fontSize) && item.fontSize > 0 &&
               std::isfinite(item.wrapWidth) && std::isfinite(item.xx) && std::isfinite(item.yx) &&
               std::isfinite(item.xy) && std::isfinite(item.yy) && std::isfinite(item.x) &&
               std::isfinite(item.y) && std::isfinite(item.layoutWidth) && item.layoutWidth >= 0 &&
               std::isfinite(item.layoutHeight) && item.layoutHeight >= 0;
    }

    size_t nextOriginOrder(int pageIndex, size_t origin) const {
        size_t next = 0;
        for (const auto& stroke: ink) {
            if (stroke.page == pageIndex && stroke.originLayer == origin) {
                next = std::max(next, stroke.layerOrder + 1);
            }
        }
        for (const auto& text: texts) {
            if (text.page == pageIndex && text.originLayer == origin) {
                next = std::max(next, text.layerOrder + 1);
            }
        }
        for (const auto& image: images) {
            if (image.page == pageIndex && image.originLayer == origin) {
                next = std::max(next, image.layerOrder + 1);
            }
        }
        return next;
    }

    static size_t nextLayerOrigin(const PageLayers& pageState) {
        size_t next = pageState.initialLayerCount;
        for (const auto& layer: pageState.layers) {
            for (size_t origin: layer.origins) {
                if (origin != newLayerOrigin && origin >= next) {
                    next = origin + 1;
                }
            }
        }
        return next;
    }

    LayerMutation captureLayerMutation(int pageIndex) const {
        LayerMutation mutation;
        mutation.page = pageIndex;
        mutation.beforeLayers = pageLayers.at(static_cast<size_t>(pageIndex));
        for (size_t i = 0; i < ink.size(); ++i) {
            if (ink[i].page == pageIndex) {
                mutation.beforeInk.push_back({i, ink[i]});
            }
        }
        for (size_t i = 0; i < texts.size(); ++i) {
            if (texts[i].page == pageIndex) {
                mutation.beforeTexts.push_back({i, texts[i]});
            }
        }
        for (size_t i = 0; i < images.size(); ++i) {
            if (images[i].page == pageIndex) {
                mutation.beforeImages.push_back({i, images[i]});
            }
        }
        return mutation;
    }

    void recordLayerMutation(LayerMutation mutation) {
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

    template <typename T>
    static void restoreIndexedObjects(std::vector<T>& objects, int pageIndex,
                                      const std::vector<IndexedObject<T>>& snapshot) {
        std::erase_if(objects, [pageIndex](const T& item) { return item.page == pageIndex; });
        for (const auto& indexed: snapshot) {
            const auto at = std::min(indexed.index, objects.size());
            objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(at), indexed.value);
        }
    }

    void remapObjectsAfterLayerRemoval(int pageIndex, size_t deletedId) {
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

    static void shiftTexts(std::vector<TextItem>& items, int insertedIndex) {
        for (auto& item: items) {
            if (item.page >= insertedIndex) {
                ++item.page;
            }
        }
    }

    static void shiftImages(std::vector<CanvasImage>& items, int insertedIndex) {
        for (auto& item: items) {
            if (item.page >= insertedIndex) {
                ++item.page;
            }
        }
    }

    static void shiftLayerMutationPage(std::optional<LayerMutation>& mutation, int insertedIndex) {
        if (!mutation || mutation->page < insertedIndex) {
            return;
        }
        ++mutation->page;
        for (auto& indexed: mutation->beforeInk) {
            ++indexed.value.page;
        }
        for (auto& indexed: mutation->afterInk) {
            ++indexed.value.page;
        }
        for (auto& indexed: mutation->beforeTexts) {
            ++indexed.value.page;
        }
        for (auto& indexed: mutation->afterTexts) {
            ++indexed.value.page;
        }
        for (auto& indexed: mutation->beforeImages) {
            ++indexed.value.page;
        }
        for (auto& indexed: mutation->afterImages) {
            ++indexed.value.page;
        }
    }

    struct SelectionGesture {
        int page = 0;
        Point start{}, latest{};
        bool moving = false;
        bool lasso = false, additive = false;
        bool multiLayer = false;
        int handle = -1, targetPage = 0;
        Point snapAnchor{};
        double baseRotation = 0;
        double rotationTarget = 0;
        std::vector<size_t> previousSelection;
        std::vector<size_t> previousTextSelection;
        std::vector<size_t> previousImageSelection;
        std::optional<SelectionFrameState> previousSelectionFrame;
        std::vector<size_t> indices;
        std::vector<size_t> textIndices;
        std::vector<size_t> imageIndices;
        SelectionBox bounds{};
        SelectionTransform transform;
        SelectionTransform frameTransform;
        std::vector<Point> boundary;
    };

    struct EraseGesture {
        std::vector<size_t> slotOrigin;
        std::map<size_t, Ink> touchedOriginals;
    };

    static Point lerpPoint(const Point& a, const Point& b, double t) {
        return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.pressure + t * (b.pressure - a.pressure)};
    }

    static std::array<Point, 4> textCorners(const TextItem& item) {
        const auto transform = [&](double x, double y) {
            return Point{item.xx * x + item.xy * y + item.x, item.yx * x + item.yy * y + item.y};
        };
        return {transform(0, 0), transform(item.layoutWidth, 0),
                transform(item.layoutWidth, item.layoutHeight), transform(0, item.layoutHeight)};
    }

    static std::optional<SelectionBox> inkBounds(const Ink& stroke) {
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

    double snapVertical(int pageIndex, double y) const {
        if (!std::isfinite(y) || !std::isfinite(snapGridSize) || !std::isfinite(snapGridTolerance) ||
            !snapGridEnabled || snapGridSize <= 0 || snapGridTolerance <= 0) {
            return y;
        }
        const double offset = layersForPage(pageIndex).gridYOffset;
        const double snapped = y - std::remainder(y - offset, snapGridSize);
        return std::abs(snapped - y) < snapGridTolerance * snapGridSize / 2.0 ? snapped : y;
    }

    static Point closestSelectionCorner(const SelectionBox& bounds, Point point) {
        const std::array<Point, 4> corners{{{bounds.x0, bounds.y0}, {bounds.x1, bounds.y0},
                                            {bounds.x1, bounds.y1}, {bounds.x0, bounds.y1}}};
        return *std::min_element(corners.begin(), corners.end(), [&](const Point& a, const Point& b) {
            return std::hypot(a.x - point.x, a.y - point.y) < std::hypot(b.x - point.x, b.y - point.y);
        });
    }

    Point snapSelectionMove(int pageIndex, Point point, bool altHeld) const {
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

    double snapSelectionResizeAxis(int pageIndex, double value, bool horizontal, bool altHeld) const {
        if (!std::isfinite(value) || !std::isfinite(snapGridSize) || !std::isfinite(snapGridTolerance) ||
            snapGridSize <= 0 || snapGridTolerance <= 0 || altHeld == snapGridEnabled) {
            return value;
        }
        const auto& pageGrid = layersForPage(pageIndex);
        const auto offset = horizontal ? pageGrid.gridXOffset : pageGrid.gridYOffset;
        const double snapped = value - std::remainder(value - offset, snapGridSize);
        return std::abs(snapped - value) < snapGridTolerance * snapGridSize / 2.0 ? snapped : value;
    }

    void collectVerticalSpaceObjects(VerticalSpacePreview& preview) const {
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
            if (text.page != preview.page || text.layerId != preview.layerId || text.text.empty() ||
                !validTextPage(text)) {
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
            if ((preview.moveAbove && bounds.y1 <= preview.startY) ||
                (!preview.moveAbove && bounds.y0 >= preview.startY)) {
                preview.imageIndices.push_back(index);
            }
        }
    }

    static double strokeDistance(const Ink& stroke, Point point) {
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
            if (!std::isfinite(length) || !std::isfinite(start.x) || !std::isfinite(start.y) ||
                !std::isfinite(end.x) || !std::isfinite(end.y) || !std::isfinite(start.pressure)) {
                return std::numeric_limits<double>::infinity();
            }
            if (length == 0) {
                if (stroke.pressureSensitive || stroke.capStyle == 0) {
                    const double width = stroke.brush.width *
                                         (stroke.pressureSensitive ? std::max(0.0, start.pressure) : 1.0);
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
                double current = std::hypot(std::max({capStart - along, along - capEnd, 0.0}),
                                            std::max(across - halfWidth, 0.0));
                if (!atStart) {
                    current = std::min(current,
                                       std::max(std::hypot(point.x - start.x, point.y - start.y) - halfWidth, 0.0));
                }
                if (!atEnd) {
                    current = std::min(current,
                                       std::max(std::hypot(point.x - end.x, point.y - end.y) - halfWidth, 0.0));
                }
                distance = std::min(distance, current);
                continue;
            }

            const double factor = std::clamp(along / length, 0.0, 1.0);
            const double width = stroke.brush.width *
                                 (stroke.pressureSensitive ? std::max(0.0, start.pressure) : 1.0);
            const double projectedX = start.x + factor * dx;
            const double projectedY = start.y + factor * dy;
            distance = std::clamp(std::hypot(point.x - projectedX, point.y - projectedY) - width / 2.0, 0.0,
                                  distance);
        }
        return distance;
    }

    static double quadrilateralDistance(const std::array<Point, 4>& corners, Point point) {
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
        const double projectedX = origin.x + std::clamp(localX, 0.0, 1.0) * hx +
                                  std::clamp(localY, 0.0, 1.0) * vx;
        const double projectedY = origin.y + std::clamp(localX, 0.0, 1.0) * hy +
                                  std::clamp(localY, 0.0, 1.0) * vy;
        return std::hypot(point.x - projectedX, point.y - projectedY);
    }

    static TextItem transformText(TextItem item, const SelectionTransform& transform) {
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

    static CanvasImage transformImage(CanvasImage image, const SelectionTransform& transform) {
        const auto source = image.transform;
        image.transform = {transform.xx * source.xx + transform.xy * source.yx,
                           transform.yx * source.xx + transform.yy * source.yx,
                           transform.xx * source.xy + transform.xy * source.yy,
                           transform.yx * source.xy + transform.yy * source.yy,
                           transform.xx * source.x + transform.xy * source.y + transform.x0,
                           transform.yx * source.x + transform.yy * source.y + transform.y0};
        return image;
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
            Ink piece{stroke.page, std::move(pts), stroke.brush, stroke.pressureSensitive, stroke.layerOrder};
            piece.layerId = stroke.layerId;
            piece.originLayer = stroke.originLayer;
            return piece;
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

    void record(Edit edit) {
        redoStates.clear();
        edit.revision = ++nextRevision;
        // Keep ink and text deltas, rather than a full document copy for each edit.
        undoStates.push_back(std::move(edit));
    }

    void apply(const Edit& edit, bool forward) {
        std::optional<int> changedBackgroundPage;
        if (edit.pageSizeChange) {
            const auto& change = *edit.pageSizeChange;
            if (change.page >= 0 && static_cast<size_t>(change.page) < pages.size()) {
                pages[static_cast<size_t>(change.page)] = forward ? change.after : change.before;
                if (change.page == page) {
                    pageWidth = pages[static_cast<size_t>(page)].width;
                    pageHeight = pages[static_cast<size_t>(page)].height;
                }
            }
        }
        if (edit.layerMutation) {
            const auto& mutation = *edit.layerMutation;
            const auto& layers = forward ? mutation.afterLayers : mutation.beforeLayers;
            auto& pageState = pageLayers.at(static_cast<size_t>(mutation.page));
            if (pageState.backgroundFormat != layers.backgroundFormat ||
                pageState.backgroundConfig != layers.backgroundConfig) {
                changedBackgroundPage = mutation.page;
            }
            pageState = layers;
            restoreIndexedObjects(ink, mutation.page, forward ? mutation.afterInk : mutation.beforeInk);
            restoreIndexedObjects(texts, mutation.page, forward ? mutation.afterTexts : mutation.beforeTexts);
            restoreIndexedObjects(images, mutation.page, forward ? mutation.afterImages : mutation.beforeImages);
        }
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
        for (auto it = edit.imageChanges.rbegin(); it != edit.imageChanges.rend(); ++it) {
            if (forward) {
                const auto pos = images.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex);
                images.erase(pos, pos + static_cast<std::ptrdiff_t>(it->before.size()));
                images.insert(images.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex), it->after.begin(),
                              it->after.end());
            } else {
                const auto pos = images.begin() + static_cast<std::ptrdiff_t>(it->afterIndex);
                images.erase(pos, pos + static_cast<std::ptrdiff_t>(it->after.size()));
                images.insert(images.begin() + static_cast<std::ptrdiff_t>(it->afterIndex), it->before.begin(),
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
        const auto oldSelection = selected;
        const auto oldTextSelection = selectedTexts;
        const auto oldImageSelection = selectedImages;
        selected = (forward ? edit.selectionAfter : edit.selectionBefore).value_or(std::vector<size_t>{});
        selectedTexts = (forward ? edit.textSelectionAfter : edit.textSelectionBefore).value_or(std::vector<size_t>{});
        selectedImages = (forward ? edit.imageSelectionAfter : edit.imageSelectionBefore).value_or(std::vector<size_t>{});
        if (edit.hasSelectionFrameState) {
            selectionFrame = forward ? edit.selectionFrameAfter : edit.selectionFrameBefore;
        } else if (selected != oldSelection || selectedTexts != oldTextSelection ||
                   selectedImages != oldImageSelection) {
            selectionFrame.reset();
        }
        if (changedBackgroundPage && backgroundChangedCallback) {
            backgroundChangedCallback(*changedBackgroundPage);
        }
    }

    Device owner = Device::Mouse;
    size_t nextRevision = 0;
    size_t revisionBase = 0;
    std::vector<Edit> undoStates;
    std::vector<Edit> redoStates;
    std::optional<EraseGesture> eraseGesture;
    std::optional<SelectionGesture> selectionGesture;
    std::optional<SelectionFrameState> selectionFrame;
    std::optional<VerticalSpacePreview> verticalSpace;
    std::optional<Device> laserOwner;
    std::uintptr_t nextPluginReference = 1;

    std::uintptr_t allocatePluginReference() {
        if (nextPluginReference == 0) {
            throw std::overflow_error("Lua element reference space exhausted");
        }
        const auto reference = nextPluginReference;
        nextPluginReference = reference == std::numeric_limits<std::uintptr_t>::max() ? 0 : reference + 1;
        return reference;
    }
};

}  // namespace prototype
