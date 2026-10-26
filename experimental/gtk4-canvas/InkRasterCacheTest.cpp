#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "InkRasterCache.h"
#include "InkRenderer.h"

namespace {

struct Surface {
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 240, 180);
    cairo_t* cr = cairo_create(surface);

    ~Surface() {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
    }

    void fill(double red, double green, double blue) {
        cairo_save(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgb(cr, red, green, blue);
        cairo_paint(cr);
        cairo_restore(cr);
    }
};

std::array<uint8_t, 4> pixel(cairo_surface_t* surface, int x, int y) {
    cairo_surface_flush(surface);
    std::array<uint8_t, 4> value{};
    const auto* data = cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface) + x * 4;
    std::memcpy(value.data(), data, value.size());
    return value;
}

bool samePixels(cairo_surface_t* expected, cairo_surface_t* actual, uint8_t tolerance = 1) {
    for (int y = 0; y < 180; ++y) {
        for (int x = 0; x < 240; ++x) {
            const auto a = pixel(expected, x, y);
            const auto b = pixel(actual, x, y);
            for (size_t channel = 0; channel < a.size(); ++channel) {
                if (std::abs(static_cast<int>(a[channel]) - static_cast<int>(b[channel])) > tolerance) {
                    return false;
                }
            }
        }
    }
    return true;
}

void drawReference(cairo_t* cr, const std::vector<prototype::Ink>& strokes) {
    for (const auto& stroke: strokes) {
        prototype::drawInk(cr, stroke);
    }
}

std::vector<const prototype::Ink*> references(const std::vector<prototype::Ink>& strokes) {
    std::vector<const prototype::Ink*> result;
    result.reserve(strokes.size());
    for (const auto& stroke: strokes) {
        result.push_back(&stroke);
    }
    return result;
}

bool committedRunMatchesAndExtends() {
    prototype::InkRasterCache cache;
    std::vector<prototype::Ink> strokes{
            {0, {{20, 30}, {170, 140}}, {prototype::Tool::Pen, {1, 0, 0}, 8}},
    };

    Surface expected;
    Surface actual;
    expected.fill(1, 1, 1);
    actual.fill(1, 1, 1);
    drawReference(expected.cr, strokes);
    cache.beginFrame({0});
    const bool firstDrawn = cache.drawCommitted(actual.cr, references(strokes), 1.0);
    cache.endFrame();
    if (!firstDrawn || !samePixels(expected.surface, actual.surface) || cache.stats().renders != 1) {
        return false;
    }

    strokes.push_back({0, {{80, 70}, {95, 80}}, {prototype::Tool::Pen, {1, 0, 0}, 8}});
    expected.fill(1, 1, 1);
    actual.fill(1, 1, 1);
    drawReference(expected.cr, strokes);
    cache.beginFrame({0});
    const bool appended = cache.drawCommitted(actual.cr, references(strokes), 1.0);
    cache.endFrame();
    if (!appended || !samePixels(expected.surface, actual.surface) || cache.stats().extensions != 1 ||
        cache.stats().bytesCopied != 0) {
        return false;
    }

    strokes.push_back({0, {{190, 35}, {220, 70}}, {prototype::Tool::Pen, {1, 0, 0}, 8}});
    expected.fill(1, 1, 1);
    actual.fill(1, 1, 1);
    drawReference(expected.cr, strokes);
    cache.beginFrame({0});
    const bool expanded = cache.drawCommitted(actual.cr, references(strokes), 1.0);
    cache.endFrame();
    if (!expanded || !samePixels(expected.surface, actual.surface) || cache.stats().extensions != 2 ||
        cache.stats().bytesCopied == 0) {
        return false;
    }

    expected.fill(1, 1, 1);
    actual.fill(1, 1, 1);
    drawReference(expected.cr, strokes);
    cache.beginFrame({0});
    const bool hit = cache.drawCommitted(actual.cr, references(strokes), 1.0);
    cache.endFrame();
    return hit && samePixels(expected.surface, actual.surface) && cache.stats().hits == 1;
}

