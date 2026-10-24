#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>

#include "CanvasLayers.h"
#include "CanvasDrawing.h"

typedef struct _cairo_surface cairo_surface_t;

namespace prototype {

inline constexpr size_t maxCanvasImageBytes = 64 * 1024 * 1024;
inline constexpr size_t maxCanvasImagePixels = 32 * 1024 * 1024;

struct CanvasImageTransform {
    double xx = 1;
    double yx = 0;
    double xy = 0;
    double yy = 1;
    double x = 0;
    double y = 0;
};

struct CanvasImage {
    int page = 0;
    size_t layerOrder = 0;
    size_t layerId = 1;
    size_t originLayer = newLayerOrigin;
    int pixelWidth = 0;
    int pixelHeight = 0;
    CanvasImageTransform transform;
    std::string encodedData;
    std::shared_ptr<cairo_surface_t> surface;
};

struct CanvasImageBounds {
    int page = 0;
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
};

inline bool validCanvasImage(const CanvasImage& image) {
    const auto& m = image.transform;
    const double determinant = m.xx * m.yy - m.xy * m.yx;
    return image.page >= 0 && image.layerId != 0 && image.pixelWidth > 0 && image.pixelHeight > 0 &&
           static_cast<size_t>(image.pixelWidth) <= maxCanvasImagePixels / static_cast<size_t>(image.pixelHeight) &&
           !image.encodedData.empty() && image.encodedData.size() <= maxCanvasImageBytes &&
           std::isfinite(determinant) && std::abs(determinant) > 1e-10 &&
           std::isfinite(m.xx) && std::isfinite(m.yx) && std::isfinite(m.xy) && std::isfinite(m.yy) &&
           std::isfinite(m.x) && std::isfinite(m.y);
}

inline Point canvasImagePoint(const CanvasImage& image, double x, double y) {
    const auto& m = image.transform;
    return {m.xx * x + m.xy * y + m.x, m.yx * x + m.yy * y + m.y};
}

inline std::array<Point, 4> canvasImageCorners(const CanvasImage& image) {
    const auto width = static_cast<double>(image.pixelWidth);
    const auto height = static_cast<double>(image.pixelHeight);
    return {canvasImagePoint(image, 0, 0), canvasImagePoint(image, width, 0),
            canvasImagePoint(image, width, height), canvasImagePoint(image, 0, height)};
}

inline CanvasImageBounds canvasImageBounds(const CanvasImage& image) {
    const auto corners = canvasImageCorners(image);
    CanvasImageBounds bounds{image.page, corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const auto& point: corners) {
        bounds.x0 = std::min(bounds.x0, point.x);
        bounds.y0 = std::min(bounds.y0, point.y);
        bounds.x1 = std::max(bounds.x1, point.x);
        bounds.y1 = std::max(bounds.y1, point.y);
    }
    return bounds;
}

}  // namespace prototype
