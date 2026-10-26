#include "CanvasPageLayout.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

struct PageSize {
    double width;
    double height;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireNear(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-9, message);
}

void testContinuousVerticalLayout() {
    const std::vector<PageSize> pages{{100, 200}, {80, 100}, {120, 150}};
    prototype::CanvasPageLayout layout;
    layout.configure(pages, {}, 15);

    require(layout.columnCount() == 1 && layout.rowCount() == 3, "Default layout should stack pages vertically");
    requireNear(layout.layoutWidth(), 120, "Vertical layout width should use its widest page");
    requireNear(layout.layoutHeight(), 480, "Vertical layout should retain page gaps");
    requireNear(layout.pageLeft(0, pages), 10, "Narrower pages should center inside the column");
    requireNear(layout.pageTop(1, pages), 215, "Second page should follow the first page and gap");
    require(layout.pageAt(20, 220, pages) == 1, "Page hit testing should resolve centered page cells");
    require(!layout.pageAt(20, 205, pages), "Page gaps should not be hittable");
    require(layout.pagesIntersecting(15, 220, 35, 240, pages) == std::vector<int>{1},
            "Viewport queries should return only the intersecting vertical page");
    require(layout.pagesIntersecting(0, 202, 120, 210, pages).empty(),
            "Viewport queries should omit page gaps");
}

void testHorizontalGridLayout() {
    const std::vector<PageSize> pages{{100, 200}, {80, 100}, {120, 150}, {90, 220}};
    prototype::CanvasLayoutSettings settings;
    settings.vertical = false;
    settings.columns = 2;
    prototype::CanvasPageLayout layout;
    layout.configure(pages, settings, 15);

    require(layout.columnCount() == 2 && layout.rowCount() == 2, "Two columns should form a two-by-two grid");
    require(layout.gridPosition(0).column == 0 && layout.gridPosition(0).row == 0,
            "First horizontal page should occupy the upper-left cell");
    require(layout.gridPosition(1).column == 1 && layout.gridPosition(1).row == 0,
            "Second horizontal page should occupy the upper-right cell");
    require(layout.gridPosition(2).column == 0 && layout.gridPosition(2).row == 1,
            "Third horizontal page should begin the next row");
    requireNear(layout.layoutWidth(), 225, "Grid width should include column widths and one gap");
    requireNear(layout.layoutHeight(), 435, "Grid height should include row heights and one gap");
    require(layout.pageAt(170, 60, pages) == 1, "Grid hit testing should resolve the upper-right page");
    require(!layout.pageAt(125, 60, pages), "Horizontal inter-column gap should not select a page");
}

void testPairedOffsetAndReadingDirection() {
    const std::vector<PageSize> pages{{100, 200}, {100, 200}, {100, 200}};
    prototype::CanvasLayoutSettings settings;
    settings.pairedPages = true;
    settings.pairedPageOffset = 1;
    settings.columns = 2;
    prototype::CanvasPageLayout layout;
    layout.configure(pages, settings, 15);

    require(layout.gridPosition(0).column == 1 && layout.gridPosition(0).row == 0,
            "Pair offset should put the first page in the right-hand opening cell");
    require(layout.gridPosition(1).column == 0 && layout.gridPosition(1).row == 1,
            "The next spread should start with the second page on the left");
    require(layout.gridPosition(2).column == 1 && layout.gridPosition(2).row == 1,
            "The third page should complete the second spread");

    settings.rightToLeft = true;
    settings.bottomToTop = true;
    layout.configure(pages, settings, 15);
    require(layout.gridPosition(0).column == 0 && layout.gridPosition(0).row == 1,
            "Reading directions should mirror paired-page grid positions");
    const double left = layout.pageLeft(0, pages);
    const double top = layout.pageTop(0, pages);
    require(layout.pagesIntersecting(left + 10, top + 10, left + 20, top + 20, pages) == std::vector<int>{0},
            "Viewport queries should honor paired-page reading directions");
}

void testLargeDocumentViewportQuery() {
    std::vector<PageSize> pages(10000, {100, 200});
    prototype::CanvasPageLayout layout;
    layout.configure(pages, {}, 15);

    constexpr int targetPage = 9876;
    const double top = layout.pageTop(targetPage, pages);
    const auto visible = layout.pagesIntersecting(10, top + 20, 90, top + 180, pages);
    require(visible == std::vector<int>{targetPage},
            "A large document viewport query should return only its visible page");
}

}  // namespace

int main() {
    try {
        testContinuousVerticalLayout();
        testHorizontalGridLayout();
        testPairedOffsetAndReadingDirection();
        testLargeDocumentViewportQuery();
        std::cout << "Canvas page layout checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Canvas page layout check failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
