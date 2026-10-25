#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "CanvasState.h"

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

prototype::TextItem linkAt(double x, double y, size_t order, size_t layer = 1) {
    prototype::TextItem item;
    item.isLink = true;
    item.linkUrl = "https://example.org/";
    item.text = "link";
    item.page = 0;
    item.layerId = layer;
    item.layerOrder = order;
    item.x = x;
    item.y = y;
    item.layoutWidth = 24;
    item.layoutHeight = 12;
    return item;
}

}  // namespace

int main() {
    try {
        prototype::CanvasState state;
        state.setPages({{595, 842}});
        state.pageLayers[0].layers = {{"Lower", true, {0}}, {"Upper", true, {1}}};
        state.pageLayers[0].initialLayerCount = 2;
        state.pageLayers[0].selectedLayer = 1;

        auto earlier = linkAt(100, 100, 2);
        auto later = linkAt(100, 100, 9);
        auto otherPage = linkAt(100, 100, 0);
        otherPage.page = 1;
        auto ordinaryText = linkAt(100, 100, 1);
        ordinaryText.isLink = false;
        ordinaryText.linkUrl.clear();
        state.texts = {later, otherPage, ordinaryText, earlier};

        check(state.pickLink(0, {105, 105}) == 3,
              "Overlapping links should resolve to the first link in the native layer order");
        check(!state.pickLink(0, {125, 112}), "Link hit testing accepted a point outside its natural bounds");

        auto rotated = linkAt(200, 200, 4);
        rotated.xx = 0;
        rotated.yx = 1;
        rotated.xy = -1;
        rotated.yy = 0;
        state.texts = {rotated};
        check(state.pickLink(0, {196, 205}) == 0,
              "Transformed links should be hit in their local text rectangle");
        check(!state.pickLink(0, {205, 196}), "Rotated link hit testing accepted the wrong side of its bounds");

        state.texts = {linkAt(50, 50, 1, 2)};
        state.pageLayers[0].selectedLayer = 1;
        check(!state.pickLink(0, {55, 55}), "Link hit testing escaped the active layer");
        state.pageLayers[0].selectedLayer = 2;
        check(state.pickLink(0, {55, 55}) == 0, "Link hit testing missed a link on the active layer");
        state.pageLayers[0].layers[1].visible = false;
        check(!state.pickLink(0, {55, 55}), "Hidden-layer links should not respond to the Link tool");

        std::cout << "PASS: active-layer link hit testing honors native order, transforms and visibility\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
