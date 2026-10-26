#include "ClipboardRenderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <cairo-svg.h>

#include "ImageRenderer.h"
#include "InkRenderer.h"
#include "StrokeClipboard.h"
#include "TextRenderer.h"

namespace prototype::clipboard {

namespace {

std::string renderImagesPng(const std::vector<prototype::CanvasImage>& images) {
    if (images.empty()) {
        throw std::runtime_error("No images are selected");
    }
    for (const auto& image: images) {
        if (!prototype::validCanvasImage(image) ||
            (image.kind == prototype::CanvasImageKind::Latex && !image.pdfDocument && !image.surface) ||
            (image.kind == prototype::CanvasImageKind::Raster && !image.surface)) {
            throw std::runtime_error("The selected image cannot be copied");
        }
    }

    auto bounds = prototype::canvasImageBounds(images.front());
    double pixelsPerPoint = 1.0;
    for (const auto& image: images) {
        for (const auto& point: prototype::canvasImageCorners(image)) {
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
        const auto& transform = image.transform;
        const double scaleX = std::hypot(transform.xx, transform.yx);
        const double scaleY = std::hypot(transform.xy, transform.yy);
        const double smallestScale = std::min(scaleX, scaleY);
        if (smallestScale > 1e-10) {
            pixelsPerPoint = std::max(pixelsPerPoint, 1.0 / smallestScale);
        }
    }

    const double boundsWidth = bounds.x1 - bounds.x0;
    const double boundsHeight = bounds.y1 - bounds.y0;
    if (!std::isfinite(boundsWidth) || !std::isfinite(boundsHeight)) {
        throw std::runtime_error("The selected images have invalid clipboard dimensions");
    }
    double width = std::ceil(boundsWidth * pixelsPerPoint);
    double height = std::ceil(boundsHeight * pixelsPerPoint);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1) {
        throw std::runtime_error("The selected images have invalid clipboard dimensions");
    }
    if (!std::isfinite(width * height) || width * height > static_cast<double>(prototype::maxCanvasImagePixels)) {
        const double scaleFactor =
                std::sqrt(static_cast<double>(prototype::maxCanvasImagePixels)) / std::sqrt(width) / std::sqrt(height);
        if (!std::isfinite(scaleFactor) || scaleFactor <= 0) {
            throw std::runtime_error("The selected images have invalid clipboard dimensions");
        }
        pixelsPerPoint *= scaleFactor;
        width = std::max(1.0, std::floor(boundsWidth * pixelsPerPoint));
        height = std::max(1.0, std::floor(boundsHeight * pixelsPerPoint));
        while (width * height > static_cast<double>(prototype::maxCanvasImagePixels)) {
            if (width >= height) {
                --width;
            } else {
                --height;
            }
        }
    }
    if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
        throw std::runtime_error("The selected images have invalid clipboard dimensions");
    }
    const auto pixelWidth = static_cast<int>(width);
    const auto pixelHeight = static_cast<int>(height);
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth, pixelHeight);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(cairo_surface_status(surface));
        cairo_surface_destroy(surface);
        throw std::runtime_error("Cannot render the images for the clipboard: " + message);
    }

    auto* context = cairo_create(surface);
    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(context, 0, 0, 0, 0);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    cairo_translate(context, -bounds.x0, -bounds.y0);
    cairo_scale(context, pixelsPerPoint, pixelsPerPoint);
    for (const auto& image: images) {
        prototype::drawCanvasImage(context, image);
    }
    cairo_destroy(context);

    std::string png;
    const auto write = [](void* data, const unsigned char* bytes, unsigned int length) {
        auto& output = *static_cast<std::string*>(data);
        if (length > prototype::maxCanvasImageBytes - output.size()) {
            return CAIRO_STATUS_WRITE_ERROR;
        }
        output.append(reinterpret_cast<const char*>(bytes), length);
        return CAIRO_STATUS_SUCCESS;
    };
    const auto status = cairo_surface_write_to_png_stream(surface, write, &png);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS || png.empty()) {
        throw std::runtime_error("Cannot encode the selected images as PNG");
    }
    return png;
}

}  // namespace

std::string renderImagePng(const prototype::CanvasImage& image) { return renderImagesPng({image}); }

