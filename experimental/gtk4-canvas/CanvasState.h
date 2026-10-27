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
    void ensurePluginReferences();

    void addPluginReferencesToSelection(const std::vector<std::uintptr_t>& requestedReferences);

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

    bool selectLayer(int pageIndex, size_t layerId);

    bool setLayerVisible(int pageIndex, size_t layerId, bool visible);

    bool renameLayer(int pageIndex, size_t layerId, const std::string& name);

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

    bool addPluginStrokes(std::vector<Ink> additions, int undoPolicy);

    bool addPluginTexts(std::vector<TextItem> additions, int undoPolicy);

    bool addPluginImages(std::vector<CanvasImage> additions, int undoPolicy);

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

    bool setAllLayersVisible(int pageIndex, bool visible);

    bool addLayerAboveCurrent();

    bool addLayerBelowCurrent();

    bool duplicateCurrentLayer();

    bool moveCurrentLayer(bool up);

    bool deleteCurrentLayer();

    bool mergeCurrentLayerDown();

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

    bool beginVerticalSpace(double x, double y, bool moveAbove = false);

    void moveVerticalSpace(double x, double y, bool moveAbove);

    void setVerticalSpaceDirection(bool moveAbove);

    bool finishVerticalSpace();

    void cancelVerticalSpace();

    void cancelSelection();

    void clearSelection();

    bool hasSelection() const { return !selected.empty() || !selectedTexts.empty() || !selectedImages.empty(); }

    size_t selectionCount() const { return selected.size() + selectedTexts.size() + selectedImages.size(); }

    std::optional<SelectableObject> pickSmartSelectObject(int pageIndex, Point point) const;

    std::optional<size_t> pickStroke(int pageIndex, Point point) const;

    std::optional<size_t> pickText(int pageIndex, Point point) const;

    std::optional<size_t> pickLink(int pageIndex, Point point) const;

    static void extendLassoAtPageEdges(std::vector<Point>& boundary, const PageSize& size);

    bool beginSelection(double x, double y, bool forceRectangle = false);

    void moveSelectionPointer(double x, double y, bool altHeld = false);

    bool edgePanSelectionAtPointer(double x, double y, double viewportWidth, double viewportHeight,
                                   bool altHeld = false);

    void finishSelection();

    void selectAllOnPage();

    void deleteSelection();

    static bool polygonContains(const std::vector<Point>& polygon, Point p);

    std::vector<Ink> copySelection() const;

    std::vector<TextItem> copyTextSelection() const;

    std::vector<EditableObject> copySelectedObjects() const;

    bool pasteStrokes(std::vector<Ink> pasted, int targetPage, Point center);

    bool pasteTexts(std::vector<TextItem> pasted, int targetPage, Point center);

    bool pasteObjects(std::vector<EditableObject> pasted, int targetPage, Point center);

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

    bool setSelectedInkFill(int penOpacity, int highlighterOpacity);

    bool setSelectedInkLineStyle(const LineDashPattern& lineDashes);

    bool applyInkEdits(std::vector<std::pair<size_t, Ink>> replacements, std::vector<Ink> additions);

    bool insertText(TextItem item);

    bool replaceText(size_t index, TextItem item);

    bool replaceTextWithImage(size_t index, CanvasImage image);

    bool recolorSelectedTexts(uint32_t color);

    bool recolorSelection(const std::array<double, 3>& color, uint32_t textColor);

    bool deleteText(size_t index);

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

    bool insertImage(CanvasImage image);

    bool pasteImages(std::vector<CanvasImage> pasted, int targetPage, Point center);

    bool replaceImage(size_t index, CanvasImage image);

    bool replaceImageGroup(std::vector<std::pair<size_t, std::vector<CanvasImage>>> replacements);

    bool deleteImage(size_t index);

    std::optional<size_t> pickImage(int pageIndex, Point point) const;

    bool selectObjectAt(int pageIndex, Point point, bool additive = false);

    std::optional<SelectableObject> pickSelectableObject(int pageIndex, Point point) const;

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

    void recordLayerMutation(LayerMutation mutation);

    template <typename T>
    static void restoreIndexedObjects(std::vector<T>& objects, int pageIndex,
                                      const std::vector<IndexedObject<T>>& snapshot) {
        std::erase_if(objects, [pageIndex](const T& item) { return item.page == pageIndex; });
        for (const auto& indexed: snapshot) {
            const auto at = std::min(indexed.index, objects.size());
            objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(at), indexed.value);
        }
    }

    void remapObjectsAfterLayerRemoval(int pageIndex, size_t deletedId);

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
