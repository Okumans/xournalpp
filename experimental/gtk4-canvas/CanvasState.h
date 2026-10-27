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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "BehaviorDefaults.h"
#include "CanvasDrawing.h"
#include "CanvasImage.h"
#include "CanvasLineStyle.h"
#include "CanvasLayers.h"
#include "CanvasPageLayout.h"
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
    LineDashPattern lineDashes;

    Brush() = default;
    Brush(Tool tool, std::array<double, 3> color, double width, bool fillEnabled = false, int fillOpacity = 128,
          LineDashPattern lineDashes = {}):
            tool(tool), color(color), width(width), fillEnabled(fillEnabled), fillOpacity(fillOpacity),
            lineDashes(std::move(lineDashes)) {}
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
    mutable std::uint64_t rasterCacheKey = 0;
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
    CanvasLayoutSettings layoutSettings;
    CanvasPageLayout pageLayout;
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
    std::function<void(const std::vector<std::string>&)> historyChangedCallback;

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
        const auto bounds = selectionLocalBounds();
        if (!bounds) {
            return std::nullopt;
        }
        if (selectionGesture && selectionGesture->moving) {
            auto corners = transformedCorners(*bounds, selectionGesture->baseFrameTransform);
            for (auto& point: corners) {
                point = selectionGesture->transform.apply(point);
            }
            return boundsForCorners(selectionGesture->targetPage, corners);
        }
        if (selectionFrameMatchesCurrentSelection()) {
            return boundsForCorners(bounds->page, transformedCorners(*bounds, selectionFrame->transform));
        }
        return bounds;
    }

    std::optional<SelectionBox> selectionLocalBounds() const {
        if (selectionGesture && selectionGesture->moving) {
            return selectionGesture->bounds;
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

    std::optional<int> movingSelectionHandle() const {
        return movingSelection() ? std::optional<int>{selectionGesture->handle} : std::nullopt;
    }

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

    static SelectionTransform composeSelectionTransforms(const SelectionTransform& outer,
                                                          const SelectionTransform& inner) {
        return {outer.xx * inner.xx + outer.xy * inner.yx, outer.yx * inner.xx + outer.yy * inner.yx,
                outer.xx * inner.xy + outer.xy * inner.yy, outer.yx * inner.xy + outer.yy * inner.yy,
                outer.xx * inner.x0 + outer.xy * inner.y0 + outer.x0,
                outer.yx * inner.x0 + outer.yy * inner.y0 + outer.y0,
                outer.widthScale * inner.widthScale};
    }

    static SelectionTransform inverseSelectionTransform(const SelectionTransform& transform) {
        const double determinant = transform.xx * transform.yy - transform.xy * transform.yx;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
            const double invalid = std::numeric_limits<double>::quiet_NaN();
            return {invalid, invalid, invalid, invalid, invalid, invalid, invalid};
        }
        const double xx = transform.yy / determinant;
        const double xy = -transform.xy / determinant;
        const double yx = -transform.yx / determinant;
        const double yy = transform.xx / determinant;
        return {xx, yx, xy, yy, -(xx * transform.x0 + xy * transform.y0),
                -(yx * transform.x0 + yy * transform.y0),
                transform.widthScale > 0 ? 1 / transform.widthScale : 1};
    }

    static SelectionBox boundsForCorners(int page, const std::array<Point, 4>& corners) {
        SelectionBox bounds{page, corners[0].x, corners[0].y, corners[0].x, corners[0].y};
        for (const auto& point: corners) {
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
        return bounds;
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

    SelectionBox handleBounds(SelectionBox box, const SelectionTransform& transform = {}) const {
        const auto extentX = 20 / (zoom * std::hypot(transform.xx, transform.yx));
        const auto extentY = 20 / (zoom * std::hypot(transform.xy, transform.yy));
        if (box.x1-box.x0 < extentX) {
            const auto center = (box.x0+box.x1)/2;
            box.x0 = center-extentX/2; box.x1 = center+extentX/2;
        }
        if (box.y1-box.y0 < extentY) {
            const auto center = (box.y0+box.y1)/2;
            box.y0 = center-extentY/2; box.y1 = center+extentY/2;
        }
        return box;
    }

    std::array<Point, 9> selectionHandles() const {
        const auto b = selectionLocalBounds();
        if (!b) return {};
        SelectionTransform transform;
        if (selectionGesture && selectionGesture->moving) {
            transform = composeSelectionTransforms(selectionGesture->transform,
                                                   selectionGesture->baseFrameTransform);
        } else {
            if (selectionFrameMatchesCurrentSelection()) {
                transform = selectionFrame->transform;
            }
        }
        const auto corners = transformedCorners(handleBounds(*b, transform), transform);
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
        gesture.baseFrameTransform = selectionFrame ? selectionFrame->transform : SelectionTransform{};
        gesture.multiLayer = selectionAcrossLayers;
        auto bounds = selectionLocalBounds();
        if (bounds && bounds->page == hit->page && !forceRectangle) {
            const auto handles = selectionHandles();
            for (int n = 0; n < 9; ++n) {
                if (std::hypot(handles[n].x - hit->local.x, handles[n].y - hit->local.y) <= 5 / zoom) {
                    gesture.handle = n;
                    break;
                }
            }
        }
        const auto inverseFrame = inverseSelectionTransform(gesture.baseFrameTransform);
        const auto framePoint = inverseFrame.apply(hit->local);
        const auto paddingX = 5 / (zoom * std::max(0.01, std::hypot(gesture.baseFrameTransform.xx,
                                                                    gesture.baseFrameTransform.yx)));
        const auto paddingY = 5 / (zoom * std::max(0.01, std::hypot(gesture.baseFrameTransform.xy,
                                                                    gesture.baseFrameTransform.yy)));
        gesture.moving = !forceRectangle &&
                         (gesture.handle >= 0 ||
                          (bounds && bounds->page == hit->page && framePoint.x >= bounds->x0 - paddingX &&
                           framePoint.x <= bounds->x1 + paddingX && framePoint.y >= bounds->y0 - paddingY &&
                           framePoint.y <= bounds->y1 + paddingY));
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
                gesture.baseFrameTransform = {};
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
                bounds = selectionLocalBounds();
                gesture.moving = true;
            }
        }
        if (gesture.moving) {
            gesture.indices = selected;
            gesture.textIndices = selectedTexts;
            gesture.imageIndices = selectedImages;
            gesture.bounds = gesture.handle >= 0 ? handleBounds(*bounds, gesture.baseFrameTransform) : *bounds;
            gesture.snapAnchor = closestSelectionCorner(transformedCorners(*bounds, gesture.baseFrameTransform),
                                                        gesture.start);
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
                const auto corners = transformedCorners(g.bounds, g.baseFrameTransform);
                const auto cx = (corners[0].x + corners[2].x) / 2;
                const auto cy = (corners[0].y + corners[2].y) / 2;
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
                const auto latest = inverseSelectionTransform(g.baseFrameTransform).apply(g.latest);
                const auto projection = (xs ? (latest.x-ax)*vx : 0) + (ys ? (latest.y-ay)*vy : 0);
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
                const Point candidateHandleLocal{xs ? candidateEdgeX : (g.bounds.x0 + g.bounds.x1) / 2,
                                                 ys ? candidateEdgeY : (g.bounds.y0 + g.bounds.y1) / 2};
                const auto candidateHandle = g.baseFrameTransform.apply(candidateHandleLocal);
                const Point snappedHandle{
                        snapSelectionResizeAxis(g.targetPage, candidateHandle.x, true, altHeld),
                        snapSelectionResizeAxis(g.targetPage, candidateHandle.y, false, altHeld)};
                const auto snappedLocal = inverseSelectionTransform(g.baseFrameTransform).apply(snappedHandle);
                const auto snappedX = xs ? snappedLocal.x : candidateEdgeX;
                const auto snappedY = ys ? snappedLocal.y : candidateEdgeY;
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

                g.transform = composeSelectionTransforms(
                        g.baseFrameTransform,
                        composeSelectionTransforms(g.frameTransform,
                                                   inverseSelectionTransform(g.baseFrameTransform)));
                g.transform.widthScale = g.frameTransform.widthScale;
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
                    SelectionFrameState nextFrame;
                    nextFrame.bounds = g.bounds;
                    nextFrame.bounds.page = g.targetPage;
                    nextFrame.transform = composeSelectionTransforms(g.transform, g.baseFrameTransform);
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

    PageSize pageSize(int index) const;

    void selectActivePage(int target);

    void setPages(std::vector<PageSize> nextPages);

    void setLayoutSettings(const CanvasLayoutSettings& settings);

    void rebuildPageLayout();

    double layoutWidth() const;

    double layoutHeight() const;

    double pageLeft(int index) const;

    double pageTop(int index) const;

    std::vector<int> visiblePageIndices(double viewportWidth, double viewportHeight, double marginX = 0,
                                        double marginY = 0) const;

    double activeEraserWidth() const;

    double steppedZoom(bool zoomIn, bool scroll) const;

    Point documentPoint(double x, double y, double pressure = 1) const;

    Point pageLocalPoint(int pageIndex, double x, double y, double pressure = 1) const;

    Point pageScreenPoint(int pageIndex, double localX, double localY, double pressure = 1) const;

    bool contains(const Point& p) const;

    bool containsOnPage(int pageIndex, const Point& local) const;

    std::optional<PageHit> hitPage(double x, double y, double pressure = 1) const;

    std::optional<PageHit> hitSelectionHandle(double x, double y) const;

    void fit(double width, double height);

    void fitPresentation(double width, double height);

    void scrollBy(double dx, double dy, bool discrete);

    bool hasHorizontalScrollRoom(double viewportWidth) const;

    void constrainScrollOffsets(double viewportWidth, double viewportHeight);

    void scrollToPage(int targetPage, double viewportWidth, double viewportHeight);

    // Mirrors Layout::updateVisibility in src/core/gui/Layout.cpp: select the page with the
    // largest visible viewport intersection area while keeping the current page if none intersect.
    void syncActivePage(double viewportWidth, double viewportHeight);

    void insertPageAfter(int afterIndex, PageSize size, bool shiftHistory = true);

    void recordPageInsertion(int pageIndex, int currentPageIndex = -1, std::string sourceSnapshotPath = {});

    bool deletePageWithHistory(int removeIndex, std::string pageSnapshotPath);

    bool nextUndoPageStructureChange(bool& pageExistsAfter, int& pageIndex, std::string& sourceSnapshotPath) const;

    bool nextRedoPageStructureChange(bool& pageExistsAfter, int& pageIndex, std::string& sourceSnapshotPath) const;

    std::vector<std::string> pageHistorySnapshotPaths() const;

    bool replaceNextPageWithClone(int sourcePage, PageSize replacementSize, std::string insertedPageSnapshotPath,
                                  std::string replacedPageSnapshotPath);

    bool deletePageAt(int removeIndex, bool clearHistory = true);

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
                    bool pressureSensitive);

    void appendLaser(Device source, double x, double y, double pressure);

    void finishLaser(Device source, std::optional<double> releasePressure = std::nullopt,
                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    void cancelLaser(Device source, std::chrono::steady_clock::time_point now =
                                             std::chrono::steady_clock::now());

    bool hasLaserFades() const;

    bool advanceLaserFades(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    void clearLaserPointers();

    bool begin(Device source, double x, double y, double pressure = 1, bool eraserEnd = false,
               bool shift = false, bool control = false, bool alt = false);

    void append(Device source, double x, double y, double pressure = 1, bool shift = false, bool control = false,
                bool alt = false);

    void refreshCurrentShape(bool shift, bool control, bool alt);

    bool addSplineKnot(double x, double y, double pressure = 1);

    void moveSplineTangent(double x, double y, double pressure = 1);

    void updateSplineHover(double x, double y);

    bool removeLastSplineKnot();

    bool closeSplinePath();

    bool moveLastSplineKnot(double dx, double dy);

    bool rotateLastSplineTangent(double degrees);

    bool scaleLastSplineTangent(double factor);

    void finish(Device source, std::optional<double> releasePressure = std::nullopt);

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

    bool setSelectedInkLineStyle(const LineDashPattern& lineDashes) {
        if (!validLineDashPattern(lineDashes)) {
            return false;
        }
        std::vector<std::pair<size_t, Ink>> replacements;
        for (const size_t index: selected) {
            if (index >= ink.size() || ink[index].brush.tool != Tool::Pen ||
                ink[index].brush.lineDashes == lineDashes) {
                continue;
            }
            auto replacement = ink[index];
            replacement.brush.lineDashes = lineDashes;
            replacements.emplace_back(index, std::move(replacement));
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
            if (distance < minimumDistance ||
                (best && distance == minimumDistance && order > bestOrder)) {
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

    void undo();

    void redo();

    bool canUndo() const;
    bool canRedo() const;
    size_t revision() const;

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
    void refreshSplinePreview();

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

    struct PageSnapshot {
        int pageIndex = 0;
        PageSize size;
        PageLayers layers;
        std::vector<IndexedObject<Ink>> ink;
        std::vector<IndexedObject<TextItem>> texts;
        std::vector<IndexedObject<CanvasImage>> images;
        std::string sourceSnapshotPath;
    };

    struct PageStructureChange {
        PageSnapshot page;
        bool pageExistsAfter = false;
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
        SelectionTransform transform;
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
        std::optional<PageStructureChange> pageStructureChange;
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

    static void shiftEditPageIndices(Edit& edit, int insertedIndex) {
        auto shiftStroke = [insertedIndex](Ink& stroke) {
            if (stroke.page >= insertedIndex) {
                ++stroke.page;
            }
        };
        auto shiftText = [insertedIndex](TextItem& text) {
            if (text.page >= insertedIndex) {
                ++text.page;
            }
        };
        auto shiftImage = [insertedIndex](CanvasImage& image) {
            if (image.page >= insertedIndex) {
                ++image.page;
            }
        };
        for (auto& change: edit.changes) {
            std::for_each(change.before.begin(), change.before.end(), shiftStroke);
            std::for_each(change.after.begin(), change.after.end(), shiftStroke);
        }
        for (auto& change: edit.textChanges) {
            std::for_each(change.before.begin(), change.before.end(), shiftText);
            std::for_each(change.after.begin(), change.after.end(), shiftText);
        }
        for (auto& change: edit.imageChanges) {
            std::for_each(change.before.begin(), change.before.end(), shiftImage);
            std::for_each(change.after.begin(), change.after.end(), shiftImage);
        }
        if (edit.pageSizeChange && edit.pageSizeChange->page >= insertedIndex) {
            ++edit.pageSizeChange->page;
        }
        shiftLayerMutationPage(edit.layerMutation, insertedIndex);
        if (edit.pageStructureChange) {
            auto& snapshot = edit.pageStructureChange->page;
            if (snapshot.pageIndex >= insertedIndex) {
                ++snapshot.pageIndex;
            }
            for (auto& indexed: snapshot.ink) {
                shiftStroke(indexed.value);
            }
            for (auto& indexed: snapshot.texts) {
                shiftText(indexed.value);
            }
            for (auto& indexed: snapshot.images) {
                shiftImage(indexed.value);
            }
        }
    }

    static void forEachPageObject(PageSnapshot& snapshot, const auto& callback) {
        for (auto& indexed: snapshot.ink) {
            callback(indexed.value);
        }
        for (auto& indexed: snapshot.texts) {
            callback(indexed.value);
        }
        for (auto& indexed: snapshot.images) {
            callback(indexed.value);
        }
    }

    PageSnapshot capturePage(int pageIndex) const;

    void refreshPageStructureSnapshot(PageStructureChange& change) const;

    static bool readPageStructureChange(const std::vector<Edit>& edits, bool& pageExistsAfter, int& pageIndex,
                                        std::string& sourceSnapshotPath);

    void insertPageAt(int pageIndex, PageSize size, const PageLayers& layers);

    void restorePageSnapshot(const PageSnapshot& snapshot);

    struct SelectionGesture {
        int page = 0;
        Point start{}, latest{};
        bool moving = false;
        bool lasso = false, additive = false;
        bool multiLayer = false;
        int handle = -1, targetPage = 0;
        Point snapAnchor{};
        SelectionTransform baseFrameTransform;
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

    static Point lerpPoint(const Point& a, const Point& b, double t);

    static std::array<Point, 4> textCorners(const TextItem& item);

    static std::optional<SelectionBox> inkBounds(const Ink& stroke);

    double snapVertical(int pageIndex, double y) const;

    static Point closestSelectionCorner(const std::array<Point, 4>& corners, Point point);

    Point snapSelectionMove(int pageIndex, Point point, bool altHeld) const;

    double snapSelectionResizeAxis(int pageIndex, double value, bool horizontal, bool altHeld) const;

    void collectVerticalSpaceObjects(VerticalSpacePreview& preview) const;

    static double strokeDistance(const Ink& stroke, Point point);

    static double quadrilateralDistance(const std::array<Point, 4>& corners, Point point);

    static TextItem transformText(TextItem item, const SelectionTransform& transform);

    static CanvasImage transformImage(CanvasImage image, const SelectionTransform& transform);

    static Point pointAt(const Ink& stroke, PathParam param);

    static std::optional<Interval> intersectLineWithBox(const Point& p, const Point& q, const Box& box);

    static std::vector<double> intersectSegmentWithBox(const Point& p, const Point& q, const Box& box);

    static bool allPointsCoincident(const std::vector<Point>& pts);

    static bool intersectsSquare(const Ink& stroke, double cx, double cy, double halfSize);

    // Mirrors Stroke::intersectWithPaddedBox + ErasableStroke::getStrokes in src/core/model/.
    static SplitResult eraseWithPaddedBox(const Ink& stroke, double cx, double cy, double halfSize);

    void eraseStep(int targetPage, const Point& p);

    void record(Edit edit);

    void notifyHistoryChanged() const;

    void apply(const Edit& edit, bool forward);

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