RenderedSelection renderSelection(const std::vector<prototype::EditableObject>& objects,
                                  const prototype::SelectionBox& selectionBounds) {
    if (objects.empty()) {
        throw std::runtime_error("No objects are selected");
    }

    auto bounds = selectionBounds;
    double padding = 2;
    double pixelsPerPoint = 300.0 / 72.0;
    std::vector<prototype::TextItem> textItems;
    for (const auto& object: objects) {
        std::visit(
                [&](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, Ink>) {
                        double pressure = 1;
                        if (item.pressureSensitive) {
                            for (const auto& point: item.points) {
                                pressure = std::max(pressure, point.pressure);
                            }
                        }
                        padding = std::max(padding, item.brush.width * pressure / 2.0 + 1);
                    } else if constexpr (std::is_same_v<T, prototype::TextItem>) {
                        textItems.push_back(item);
                    } else {
                        const auto& transform = item.transform;
                        const double scaleX = std::hypot(transform.xx, transform.yx);
                        const double scaleY = std::hypot(transform.xy, transform.yy);
                        const double smallestScale = std::min(scaleX, scaleY);
                        if (smallestScale > 1e-10) {
                            pixelsPerPoint = std::max(pixelsPerPoint, 1.0 / smallestScale);
                        }
                    }
                },
                object);
    }
    bounds.x0 -= padding;
    bounds.y0 -= padding;
    bounds.x1 += padding;
    bounds.y1 += padding;
    const double widthPoints = bounds.x1 - bounds.x0;
    const double heightPoints = bounds.y1 - bounds.y0;
    if (!std::isfinite(widthPoints) || !std::isfinite(heightPoints) || widthPoints <= 0 || heightPoints <= 0 ||
        widthPoints > 100000 || heightPoints > 100000) {
        throw std::runtime_error("The selected objects have invalid clipboard dimensions");
    }

    double pixelWidthValue = std::ceil(widthPoints * pixelsPerPoint);
    double pixelHeightValue = std::ceil(heightPoints * pixelsPerPoint);
    if (!std::isfinite(pixelWidthValue) || !std::isfinite(pixelHeightValue) || pixelWidthValue < 1 ||
        pixelHeightValue < 1) {
        throw std::runtime_error("The selected objects have invalid clipboard dimensions");
    }
    if (!std::isfinite(pixelWidthValue * pixelHeightValue) ||
        pixelWidthValue * pixelHeightValue > static_cast<double>(prototype::maxCanvasImagePixels)) {
        const double scaleFactor = std::sqrt(static_cast<double>(prototype::maxCanvasImagePixels)) /
                                   std::sqrt(pixelWidthValue) / std::sqrt(pixelHeightValue);
        if (!std::isfinite(scaleFactor) || scaleFactor <= 0) {
            throw std::runtime_error("The selected objects have invalid clipboard dimensions");
        }
        pixelsPerPoint *= scaleFactor;
        pixelWidthValue = std::max(1.0, std::floor(widthPoints * pixelsPerPoint));
        pixelHeightValue = std::max(1.0, std::floor(heightPoints * pixelsPerPoint));
        while (pixelWidthValue * pixelHeightValue > static_cast<double>(prototype::maxCanvasImagePixels)) {
            if (pixelWidthValue >= pixelHeightValue) {
                --pixelWidthValue;
            } else {
                --pixelHeightValue;
            }
        }
    }
    if (pixelWidthValue > std::numeric_limits<int>::max() || pixelHeightValue > std::numeric_limits<int>::max()) {
        throw std::runtime_error("The selected objects have invalid clipboard dimensions");
    }

    const auto drawObjects = [&](cairo_t* context) {
        for (const auto& object: objects) {
            std::visit(
                    [&](const auto& item) {
                        using T = std::decay_t<decltype(item)>;
                        if constexpr (std::is_same_v<T, Ink>) {
                            prototype::drawInk(context, item);
                        } else if constexpr (std::is_same_v<T, prototype::TextItem>) {
                            prototype::drawText(context, item);
                        } else {
                            prototype::drawCanvasImage(context, item);
                        }
                    },
                    object);
        }
    };
    RenderedSelection rendered;

    auto* pngSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(pixelWidthValue),
                                                  static_cast<int>(pixelHeightValue));
    if (cairo_surface_status(pngSurface) != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(cairo_surface_status(pngSurface));
        cairo_surface_destroy(pngSurface);
        throw std::runtime_error("Cannot render selected objects as PNG: " + message);
    }
    auto* pngContext = cairo_create(pngSurface);
    cairo_set_operator(pngContext, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(pngContext, 0, 0, 0, 0);
    cairo_paint(pngContext);
    cairo_set_operator(pngContext, CAIRO_OPERATOR_OVER);
    cairo_scale(pngContext, pixelsPerPoint, pixelsPerPoint);
    cairo_translate(pngContext, -bounds.x0, -bounds.y0);
    drawObjects(pngContext);
    const auto pngContextStatus = cairo_status(pngContext);
    cairo_destroy(pngContext);
    if (pngContextStatus != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(pngSurface);
        throw std::runtime_error("Cannot render selected objects as PNG");
    }
    const auto writePng = [](void* data, const unsigned char* bytes, unsigned int length) {
        auto& output = *static_cast<std::string*>(data);
        if (length > prototype::clipboard::maxBytes - output.size()) {
            return CAIRO_STATUS_WRITE_ERROR;
        }
        output.append(reinterpret_cast<const char*>(bytes), length);
        return CAIRO_STATUS_SUCCESS;
    };
    const auto pngStatus = cairo_surface_write_to_png_stream(pngSurface, writePng, &rendered.png);
    cairo_surface_destroy(pngSurface);
    if (pngStatus != CAIRO_STATUS_SUCCESS || rendered.png.empty()) {
        throw std::runtime_error("Cannot encode selected objects as PNG");
    }

    const auto writeSvg = [](void* data, const unsigned char* bytes, unsigned int length) {
        auto& output = *static_cast<std::string*>(data);
        if (length > prototype::clipboard::maxBytes - output.size()) {
            return CAIRO_STATUS_WRITE_ERROR;
        }
        output.append(reinterpret_cast<const char*>(bytes), length);
        return CAIRO_STATUS_SUCCESS;
    };
    auto* svgSurface = cairo_svg_surface_create_for_stream(writeSvg, &rendered.svg, widthPoints, heightPoints);
    if (cairo_surface_status(svgSurface) != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(cairo_surface_status(svgSurface));
        cairo_surface_destroy(svgSurface);
        throw std::runtime_error("Cannot create selected objects SVG: " + message);
    }
    auto* svgContext = cairo_create(svgSurface);
    cairo_translate(svgContext, -bounds.x0, -bounds.y0);
    drawObjects(svgContext);
    const auto svgContextStatus = cairo_status(svgContext);
    cairo_destroy(svgContext);
    cairo_surface_finish(svgSurface);
    const auto svgSurfaceStatus = cairo_surface_status(svgSurface);
    cairo_surface_destroy(svgSurface);
    if (svgContextStatus != CAIRO_STATUS_SUCCESS || svgSurfaceStatus != CAIRO_STATUS_SUCCESS || rendered.svg.empty()) {
        throw std::runtime_error("Cannot encode selected objects as SVG");
    }

    const auto topLeft = [](const prototype::TextItem& item) {
        double x0 = std::numeric_limits<double>::infinity();
        double y0 = std::numeric_limits<double>::infinity();
        for (const prototype::Point point: {
                     prototype::Point{0, 0},
                     prototype::Point{item.layoutWidth, 0},
                     prototype::Point{item.layoutWidth, item.layoutHeight},
                     prototype::Point{0, item.layoutHeight},
             }) {
            x0 = std::min(x0, item.xx * point.x + item.xy * point.y + item.x);
            y0 = std::min(y0, item.yx * point.x + item.yy * point.y + item.y);
        }
        return std::pair{y0, x0};
    };
    std::stable_sort(textItems.begin(), textItems.end(),
                     [&](const auto& lhs, const auto& rhs) { return topLeft(lhs) < topLeft(rhs); });
    for (const auto& text: textItems) {
        if (!rendered.plainText.empty()) {
            rendered.plainText.push_back('\n');
        }
        rendered.plainText += text.text;
    }
    return rendered;
}


}  // namespace prototype::clipboard
