#include <vector>

#include <gtest/gtest.h>

#include "util/Range.h"
#include "view/Mask.h"

TEST(Mask, TracksEstimatedMemoryAndReset) {
    const Range extent(10.0, 20.0, 110.0, 220.0);
    xoj::view::Mask mask(1, extent, 2.0, CAIRO_CONTENT_COLOR_ALPHA);

    EXPECT_TRUE(mask.isInitialized());
    EXPECT_EQ(mask.getEstimatedMemoryBytes(), 320000U);

    mask.reset();
    EXPECT_FALSE(mask.isInitialized());
    EXPECT_EQ(mask.getEstimatedMemoryBytes(), 0U);
}

TEST(Mask, AccountsForDpiScalingInMemoryEstimate) {
    xoj::view::Mask mask(2, Range(0.0, 0.0, 100.0, 200.0), 2.0, CAIRO_CONTENT_ALPHA);

    EXPECT_EQ(mask.getEstimatedMemoryBytes(), 320000U);
}

TEST(Mask, FastPreviewFilterAppliesToThePaintedSurfaceAndPreservesTargetState) {
    xoj::view::Mask mask(1, Range(0.0, 0.0, 2.0, 2.0), 1.0, CAIRO_CONTENT_COLOR_ALPHA);
    cairo_set_source_rgb(mask.get(), 1.0, 1.0, 1.0);
    cairo_rectangle(mask.get(), 0.0, 0.0, 1.0, 2.0);
    cairo_fill(mask.get());

    const auto render = [&](cairo_filter_t filter, bool useMask) {
        xoj::util::CairoSurfaceSPtr surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 6, 6), xoj::util::adopt);
        xoj::util::CairoSPtr cr(cairo_create(surface.get()), xoj::util::adopt);
        cairo_scale(cr.get(), 3.0, 3.0);
        cairo_set_source_rgb(cr.get(), 0.25, 0.5, 0.75);
        auto* originalSource = cairo_get_source(cr.get());
        if (useMask) {
            mask.paintTo(cr.get(), filter);
            EXPECT_EQ(cairo_get_source(cr.get()), originalSource);
            cairo_matrix_t matrix;
            cairo_get_matrix(cr.get(), &matrix);
            EXPECT_EQ(matrix.xx, 3.0);
            EXPECT_EQ(matrix.yy, 3.0);
        } else {
            cairo_set_source_surface(cr.get(), cairo_get_target(mask.get()), 0.0, 0.0);
            cairo_pattern_set_filter(cairo_get_source(cr.get()), filter);
            cairo_paint(cr.get());
        }
        cairo_surface_flush(surface.get());
        auto* data = cairo_image_surface_get_data(surface.get());
        return std::vector<unsigned char>(data, data + 6 * cairo_image_surface_get_stride(surface.get()));
    };

    EXPECT_EQ(render(CAIRO_FILTER_FAST, true), render(CAIRO_FILTER_FAST, false));
    EXPECT_EQ(render(CAIRO_FILTER_GOOD, true), render(CAIRO_FILTER_GOOD, false));
    EXPECT_NE(render(CAIRO_FILTER_FAST, true), render(CAIRO_FILTER_GOOD, true));
}
