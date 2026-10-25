#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "CanvasLayers.h"

namespace prototype {

struct TextStyleRun {
    size_t start = 0;
    size_t end = 0;
    std::string fontName;
    double fontSize = 12;
    std::optional<uint32_t> color;
    uint32_t attributes = 0;

    bool operator==(const TextStyleRun&) const = default;
};

// A GTK-independent editable text element in page-local PDF point coordinates.
// Style offsets use half-open UTF-8 byte ranges, matching the native Text model.
struct TextItem {
    int page = 0;
    size_t layerOrder = 0;
    size_t layerId = 1;
    size_t originLayer = newLayerOrigin;
    std::string text;
    std::string fontName = "Sans";
    std::string linkUrl;
    bool isLink = false;
    double fontSize = 12;
    uint32_t color = 0xff000000U;
    double wrapWidth = -1;
    int alignment = 0;
    bool justify = false;
    double xx = 1;
    double yx = 0;
    double xy = 0;
    double yy = 1;
    double x = 0;
    double y = 0;
    // Natural untransformed layout size used for rectangular selection.
    double layoutWidth = 0;
    double layoutHeight = 0;
    std::vector<TextStyleRun> styleRuns;
    std::uintptr_t pluginReference = 0;

    bool operator==(const TextItem& other) const {
        return std::tie(page, layerOrder, layerId, originLayer, text, fontName, linkUrl, isLink, fontSize, color,
                        wrapWidth, alignment, justify, xx, yx, xy, yy, x, y, layoutWidth, layoutHeight, styleRuns) ==
               std::tie(other.page, other.layerOrder, other.layerId, other.originLayer, other.text, other.fontName,
                        other.linkUrl, other.isLink, other.fontSize, other.color, other.wrapWidth, other.alignment,
                        other.justify, other.xx, other.yx, other.xy, other.yy, other.x, other.y,
                        other.layoutWidth, other.layoutHeight, other.styleRuns);
    }
};

}  // namespace prototype
