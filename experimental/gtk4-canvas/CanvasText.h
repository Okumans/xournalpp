#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
    std::vector<TextStyleRun> styleRuns;

    bool operator==(const TextItem&) const = default;
};

}  // namespace prototype
