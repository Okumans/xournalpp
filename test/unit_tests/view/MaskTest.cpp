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
