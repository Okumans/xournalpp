#include <gtest/gtest.h>

#include "control/zoom/ZoomControl.h"
#include "control/zoom/ZoomListener.h"

namespace {
class ViewListener: public ZoomListener {
public:
    void zoomChanged() override { ++updates; }
    int updates = 0;
};
}  // namespace

TEST(ZoomControl, followsFingerMovementWhenPinchScaleDoesNotChange) {
    ZoomControl zoom;
    ViewListener view;
    zoom.addZoomListener(&view);

    zoom.zoomSequenceChange(1.0, true, {5, -8});
    EXPECT_DOUBLE_EQ(zoom.getZoom(), 1.0);
    EXPECT_EQ(view.updates, 1);

    zoom.zoomSequenceChange(1.0, true, {-2, 3});
    EXPECT_EQ(view.updates, 2);

    // An unchanged scale with no translation should not redraw the view.
    zoom.zoomSequenceChange(1.0, true, {0, 0});
    EXPECT_EQ(view.updates, 2);
    zoom.removeZoomListener(&view);
}
