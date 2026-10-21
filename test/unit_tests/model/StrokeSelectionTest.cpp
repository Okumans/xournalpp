#include <algorithm>

#include <gtest/gtest.h>

#include "model/Stroke.h"
#include "util/raii/CairoWrappers.h"

TEST(StrokeSelection, FlatHighlighterEndsExcludeBlankSpace) {
    Stroke stroke;
    stroke.setWidth(40.);
    stroke.setToolType(StrokeTool::HIGHLIGHTER);
    stroke.setStrokeCapStyle(StrokeCapStyle::BUTT);
    stroke.addPoint(Point(100., 100.));
    stroke.addPoint(Point(200., 100.));

    EXPECT_DOUBLE_EQ(stroke.distanceTo(210., 100.), 10.);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(90., 100.), 10.);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(150., 119.), 0.);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(150., 122.), 2.);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(203., 124.), 5.);

    const auto bounds = stroke.getSelectionBounds();
    EXPECT_DOUBLE_EQ(bounds.x, 100.);
    EXPECT_DOUBLE_EQ(bounds.y, 80.);
    EXPECT_DOUBLE_EQ(bounds.width, 100.);
    EXPECT_DOUBLE_EQ(bounds.height, 40.);
    EXPECT_LT(stroke.getBoundingBox().x, bounds.x);
}

TEST(StrokeSelection, RepeatedEndpointsKeepFlatCaps) {
    Stroke stroke;
    stroke.setWidth(40.);
    stroke.setStrokeCapStyle(StrokeCapStyle::BUTT);
    stroke.addPoint(Point(100., 100.));
    stroke.addPoint(Point(100., 100.));
    stroke.addPoint(Point(200., 100.));
    stroke.addPoint(Point(200., 100.));

    EXPECT_DOUBLE_EQ(stroke.distanceTo(210., 100.), 10.);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(90., 100.), 10.);
}

TEST(StrokeSelection, RoundCapsAndPressureDotsRemainSelectable) {
    Stroke stroke;
    stroke.setWidth(40.);
    stroke.addPoint(Point(100., 100.));
    stroke.addPoint(Point(200., 100.));
    EXPECT_DOUBLE_EQ(stroke.distanceTo(210., 100.), 0.);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(222., 100.), 2.);

    stroke.setPointVector({Point(100., 100., 40.), Point(100., 100., 40.)});
    stroke.setStrokeCapStyle(StrokeCapStyle::BUTT);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(110., 100.), 0.);
    EXPECT_DOUBLE_EQ(stroke.distanceTo(122., 100.), 2.);
}

TEST(StrokeSelection, HitTestingMatchesCairoCapsAndRoundJoins) {
    for (auto cap: {StrokeCapStyle::ROUND, StrokeCapStyle::BUTT, StrokeCapStyle::SQUARE}) {
        for (bool reverse: {false, true}) {
            Stroke stroke;
            stroke.setWidth(12.);
            stroke.setStrokeCapStyle(cap);
            std::vector<Point> points = {Point(20., 30.), Point(60., 60.), Point(90., 20.)};
            if (reverse) {
                std::reverse(points.begin(), points.end());
            }
            stroke.setPointVector(points);

            xoj::util::CairoSurfaceSPtr surface(cairo_recording_surface_create(CAIRO_CONTENT_ALPHA, nullptr),
                                                xoj::util::adopt);
            xoj::util::CairoSPtr cr(cairo_create(surface.get()), xoj::util::adopt);
            cairo_set_line_width(cr.get(), stroke.getWidth());
            cairo_set_line_join(cr.get(), CAIRO_LINE_JOIN_ROUND);
            cairo_set_line_cap(cr.get(), cap == StrokeCapStyle::ROUND ? CAIRO_LINE_CAP_ROUND :
                                         cap == StrokeCapStyle::BUTT  ? CAIRO_LINE_CAP_BUTT :
                                                                        CAIRO_LINE_CAP_SQUARE);
            cairo_move_to(cr.get(), points.front().x, points.front().y);
            for (size_t i = 1; i < points.size(); ++i) {
                cairo_line_to(cr.get(), points[i].x, points[i].y);
            }

            for (double x = 8.125; x < 104.; x += 2.) {
                for (double y = 8.375; y < 74.; y += 2.) {
                    EXPECT_EQ(stroke.distanceTo(x, y) == 0., bool(cairo_in_stroke(cr.get(), x, y)))
                            << "cap=" << cap << " reverse=" << reverse << " x=" << x << " y=" << y;
                }
            }
        }
    }
}