bool highlighterBlendMatches() {
    prototype::InkRasterCache cache;
    std::vector<prototype::Ink> strokes{{0, {{25, 80}, {125, 80}}, {prototype::Tool::Highlighter, {1, 0.86, 0}, 24}}};
    Surface expected;
    Surface actual;
    expected.fill(0.08, 0.12, 0.2);
    actual.fill(0.08, 0.12, 0.2);
    drawReference(expected.cr, strokes);
    cache.beginFrame({0});
    const bool initial = cache.drawCommitted(actual.cr, references(strokes), 1.0);
    cache.endFrame();
    if (!initial || !samePixels(expected.surface, actual.surface, 2)) {
        return false;
    }

    strokes.push_back({0, {{78, 80}, {82, 80}}, {prototype::Tool::Highlighter, {1, 0.86, 0}, 24}});
    expected.fill(0.08, 0.12, 0.2);
    actual.fill(0.08, 0.12, 0.2);
    drawReference(expected.cr, strokes);
    cache.beginFrame({0});
    const bool appended = cache.drawCommitted(actual.cr, references(strokes), 1.0);
    cache.endFrame();
    return appended && samePixels(expected.surface, actual.surface, 2) && cache.stats().extensions == 1 &&
           cache.stats().bytesCopied == 0;
}

bool fortyStrokesStayInOneRasterRun() {
    prototype::InkRasterCache cache;
    std::vector<prototype::Ink> strokes;
    strokes.reserve(40);
    strokes.push_back({0, {{20, 20}, {200, 150}}, {prototype::Tool::Pen, {0.1, 0.2, 0.3}, 8}});
    Surface expected;
    Surface actual;

    for (int i = 0; i < 40; ++i) {
        if (i > 0) {
            const double offset = static_cast<double>(i * 2);
            strokes.push_back({0,
                               {{40 + offset, 30 + offset}, {65 + offset, 36 + offset}},
                               {prototype::Tool::Pen, {0.1, 0.2, 0.3}, 8}});
        }
        expected.fill(1, 1, 1);
        actual.fill(1, 1, 1);
        drawReference(expected.cr, strokes);
        cache.beginFrame({0});
        const bool drawn = cache.drawCommitted(actual.cr, references(strokes), 1.0);
        cache.endFrame();
        if (!drawn || !samePixels(expected.surface, actual.surface)) {
            return false;
        }
    }

    return cache.stats().renders == 1 && cache.stats().extensions == 39 && cache.stats().bytesCopied == 0;
}

bool activeStrokeAppendsWithoutChangingPixels(prototype::Tool tool, bool dashed, bool pressureSensitive) {
    prototype::InkRasterCache cache;
    prototype::Brush brush{tool, {1, 0.86, 0}, 24};
    if (dashed) {
        brush.lineDashes = {14, 8};
    }
    prototype::Ink stroke{0,
                          pressureSensitive ? std::vector<prototype::Point>{{25, 55, 0.35}, {105, 55, 0.8}} :
                                              std::vector<prototype::Point>{{25, 55}, {105, 55}},
                          brush, pressureSensitive};
    Surface expected;
    Surface actual;

    const auto drawFrame = [&] {
        expected.fill(0.08, 0.12, 0.2);
        actual.fill(0.08, 0.12, 0.2);
        prototype::drawInk(expected.cr, stroke);
        cache.beginFrame({0});
        const bool drawn = cache.drawActive(actual.cr, stroke, 1.0, 0, 0, 220, 160, true);
        cache.endFrame();
        return drawn && samePixels(expected.surface, actual.surface, 2);
    };

    if (!drawFrame()) {
        return false;
    }
    stroke.points.push_back(pressureSensitive ? prototype::Point{105, 115, 0.65} : prototype::Point{105, 115});
    if (!drawFrame() || cache.stats().liveAppends != 1) {
        return false;
    }
    stroke.points.push_back(pressureSensitive ? prototype::Point{160, 115, 0.7} : prototype::Point{160, 115});
    if (!drawFrame() || cache.stats().liveAppends != 2) {
        return false;
    }
    return drawFrame() && cache.stats().liveAppends == 2;
}

