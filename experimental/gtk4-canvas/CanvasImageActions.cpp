#include "CanvasImageActions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "ImageRenderer.h"

#ifdef GTK4_CANVAS_HAVE_VIPS
#include <vips/vips.h>
#endif

namespace prototype {
namespace {

#ifdef GTK4_CANVAS_HAVE_VIPS

struct ImageDeleter {
    void operator()(VipsImage* image) const {
        if (image) {
            g_object_unref(image);
        }
    }
};

using VipsImagePtr = std::unique_ptr<VipsImage, ImageDeleter>;

std::string vipsError() {
    const char* message = vips_error_buffer();
    std::string error = message ? message : "libvips image operation failed";
    vips_error_clear();
    return error;
}

bool initializeVips(std::string& error) {
    static std::once_flag once;
    static int result = 0;
    std::call_once(once, [] { result = vips_init("xournalpp-gtk4-canvas"); });
    if (result != 0) {
        error = vipsError();
        return false;
    }
    return true;
}

bool wrapOutput(int status, VipsImage* output, VipsImagePtr& target, std::string& error) {
    if (status != 0 || !output) {
        if (output) {
            g_object_unref(output);
        }
        error = vipsError();
        return false;
    }
    target.reset(output);
    return true;
}

template <typename Operation>
bool assignOutput(Operation&& operation, VipsImagePtr& target, std::string& error) {
    VipsImage* output = nullptr;
    const int status = operation(&output);
    return wrapOutput(status, output, target, error);
}

bool encodePng(VipsImage* image, std::string& data, std::string& error) {
    void* buffer = nullptr;
    size_t length = 0;
    if (vips_image_write_to_buffer(image, ".png", &buffer, &length, nullptr) != 0 || !buffer || length == 0) {
        g_free(buffer);
        error = vipsError();
        return false;
    }
    if (length > maxCanvasImageBytes) {
        g_free(buffer);
        error = "Processed image exceeds the 64 MiB limit";
        return false;
    }
    data.assign(static_cast<const char*>(buffer), length);
    g_free(buffer);
    return true;
}

struct ImageFrame {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

ImageFrame frameForImage(const CanvasImage& source) {
    const auto bounds = canvasImageBounds(source);
    return {source.transform.x, source.transform.y, static_cast<double>(std::ceil(bounds.x1 - bounds.x0)),
            static_cast<double>(std::ceil(bounds.y1 - bounds.y0))};
}

bool makeCanvasImage(const CanvasImage& source, VipsImage* output, ImageFrame frame, double pageWidth,
                     double pageHeight, CanvasImage& result, std::string& error) {
    const int width = vips_image_get_width(output);
    const int height = vips_image_get_height(output);
    if (width <= 0 || height <= 0 || static_cast<size_t>(width) > maxCanvasImagePixels / static_cast<size_t>(height)) {
        error = "Processed image dimensions exceed the 32 megapixel limit";
        return false;
    }
    if (!std::isfinite(frame.x) || !std::isfinite(frame.y) || !std::isfinite(frame.width) ||
        !std::isfinite(frame.height) || frame.width <= 0 || frame.height <= 0 || !std::isfinite(pageWidth) ||
        !std::isfinite(pageHeight) || pageWidth <= 0 || pageHeight <= 0) {
        error = "Processed image placement or page dimensions are invalid";
        return false;
    }
    const double frameRight = frame.x + frame.width;
    const double frameBottom = frame.y + frame.height;
    if (!std::isfinite(frameRight) || !std::isfinite(frameBottom)) {
        error = "Processed image placement exceeds the supported coordinate range";
        return false;
    }
    double scale = 1;
    if (frameRight > pageWidth || frameBottom > pageHeight) {
        const double scaleX = (pageWidth - frame.x) / frame.width;
        const double scaleY = (pageHeight - frame.y) / frame.height;
        scale = std::min(scaleX, scaleY);
    }
    if (!std::isfinite(scale) || scale <= 0) {
        error = "Processed image cannot fit on the current page";
        return false;
    }
    result = source;
    if (!encodePng(output, result.encodedData, error)) {
        return false;
    }
    result.pixelWidth = width;
    result.pixelHeight = height;
    try {
        result.surface = decodeCanvasImage(result.encodedData, result.pixelWidth, result.pixelHeight);
    } catch (const std::exception& exception) {
        error = std::string("Cannot decode processed image: ") + exception.what();
        return false;
    }
    result.transform = {scale * frame.width / width, 0, 0, scale * frame.height / height, frame.x, frame.y};
    if (!validCanvasImage(result)) {
        error = "Processed image has invalid dimensions or placement";
        return false;
    }
    return true;
}

#endif

}  // namespace

bool canvasImageActionsAvailable() {
#ifdef GTK4_CANVAS_HAVE_VIPS
    return true;
#else
    return false;
#endif
}

bool transformCanvasImage(const CanvasImage& image, CanvasImageAction action, double pageWidth, double pageHeight,
                          std::vector<CanvasImage>& results, std::string& error) {
    results.clear();
    error.clear();
#ifndef GTK4_CANVAS_HAVE_VIPS
    (void)image;
    (void)action;
    (void)pageWidth;
    (void)pageHeight;
    (void)results;
    error = "ImageActions requires libvips support in this GTK 4 build";
    return false;
#else
    if (!validCanvasImage(image) || !std::isfinite(pageWidth) || !std::isfinite(pageHeight) || pageWidth <= 0 ||
        pageHeight <= 0) {
        error = "Selected image or page dimensions are invalid";
        return false;
    }
    if (!initializeVips(error)) {
        return false;
    }

    VipsImagePtr encodedInput(
            vips_image_new_from_buffer(image.encodedData.data(), image.encodedData.size(), "", nullptr));
    if (!encodedInput) {
        error = vipsError();
        return false;
    }
    VipsImagePtr input;
    if (!assignOutput([&](VipsImage** target) { return vips_autorot(encodedInput.get(), target, nullptr); }, input,
                      error)) {
        return false;
    }
    if (vips_image_get_width(input.get()) != image.pixelWidth ||
        vips_image_get_height(input.get()) != image.pixelHeight) {
        error = "Selected image dimensions changed during decoding";
        return false;
    }

    ImageFrame frame = frameForImage(image);
    VipsImagePtr output;
    switch (action) {
        case CanvasImageAction::Invert:
            if (!assignOutput([&](VipsImage** target) { return vips_invert(input.get(), target, nullptr); }, output,
                              error)) {
                return false;
            }
            break;
        case CanvasImageAction::Downscale:
            if (!assignOutput([&](VipsImage** target) { return vips_resize(input.get(), target, 0.5, nullptr); },
                              output, error)) {
                return false;
            }
            break;
        case CanvasImageAction::FlipHorizontal:
        case CanvasImageAction::FlipVertical: {
            const auto direction =
                    action == CanvasImageAction::FlipHorizontal ? VIPS_DIRECTION_HORIZONTAL : VIPS_DIRECTION_VERTICAL;
            if (!assignOutput([&](VipsImage** target) { return vips_flip(input.get(), target, direction, nullptr); },
                              output, error)) {
                return false;
            }
            break;
        }
        case CanvasImageAction::RotateClockwise:
        case CanvasImageAction::RotateCounterclockwise: {
            const auto angle = action == CanvasImageAction::RotateClockwise ? VIPS_ANGLE_D90 : VIPS_ANGLE_D270;
            if (!assignOutput([&](VipsImage** target) { return vips_rot(input.get(), target, angle, nullptr); }, output,
                              error)) {
                return false;
            }
            std::swap(frame.width, frame.height);
            break;
        }
        case CanvasImageAction::MakeWhiteTransparent: {
            VipsImagePtr alphaInput;
            VipsImage* comparisonInput = input.get();
            if (vips_image_get_bands(input.get()) == 3) {
                if (!assignOutput(
                            [&](VipsImage** target) { return vips_bandjoin_const1(input.get(), target, 255, nullptr); },
                            alphaInput, error)) {
                    return false;
                }
                comparisonInput = alphaInput.get();
            }
            if (vips_image_get_bands(comparisonInput) != 4) {
                error = "Make white pixels transparent supports RGB and RGBA images";
                return false;
            }
            constexpr double white[]{240, 240, 240, -1};
            VipsImagePtr threshold;
            if (!assignOutput(
                        [&](VipsImage** target) { return vips_more_const(comparisonInput, target, white, 4, nullptr); },
                        threshold, error)) {
                return false;
            }
            VipsImagePtr mask;
            if (!assignOutput([&](VipsImage** target) { return vips_bandand(threshold.get(), target, nullptr); }, mask,
                              error)) {
                return false;
            }
            VipsImagePtr transparent;
            if (!assignOutput(
                        [&](VipsImage** target) {
                            return vips_black(target, vips_image_get_width(input.get()),
                                              vips_image_get_height(input.get()), "bands", 4, nullptr);
                        },
                        transparent, error)) {
                return false;
            }
            if (!assignOutput(
                        [&](VipsImage** target) {
                            return vips_ifthenelse(mask.get(), transparent.get(), comparisonInput, target, nullptr);
                        },
                        output, error)) {
                return false;
            }
            break;
        }
        case CanvasImageAction::Trim: {
            int left = 0;
            int top = 0;
            int width = 0;
            int height = 0;
            if (vips_find_trim(input.get(), &left, &top, &width, &height, nullptr) != 0) {
                error = vipsError();
                return false;
            }
            if (width <= 0 || height <= 0) {
                results.push_back(image);
                return true;
            }
            if (!assignOutput(
                        [&](VipsImage** target) {
                            return vips_extract_area(input.get(), target, left, top, width, height, nullptr);
                        },
                        output, error)) {
                return false;
            }
            frame.width = std::max(1.0, std::floor(frame.width * static_cast<double>(width) / image.pixelWidth));
            frame.height = std::max(1.0, std::floor(frame.height * static_cast<double>(height) / image.pixelHeight));
            break;
        }
        case CanvasImageAction::SplitVertical:
        case CanvasImageAction::SplitHorizontal: {
            const bool vertical = action == CanvasImageAction::SplitVertical;
            VipsImagePtr grey;
            if (!assignOutput(
                        [&](VipsImage** target) {
                            return vips_colourspace(input.get(), target, VIPS_INTERPRETATION_B_W, nullptr);
                        },
                        grey, error)) {
                return false;
            }
            VipsImagePtr band;
            if (!assignOutput([&](VipsImage** target) { return vips_extract_band(grey.get(), target, 0, nullptr); },
                              band, error)) {
                return false;
            }
            VipsImagePtr integerBand;
            if (!assignOutput(
                        [&](VipsImage** target) { return vips_cast(band.get(), target, VIPS_FORMAT_UINT, nullptr); },
                        integerBand, error)) {
                return false;
            }
            VipsImagePtr whiteRows;
            if (!assignOutput(
                        [&](VipsImage** target) { return vips_more_const1(integerBand.get(), target, 240, nullptr); },
                        whiteRows, error)) {
                return false;
            }
            VipsImagePtr columns;
            VipsImagePtr rows;
            VipsImage* columnsRaw = nullptr;
            VipsImage* rowsRaw = nullptr;
            if (vips_project(whiteRows.get(), &columnsRaw, &rowsRaw, nullptr) != 0 || !columnsRaw || !rowsRaw) {
                g_clear_object(&columnsRaw);
                g_clear_object(&rowsRaw);
                error = vipsError();
                return false;
            }
            columns.reset(columnsRaw);
            rows.reset(rowsRaw);
            VipsImagePtr sums;
            if (!assignOutput(
                        [&](VipsImage** target) {
                            return vips_cast(vertical ? rows.get() : columns.get(), target, VIPS_FORMAT_UINT, nullptr);
                        },
                        sums, error)) {
                return false;
            }
            size_t memorySize = 0;
            const auto* projection = vertical ? rows.get() : columns.get();
            if (vips_image_get_format(sums.get()) != VIPS_FORMAT_UINT || vips_image_get_bands(sums.get()) != 1 ||
                static_cast<std::uint64_t>(vips_image_get_width(projection)) *
                                static_cast<std::uint64_t>(vips_image_get_height(projection)) !=
                        static_cast<std::uint64_t>(vertical ? vips_image_get_height(input.get()) :
                                                              vips_image_get_width(input.get()))) {
                error = "Image projection dimensions are invalid for split";
                return false;
            }
            std::unique_ptr<void, decltype(&g_free)> memory(vips_image_write_to_memory(sums.get(), &memorySize),
                                                            g_free);
            if (!memory || memorySize != static_cast<size_t>(vertical ? vips_image_get_height(input.get()) :
                                                                        vips_image_get_width(input.get())) *
                                                 sizeof(std::uint32_t)) {
                error = "Cannot read image projection for split";
                return false;
            }
            const auto* sum = static_cast<const std::uint32_t*>(memory.get());
            const int width = vips_image_get_width(input.get());
            const int height = vips_image_get_height(input.get());
            const int last = vertical ? height : width;
            const std::uint64_t maxSum = static_cast<std::uint64_t>(255) * (vertical ? width : height);
            if (maxSum > std::numeric_limits<std::uint32_t>::max()) {
                error = "Image is too large to split safely";
                return false;
            }
            int longestEnd = 0;
            int longestLength = 0;
            int currentLength = 0;
            int currentEnd = 0;
            bool firstAfter = false;
            for (int index = 0; index < last; ++index) {
                if (sum[index] == maxSum) {
                    if (currentLength > 0) {
                        ++currentLength;
                    } else {
                        currentLength = 1;
                        firstAfter = true;
                    }
                } else if (firstAfter) {
                    currentEnd = index;
                    if (currentLength > longestLength) {
                        longestLength = currentLength;
                        longestEnd = currentEnd;
                    }
                    firstAfter = false;
                    currentLength = 0;
                }
            }
            if (longestLength == 0) {
                results.push_back(image);
                return true;
            }
            const int splitPosition = longestEnd - longestLength / 2;
            if (splitPosition <= 0 || splitPosition >= (vertical ? height : width)) {
                results.push_back(image);
                return true;
            }
            VipsImagePtr first;
            VipsImagePtr second;
            VipsImage* firstRaw = nullptr;
            VipsImage* secondRaw = nullptr;
            const int secondWidth = vertical ? width : width - splitPosition;
            const int secondHeight = vertical ? height - splitPosition : height;
            const auto firstWidth = vertical ? width : splitPosition;
            const auto firstHeight = vertical ? splitPosition : height;
            const auto firstStatus = vips_extract_area(input.get(), &firstRaw, 0, 0, firstWidth, firstHeight, nullptr);
            if (!wrapOutput(firstStatus, firstRaw, first, error)) {
                if (secondRaw) {
                    g_object_unref(secondRaw);
                }
                return false;
            }
            const auto secondStatus =
                    vips_extract_area(input.get(), &secondRaw, vertical ? 0 : splitPosition,
                                      vertical ? splitPosition : 0, secondWidth, secondHeight, nullptr);
            if (!wrapOutput(secondStatus, secondRaw, second, error)) {
                return false;
            }
            const auto originalFrame = frame;
            if (vertical) {
                const double maxHeight = std::ceil(originalFrame.height);
                const int firstHeightPx = vips_image_get_height(first.get());
                const int secondHeightPx = vips_image_get_height(second.get());
                ImageFrame firstFrame{originalFrame.x, originalFrame.y, originalFrame.width,
                                      originalFrame.width * firstHeightPx / vips_image_get_width(first.get())};
                ImageFrame secondFrame{
                        originalFrame.x, originalFrame.y + std::floor(splitPosition * maxHeight / height) + 15,
                        originalFrame.width, originalFrame.width * secondHeightPx / vips_image_get_width(second.get())};
                CanvasImage firstImage;
                CanvasImage secondImage;
                if (!makeCanvasImage(image, first.get(), firstFrame, pageWidth, pageHeight, firstImage, error) ||
                    !makeCanvasImage(image, second.get(), secondFrame, pageWidth, pageHeight, secondImage, error)) {
                    return false;
                }
                results.push_back(std::move(firstImage));
                results.push_back(std::move(secondImage));
            } else {
                const double maxWidth = std::ceil(originalFrame.width);
                const int firstWidthPx = vips_image_get_width(first.get());
                const int secondWidthPx = vips_image_get_width(second.get());
                ImageFrame firstFrame{originalFrame.x, originalFrame.y,
                                      originalFrame.height * firstWidthPx / vips_image_get_height(first.get()),
                                      originalFrame.height};
                ImageFrame secondFrame{originalFrame.x + std::floor(splitPosition * maxWidth / width) + 15,
                                       originalFrame.y,
                                       originalFrame.height * secondWidthPx / vips_image_get_height(second.get()),
                                       originalFrame.height};
                CanvasImage firstImage;
                CanvasImage secondImage;
                if (!makeCanvasImage(image, first.get(), firstFrame, pageWidth, pageHeight, firstImage, error) ||
                    !makeCanvasImage(image, second.get(), secondFrame, pageWidth, pageHeight, secondImage, error)) {
                    return false;
                }
                results.push_back(std::move(firstImage));
                results.push_back(std::move(secondImage));
            }
            return true;
        }
    }
    if (!output) {
        error = "Unsupported image action";
        return false;
    }
    if (!makeCanvasImage(image, output.get(), frame, pageWidth, pageHeight, results.emplace_back(), error)) {
        results.clear();
        return false;
    }
    return true;
#endif
}

}  // namespace prototype
