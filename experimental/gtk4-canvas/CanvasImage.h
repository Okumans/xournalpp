#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "CanvasLayers.h"
#include "CanvasDrawing.h"

typedef struct _cairo_surface cairo_surface_t;
typedef struct _PopplerDocument PopplerDocument;

namespace prototype {

inline constexpr size_t maxCanvasImageBytes = 64 * 1024 * 1024;
inline constexpr size_t maxCanvasImagePixels = 32 * 1024 * 1024;
inline constexpr size_t maxCanvasImageObjects = 4096;
inline constexpr size_t maxCanvasLatexSourceBytes = 1024 * 1024;

enum class CanvasImageKind { Raster, Latex };

inline bool isCanvasTexPdf(std::string_view bytes) { return bytes.starts_with("%PDF-"); }

inline bool isCanvasTexPng(std::string_view bytes) {
    static constexpr std::string_view signature{"\x89PNG\r\n\x1a\n", 8};
    return bytes.starts_with(signature);
}

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
    CanvasImageKind kind = CanvasImageKind::Raster;
    double nativeWidth = 0;
    double nativeHeight = 0;
    CanvasImageTransform transform;
    std::string encodedData;
    std::string latexSource;
    std::shared_ptr<cairo_surface_t> surface;
    std::shared_ptr<PopplerDocument> pdfDocument;
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
    const bool validContent = image.kind == CanvasImageKind::Raster ?
                                      image.pixelWidth > 0 && image.pixelHeight > 0 &&
                                              static_cast<size_t>(image.pixelWidth) <=
                                                      maxCanvasImagePixels / static_cast<size_t>(image.pixelHeight) :
                              image.kind == CanvasImageKind::Latex ?
                                      (isCanvasTexPdf(image.encodedData) || isCanvasTexPng(image.encodedData)) &&
                                              image.latexSource.size() <= maxCanvasLatexSourceBytes &&
                                              std::isfinite(image.nativeWidth) && image.nativeWidth > 0 &&
                                              image.nativeWidth <= 100000 && std::isfinite(image.nativeHeight) &&
                                              image.nativeHeight > 0 && image.nativeHeight <= 100000 :
                                      false;
    return image.page >= 0 && image.layerId != 0 && validContent && !image.encodedData.empty() &&
           image.encodedData.size() <= maxCanvasImageBytes && std::isfinite(determinant) &&
           std::abs(determinant) > 1e-10 && std::isfinite(m.xx) && std::isfinite(m.yx) &&
           std::isfinite(m.xy) && std::isfinite(m.yy) && std::isfinite(m.x) && std::isfinite(m.y);
}

inline double canvasImageWidth(const CanvasImage& image) {
    return image.kind == CanvasImageKind::Latex ? image.nativeWidth : static_cast<double>(image.pixelWidth);
}

inline double canvasImageHeight(const CanvasImage& image) {
    return image.kind == CanvasImageKind::Latex ? image.nativeHeight : static_cast<double>(image.pixelHeight);
}

inline Point canvasImagePoint(const CanvasImage& image, double x, double y) {
    const auto& m = image.transform;
    return {m.xx * x + m.xy * y + m.x, m.yx * x + m.yy * y + m.y};
}

inline std::array<Point, 4> canvasImageCorners(const CanvasImage& image) {
    const auto width = canvasImageWidth(image);
    const auto height = canvasImageHeight(image);
    return {canvasImagePoint(image, 0, 0), canvasImagePoint(image, width, 0),
            canvasImagePoint(image, width, height), canvasImagePoint(image, 0, height)};
}

inline Point canvasImageRotationHandle(const CanvasImage& image, double zoom, double distancePixels = 18) {
    const auto corners = canvasImageCorners(image);
    const double edgeX = corners[1].x - corners[0].x;
    const double edgeY = corners[1].y - corners[0].y;
    const double edgeLength = std::hypot(edgeX, edgeY);
    const Point topCenter{(corners[0].x + corners[1].x) / 2, (corners[0].y + corners[1].y) / 2};
    if (!std::isfinite(edgeLength) || edgeLength <= 1e-10 || !std::isfinite(zoom) || zoom <= 0) {
        return topCenter;
    }
    const double distance = distancePixels / zoom;
    return {topCenter.x + edgeY * distance / edgeLength,
            topCenter.y - edgeX * distance / edgeLength};
}

inline Point canvasImageCenter(const CanvasImage& image) {
    return canvasImagePoint(image, canvasImageWidth(image) / 2.0, canvasImageHeight(image) / 2.0);
}

inline double snapCanvasImageRotation(double radians, double tolerance) {
    if (!std::isfinite(radians) || !std::isfinite(tolerance)) {
        return radians;
    }
    const double pi = std::acos(-1.0);
    const double step = pi / 12.0;
    const double snapped = radians - std::remainder(radians, step);
    const double threshold = (step / 2.0) * std::clamp(tolerance, 0.0, 1.0);
    return std::abs(snapped - radians) < threshold ? snapped : radians;
}

inline double canvasImageRotationDelta(double startAngle, double currentAngle, bool snapRotation,
                                       double tolerance) {
    if (!std::isfinite(startAngle) || !std::isfinite(currentAngle)) {
        return 0;
    }
    const double targetAngle = snapRotation ? snapCanvasImageRotation(currentAngle, tolerance) : currentAngle;
    return std::remainder(targetAngle - startAngle, 2 * std::acos(-1.0));
}

inline bool rotateCanvasImage(CanvasImage& image, double radians) {
    if (!validCanvasImage(image) || !std::isfinite(radians)) {
        return false;
    }

    const Point center = canvasImageCenter(image);
    const auto source = image.transform;
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    CanvasImageTransform rotated;
    rotated.xx = cosine * source.xx - sine * source.yx;
    rotated.xy = cosine * source.xy - sine * source.yy;
    rotated.yx = sine * source.xx + cosine * source.yx;
    rotated.yy = sine * source.xy + cosine * source.yy;
    rotated.x = center.x - rotated.xx * canvasImageWidth(image) / 2.0 -
                rotated.xy * canvasImageHeight(image) / 2.0;
    rotated.y = center.y - rotated.yx * canvasImageWidth(image) / 2.0 -
                rotated.yy * canvasImageHeight(image) / 2.0;

    const double determinant = rotated.xx * rotated.yy - rotated.xy * rotated.yx;
    if (!std::isfinite(rotated.xx) || !std::isfinite(rotated.xy) || !std::isfinite(rotated.yx) ||
        !std::isfinite(rotated.yy) || !std::isfinite(rotated.x) || !std::isfinite(rotated.y) ||
        !std::isfinite(determinant) || std::abs(determinant) <= 1e-10) {
        return false;
    }
    image.transform = rotated;
    return true;
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
