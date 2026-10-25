#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#include "CanvasImage.h"

namespace prototype {

inline std::shared_ptr<cairo_surface_t> decodeCanvasImage(std::string_view bytes, int& width, int& height) {
    if (bytes.empty() || bytes.size() > maxCanvasImageBytes) {
        throw std::runtime_error("Image data is empty or exceeds the 64 MiB limit");
    }
    struct DecodeBounds {
        bool invalidDimensions = false;
    } bounds;
    auto* loader = gdk_pixbuf_loader_new();
    g_signal_connect(loader, "size-prepared",
                     G_CALLBACK(+[](GdkPixbufLoader* loader, int width, int height, gpointer data) {
                         auto* bounds = static_cast<DecodeBounds*>(data);
                         if (width <= 0 || height <= 0) {
                             bounds->invalidDimensions = true;
                             gdk_pixbuf_loader_set_size(loader, 1, 1);
                             return;
                         }
                         if (static_cast<size_t>(width) > maxCanvasImagePixels / static_cast<size_t>(height)) {
                             const double scale = std::sqrt(static_cast<double>(maxCanvasImagePixels) /
                                                            (static_cast<double>(width) * height));
                             int targetWidth = std::max(1, static_cast<int>(std::floor(width * scale)));
                             int targetHeight = std::max(1, static_cast<int>(std::floor(height * scale)));
                             while (static_cast<size_t>(targetWidth) >
                                    maxCanvasImagePixels / static_cast<size_t>(targetHeight)) {
                                 if (targetWidth >= targetHeight) {
                                     --targetWidth;
                                 } else {
                                     --targetHeight;
                                 }
                             }
                             gdk_pixbuf_loader_set_size(loader, targetWidth, targetHeight);
                         }
                     }),
                     &bounds);
    GError* error = nullptr;
    if (!gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(bytes.data()), bytes.size(), &error)) {
        const std::string message = error ? error->message : "Unsupported or invalid image data";
        g_clear_error(&error);
        g_object_unref(loader);
        throw std::runtime_error(message);
    }
    if (!gdk_pixbuf_loader_close(loader, &error)) {
        const std::string message = error ? error->message : "Incomplete or invalid image data";
        g_clear_error(&error);
        g_object_unref(loader);
        throw std::runtime_error(message);
    }
    if (bounds.invalidDimensions) {
        g_object_unref(loader);
        throw std::runtime_error("Image has invalid dimensions");
    }
    auto* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf) {
        g_object_ref(pixbuf);
    }
    g_object_unref(loader);
    if (!pixbuf) {
        const std::string message = error ? error->message : "Unsupported or invalid image data";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }

    auto* orientedPixbuf = gdk_pixbuf_apply_embedded_orientation(pixbuf);
    g_object_unref(pixbuf);
    if (!orientedPixbuf) {
        throw std::runtime_error("Cannot apply the embedded image orientation");
    }
    pixbuf = orientedPixbuf;

    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);
    if (width <= 0 || height <= 0 || static_cast<size_t>(width) > maxCanvasImagePixels / static_cast<size_t>(height)) {
        g_object_unref(pixbuf);
        throw std::runtime_error("Image dimensions exceed the 32 megapixel limit");
    }

    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        const auto message = std::string(cairo_status_to_string(cairo_surface_status(surface)));
        cairo_surface_destroy(surface);
        g_object_unref(pixbuf);
        throw std::runtime_error("Cannot create image surface: " + message);
    }
    auto* destination = cairo_image_surface_get_data(surface);
    const auto stride = cairo_image_surface_get_stride(surface);
    const auto* source = gdk_pixbuf_get_pixels(pixbuf);
    const int sourceStride = gdk_pixbuf_get_rowstride(pixbuf);
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const bool hasAlpha = gdk_pixbuf_get_has_alpha(pixbuf);
    for (int row = 0; row < height; ++row) {
        const auto* src = source + row * sourceStride;
        auto* dst = reinterpret_cast<uint32_t*>(destination + row * stride);
        for (int col = 0; col < width; ++col) {
            const auto* pixel = src + col * channels;
            const uint32_t alpha = hasAlpha ? pixel[3] : 255U;
            const uint32_t red = (pixel[0] * alpha + 127U) / 255U;
            const uint32_t green = (pixel[1] * alpha + 127U) / 255U;
            const uint32_t blue = (pixel[2] * alpha + 127U) / 255U;
            dst[col] = (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
        }
    }
    cairo_surface_mark_dirty(surface);
    g_object_unref(pixbuf);
    return std::shared_ptr<cairo_surface_t>(surface, cairo_surface_destroy);
}

inline CanvasImage readCanvasImageFile(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("Image is not a regular file");
    }
    const auto length = std::filesystem::file_size(path, ec);
    if (ec || length == 0 || length > maxCanvasImageBytes) {
        throw std::runtime_error("Image is empty or exceeds the 64 MiB limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open image file");
    }
    CanvasImage image;
    image.encodedData.resize(static_cast<size_t>(length));
    if (!stream.read(image.encodedData.data(), static_cast<std::streamsize>(image.encodedData.size()))) {
        throw std::runtime_error("Cannot read image file");
    }
    image.surface = decodeCanvasImage(image.encodedData, image.pixelWidth, image.pixelHeight);
    return image;
}

inline void drawCanvasImage(cairo_t* cr, const CanvasImage& image) {
    if (!image.surface) {
        return;
    }
    const auto& m = image.transform;
    const cairo_matrix_t matrix{m.xx, m.yx, m.xy, m.yy, m.x, m.y};
    cairo_save(cr);
    cairo_transform(cr, &matrix);
    cairo_set_source_surface(cr, image.surface.get(), 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_rectangle(cr, 0, 0, image.pixelWidth, image.pixelHeight);
    cairo_fill(cr);
    cairo_restore(cr);
}

inline void drawCanvasImageHandles(cairo_t* cr, const CanvasImage& image, double zoom) {
    const auto corners = canvasImageCorners(image);
    const double handle = 6.0 / zoom;
    const auto rotationHandle = canvasImageRotationHandle(image, zoom);
    const auto rotationStem = Point{(corners[0].x + corners[1].x) / 2,
                                    (corners[0].y + corners[1].y) / 2};
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.12, 0.42, 0.86, 0.95);
    cairo_set_line_width(cr, 1.5 / zoom);
    cairo_move_to(cr, corners[0].x, corners[0].y);
    for (size_t i = 1; i < corners.size(); ++i) {
        cairo_line_to(cr, corners[i].x, corners[i].y);
    }
    cairo_close_path(cr);
    cairo_stroke(cr);
    for (const auto& corner: corners) {
        cairo_rectangle(cr, corner.x - handle / 2, corner.y - handle / 2, handle, handle);
    }
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.12, 0.42, 0.86, 0.95);
    cairo_stroke(cr);
    cairo_move_to(cr, rotationStem.x, rotationStem.y);
    cairo_line_to(cr, rotationHandle.x, rotationHandle.y);
    cairo_stroke(cr);
    cairo_arc(cr, rotationHandle.x, rotationHandle.y, handle * 0.65, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.12, 0.42, 0.86, 0.95);
    cairo_stroke(cr);
    cairo_restore(cr);
}

}  // namespace prototype
