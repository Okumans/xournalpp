#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include "gui/EraserCursor.h"

TEST(EraserCursorTest, footprintMatchesEachModeAtNormalAndHiDpiScales) {
    for (double width: {1.0, 8.5, 18.0}) {
        for (double zoom: {0.5, 1.0, 3.0}) {
            for (int scale: {1, 2}) {
                for (auto mode: {ERASER_TYPE_DEFAULT, ERASER_TYPE_WHITEOUT, ERASER_TYPE_DELETE_STROKE}) {
                    auto image = xoj::gui::createEraserCursorImage(width, zoom, mode, scale);
                    ASSERT_EQ(cairo_surface_status(image.surface.get()), CAIRO_STATUS_SUCCESS);
                    const auto expected =
                            std::max(5.0, std::ceil(width * zoom * (mode == ERASER_TYPE_WHITEOUT ? 1 : 2) + 4));
                    EXPECT_DOUBLE_EQ(cairo_image_surface_get_width(image.surface.get()), expected * scale);
                    EXPECT_DOUBLE_EQ(cairo_image_surface_get_height(image.surface.get()), expected * scale);
                    EXPECT_DOUBLE_EQ(image.hotspot, expected / 2);
                    double sx, sy;
                    cairo_surface_get_device_scale(image.surface.get(), &sx, &sy);
                    EXPECT_DOUBLE_EQ(sx, scale);
                    EXPECT_DOUBLE_EQ(sy, scale);
                }
            }
        }
    }
}

TEST(EraserCursorTest, whiteoutIsRoundAndTheOutlineDoesNotObscureTheCenter) {
    auto alphaAt = [](const xoj::gui::EraserCursorImage& image, int x, int y) {
        cairo_surface_flush(image.surface.get());
        auto* data = cairo_image_surface_get_data(image.surface.get());
        const auto stride = cairo_image_surface_get_stride(image.surface.get());
        return reinterpret_cast<const uint32_t*>(data + stride * y)[x] >> 24;
    };
    // All images have a 40px footprint, independently of the mode's width convention.
    auto square = xoj::gui::createEraserCursorImage(20, 1, ERASER_TYPE_DEFAULT, 1);
    auto circle = xoj::gui::createEraserCursorImage(40, 1, ERASER_TYPE_WHITEOUT, 1);
    auto deleting = xoj::gui::createEraserCursorImage(20, 1, ERASER_TYPE_DELETE_STROKE, 1);
    EXPECT_GT(alphaAt(square, 2, 2), 0);
    EXPECT_EQ(alphaAt(circle, 2, 2), 0);
    EXPECT_EQ(alphaAt(square, 22, 22), 0);
    EXPECT_EQ(alphaAt(circle, 22, 22), 0);
    EXPECT_GT(alphaAt(deleting, 22, 22), 0);
}
