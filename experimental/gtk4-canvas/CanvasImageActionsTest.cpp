#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "CanvasImageActions.h"
#include "ImageRenderer.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string makePng(int width, int height, const std::function<void(int, int, guchar*)>& paint) {
    auto* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!pixbuf) {
        throw std::runtime_error("Could not allocate test pixbuf");
    }
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    auto* pixels = gdk_pixbuf_get_pixels(pixbuf);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* pixel = pixels + y * stride + x * channels;
            paint(x, y, pixel);
        }
    }
    gchar* buffer = nullptr;
    gsize length = 0;
    GError* error = nullptr;
    if (!gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &length, "png", &error, nullptr)) {
        const std::string message = error ? error->message : "Could not encode test pixbuf";
        g_clear_error(&error);
        g_object_unref(pixbuf);
        throw std::runtime_error(message);
    }
    std::string result(buffer, length);
    g_free(buffer);
    g_object_unref(pixbuf);
    return result;
}

prototype::CanvasImage makeImage(std::string data, int width, int height) {
    prototype::CanvasImage image;
    image.pixelWidth = width;
    image.pixelHeight = height;
    image.encodedData = std::move(data);
    image.transform = {10.0 / width, 0, 0, 10.0 / height, 10, 10};
    return image;
}

GdkPixbuf* decode(const prototype::CanvasImage& image) {
    auto* loader = gdk_pixbuf_loader_new();
    GError* error = nullptr;
    if (!gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(image.encodedData.data()),
                                 image.encodedData.size(), &error) ||
        !gdk_pixbuf_loader_close(loader, &error)) {
        const std::string message = error ? error->message : "Could not decode transformed image";
        g_clear_error(&error);
        g_object_unref(loader);
        throw std::runtime_error(message);
    }
    auto* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pixbuf) {
        g_object_ref(pixbuf);
    }
    g_object_unref(loader);
    if (!pixbuf) {
        throw std::runtime_error("Transformed image has no pixels");
    }
    return pixbuf;
}

std::array<guchar, 4> pixel(GdkPixbuf* pixbuf, int x, int y) {
    const int channels = gdk_pixbuf_get_n_channels(pixbuf);
    const int stride = gdk_pixbuf_get_rowstride(pixbuf);
    const auto* data = gdk_pixbuf_get_pixels(pixbuf) + y * stride + x * channels;
    return {data[0], data[1], data[2], channels == 4 ? data[3] : static_cast<guchar>(255)};
}

void checkOperation(const prototype::CanvasImage& source, prototype::CanvasImageAction action,
                    std::vector<prototype::CanvasImage>& output, std::string& error) {
    require(prototype::transformCanvasImage(source, action, 595, 842, output, error), error.c_str());
    require(!output.empty(), "Image action returned no images");
    for (const auto& image: output) {
        require(prototype::validCanvasImage(image), "Image action returned invalid canvas image data");
    }
}