bool activeStrokeRebuildsForMutablePath() {
    prototype::InkRasterCache cache;
    prototype::Ink stroke{0, {{25, 55}, {105, 55}}, {prototype::Tool::Pen, {1, 0, 0}, 12}};
    Surface expected;
    Surface actual;

    const auto drawFrame = [&] {
        expected.fill(1, 1, 1);
        actual.fill(1, 1, 1);
        prototype::drawInk(expected.cr, stroke);
        cache.beginFrame({0});
        const bool drawn = cache.drawActive(actual.cr, stroke, 1.0, 0, 0, 220, 160, false);
        cache.endFrame();
        return drawn && samePixels(expected.surface, actual.surface);
    };

    if (!drawFrame()) {
        return false;
    }
    stroke.points.front().y += 20;
    return drawFrame() && cache.stats().liveRebuilds == 2 && cache.stats().liveAppends == 0;
}

bool visiblePageRunsArePrunedAndReset() {
    prototype::InkRasterCache cache;
    std::vector<prototype::Ink> first{{0, {{20, 25}, {90, 25}}, {prototype::Tool::Pen, {0, 0, 0}, 8}}};
    std::vector<prototype::Ink> replacement{{0, {{20, 45}, {90, 45}}, {prototype::Tool::Pen, {0, 0, 0}, 8}}};
    Surface actual;

    cache.beginFrame({0});
    const bool firstDrawn = cache.drawCommitted(actual.cr, references(first), 1.0);
    cache.endFrame();
    const auto previousBytes = cache.stats().cachedBytes;

    cache.beginFrame({0});
    const bool replacementDrawn = cache.drawCommitted(actual.cr, references(replacement), 1.0);
    cache.endFrame();
    const auto replacementBytes = cache.stats().cachedBytes;
    if (!firstDrawn || !replacementDrawn || previousBytes == 0 || replacementBytes == 0 ||
        replacementBytes >= previousBytes * 2) {
        return false;
    }

    cache.clear();
    return cache.stats().cachedBytes == 0 && cache.stats().liveBytes == 0;
}

}  // namespace

int main() {
    const bool committed = committedRunMatchesAndExtends();
    const bool highlighter = highlighterBlendMatches();
    const bool fortyStrokes = fortyStrokesStayInOneRasterRun();
    const bool activePen = activeStrokeAppendsWithoutChangingPixels(prototype::Tool::Pen, false, false);
    const bool activeDashedPen = activeStrokeAppendsWithoutChangingPixels(prototype::Tool::Pen, true, false);
    const bool activePressurePen = activeStrokeAppendsWithoutChangingPixels(prototype::Tool::Pen, false, true);
    const bool activeHighlighter = activeStrokeAppendsWithoutChangingPixels(prototype::Tool::Highlighter, false, false);
    const bool activeMutation = activeStrokeRebuildsForMutablePath();
    const bool activeWhiteout = activeStrokeAppendsWithoutChangingPixels(prototype::Tool::Eraser, false, false);
    const bool lifecycle = visiblePageRunsArePrunedAndReset();
    const bool active =
            activePen && activeDashedPen && activePressurePen && activeHighlighter && activeMutation && activeWhiteout;
    const bool pass = committed && highlighter && fortyStrokes && active && lifecycle;
    std::cout << "committed=" << committed << " highlighter=" << highlighter << " active-pen=" << activePen
              << " active-dashed=" << activeDashedPen << " active-pressure=" << activePressurePen
              << " active-highlighter=" << activeHighlighter << " active-mutation=" << activeMutation
              << " active-whiteout=" << activeWhiteout << " forty-strokes=" << fortyStrokes
              << " lifecycle=" << lifecycle << '\n';
    std::cout << (pass ? "PASS" : "FAIL") << ": committed and live ink raster cache semantics\n";
    return pass ? 0 : 1;
}
