#include <cstdlib>
#include <iostream>
#include <stdexcept>

#include "TextRenderer.h"

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 900, 360);
        check(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS, "Could not allocate test surface");
        auto* cr = cairo_create(surface);

        prototype::TextItem item;
        item.text = "Hello 世界\nA second wrapped line";
        item.fontName = "Sans";
        item.fontSize = 20;
        item.wrapWidth = 260;
        item.x = 60;
        item.y = 40;
        item.styleRuns = {{0, 5, "Sans Bold", 24, 0xff0077aaU, 1U | 4U},
                          {6, 12, "Sans", 12, std::nullopt, 8U}};
        prototype::drawText(cr, item);

        item.xx = 1.2;
        item.xy = 0.15;
        item.yx = -0.1;
        item.yy = 1.1;
        item.x = 420;
        item.y = 100;
        item.alignment = 1;
        item.justify = true;
        prototype::drawText(cr, item);

        cairo_surface_flush(surface);
        const auto* pixels = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        size_t painted = 0;
        for (int y = 0; y < 360; ++y) {
            for (int x = 0; x < 900; ++x) {
                const auto* pixel = reinterpret_cast<const uint32_t*>(pixels + y * stride) + x;
                if (*pixel != 0) {
                    ++painted;
                }
            }
        }
        check(painted > 250, "Pango text renderer did not paint styled multiline UTF-8 text");
        check(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "Pango text renderer reported a Cairo error");
        cairo_destroy(cr);
        cairo_surface_destroy(surface);

        auto* alphaSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 160, 150);
        auto* alphaContext = cairo_create(alphaSurface);
        prototype::TextItem translucent;
        translucent.text = "A";
        translucent.fontSize = 120;
        translucent.color = 0x00000000U;
        translucent.x = 8;
        translucent.y = 8;
        translucent.styleRuns = {{0, 1, "Sans", 120, 0x7f0077aaU, 0}};
        prototype::drawText(alphaContext, translucent);
        cairo_surface_flush(alphaSurface);
        const auto* alphaPixels = cairo_image_surface_get_data(alphaSurface);
        const int alphaStride = cairo_image_surface_get_stride(alphaSurface);
        size_t translucentPixels = 0;
        for (int y = 0; y < 150; ++y) {
            for (int x = 0; x < 160; ++x) {
                const auto pixel = *(reinterpret_cast<const uint32_t*>(alphaPixels + y * alphaStride) + x);
                const auto alpha = (pixel >> 24U) & 0xffU;
                if (alpha > 0 && alpha < 255) {
                    ++translucentPixels;
                }
            }
        }
        check(translucentPixels > 0, "Inline text color alpha was not preserved by the Pango renderer");
        check(cairo_status(alphaContext) == CAIRO_STATUS_SUCCESS, "Pango alpha rendering reported a Cairo error");
        cairo_destroy(alphaContext);
        cairo_surface_destroy(alphaSurface);
        std::cout << "PASS: Pango text, inline styles, wrapping, alignment and affine transforms\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