void testImageActions() {
    if (!prototype::canvasImageActionsAvailable()) {
        std::vector<prototype::CanvasImage> output;
        std::string error;
        const auto source = makeImage(makePng(2, 2,
                                              [](int, int, guchar* pixel) {
                                                  pixel[0] = 20;
                                                  pixel[1] = 30;
                                                  pixel[2] = 40;
                                                  pixel[3] = 255;
                                              }),
                                      2, 2);
        require(!prototype::transformCanvasImage(source, prototype::CanvasImageAction::Invert, 595, 842, output, error),
                "Image action unexpectedly ran without libvips");
        require(error.find("libvips") != std::string::npos, "Missing libvips support did not explain the limitation");
        return;
    }

    std::string error;
    std::vector<prototype::CanvasImage> output;
    const auto patterned = makeImage(makePng(8, 6,
                                             [](int x, int y, guchar* pixel) {
                                                 pixel[0] = static_cast<guchar>(20 + x * 10);
                                                 pixel[1] = static_cast<guchar>(30 + y * 10);
                                                 pixel[2] = 40;
                                                 pixel[3] = 255;
                                             }),
                                     8, 6);

    checkOperation(patterned, prototype::CanvasImageAction::Invert, output, error);
    require(output.size() == 1 && output[0].pixelWidth == 8 && output[0].pixelHeight == 6,
            "Invert changed the image dimensions");
    auto* inverted = decode(output[0]);
    const auto invertedPixel = pixel(inverted, 0, 0);
    require(invertedPixel[0] == 235 && invertedPixel[1] == 225 && invertedPixel[2] == 215,
            "Invert did not invert the source color channels");
    g_object_unref(inverted);

    output.clear();
    checkOperation(patterned, prototype::CanvasImageAction::Downscale, output, error);
    require(output.size() == 1 && output[0].pixelWidth == 4 && output[0].pixelHeight == 3,
            "Downscale did not halve the encoded image dimensions");

    output.clear();
    checkOperation(patterned, prototype::CanvasImageAction::FlipHorizontal, output, error);
    auto* flipped = decode(output[0]);
    auto* original = decode(patterned);
    require(pixel(flipped, 0, 0) == pixel(original, 7, 0), "Horizontal flip did not reverse the pixel columns");
    g_object_unref(flipped);
    g_object_unref(original);

    output.clear();
    checkOperation(patterned, prototype::CanvasImageAction::FlipVertical, output, error);
    auto* verticallyFlipped = decode(output[0]);
    original = decode(patterned);
    require(pixel(verticallyFlipped, 0, 0) == pixel(original, 0, 5), "Vertical flip did not reverse the pixel rows");
    g_object_unref(verticallyFlipped);
    g_object_unref(original);

    output.clear();
    checkOperation(patterned, prototype::CanvasImageAction::RotateClockwise, output, error);
    require(output.size() == 1 && output[0].pixelWidth == 6 && output[0].pixelHeight == 8,
            "Clockwise rotation did not swap pixel dimensions");

    output.clear();
    checkOperation(patterned, prototype::CanvasImageAction::RotateCounterclockwise, output, error);
    require(output.size() == 1 && output[0].pixelWidth == 6 && output[0].pixelHeight == 8,
            "Counterclockwise rotation did not swap pixel dimensions");

    const auto transparencySource = makeImage(makePng(4, 4,
                                                      [](int x, int, guchar* pixel) {
                                                          const bool white = x < 2;
                                                          pixel[0] = white ? 250 : 20;
                                                          pixel[1] = white ? 245 : 40;
                                                          pixel[2] = white ? 241 : 60;
                                                          pixel[3] = 255;
                                                      }),
                                              4, 4);
    output.clear();
    checkOperation(transparencySource, prototype::CanvasImageAction::MakeWhiteTransparent, output, error);
    auto* transparent = decode(output[0]);
    require(pixel(transparent, 0, 0)[3] == 0 && pixel(transparent, 3, 0)[3] == 255,
            "White transparency did not preserve non-white pixels");
    g_object_unref(transparent);

    const auto trimSource = makeImage(makePng(8, 8,
                                              [](int x, int y, guchar* pixel) {
                                                  const bool content = x >= 2 && x < 6 && y >= 1 && y < 7;
                                                  pixel[0] = content ? 30 : 255;
                                                  pixel[1] = content ? 40 : 255;
                                                  pixel[2] = content ? 50 : 255;
                                                  pixel[3] = 255;
                                              }),
                                      8, 8);
    output.clear();
    checkOperation(trimSource, prototype::CanvasImageAction::Trim, output, error);
    require(output.size() == 1 && output[0].pixelWidth < 8 && output[0].pixelHeight < 8,
            "Trim did not remove the white image border");

    const auto verticalSplitSource = makeImage(makePng(8, 8,
                                                       [](int, int y, guchar* pixel) {
                                                           const bool gap = y == 3;
                                                           pixel[0] = gap ? 255 : 0;
                                                           pixel[1] = gap ? 255 : 0;
                                                           pixel[2] = gap ? 255 : 0;
                                                           pixel[3] = 255;
                                                       }),
                                               8, 8);
    output.clear();
    checkOperation(verticalSplitSource, prototype::CanvasImageAction::SplitVertical, output, error);
    require(output.size() == 2 && output[0].pixelHeight == 4 && output[1].pixelHeight == 4 &&
                    output[1].transform.y > output[0].transform.y,
            "Vertical split did not divide at its largest white row block");

    const auto horizontalSplitSource = makeImage(makePng(8, 8,
                                                         [](int x, int, guchar* pixel) {
                                                             const bool gap = x == 3;
                                                             pixel[0] = gap ? 255 : 0;
                                                             pixel[1] = gap ? 255 : 0;
                                                             pixel[2] = gap ? 255 : 0;
                                                             pixel[3] = 255;
                                                         }),
                                                 8, 8);
    output.clear();
    checkOperation(horizontalSplitSource, prototype::CanvasImageAction::SplitHorizontal, output, error);
    require(output.size() == 2 && output[0].pixelWidth == 4 && output[1].pixelWidth == 4 &&
                    output[1].transform.x > output[0].transform.x,
            "Horizontal split did not divide at its largest white column block");

    const auto orientationPath =
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "test/files/images/r90.jpg";
    const auto oriented = prototype::readCanvasImageFile(orientationPath);
    require(oriented.pixelWidth == 130 && oriented.pixelHeight == 500,
            "Image fixture should expose its embedded 90-degree orientation");
    output.clear();
    checkOperation(oriented, prototype::CanvasImageAction::Invert, output, error);
    require(output.size() == 1 && output[0].pixelWidth == oriented.pixelWidth &&
                    output[0].pixelHeight == oriented.pixelHeight,
            "Image actions should process the displayed orientation of embedded-EXIF images");
}

}  // namespace

int main() {
    try {
        testImageActions();
        std::cout << "PASS: GTK4 canvas image actions"
                  << (prototype::canvasImageActionsAvailable() ? " (libvips)\n" : " (dependency check)\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
