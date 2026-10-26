#include <cstdint>
#include <cstring>
#include <iostream>

#include "InkRenderer.h"

static uint32_t pixel(cairo_surface_t* surface, int x, int y) {
    cairo_surface_flush(surface);
    uint32_t value;
    std::memcpy(&value, cairo_image_surface_get_data(surface) + y * cairo_image_surface_get_stride(surface) + x * 4, 4);
    return value & 0xffffff;
}

int main() {
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 240, 160);
    auto* cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    prototype::Ink highlight{
            0, {{20, 40, 0.1}, {120, 40, 1}, {80, 40, 0.3}}, {prototype::Tool::Highlighter, {1, 1, 0}, 20}};
    prototype::drawInk(cr, highlight);
    bool pass = pixel(surface, 40, 40) == pixel(surface, 90, 40);
    pass &= pixel(surface, 40, 40) != 0xffff00 && pixel(surface, 40, 40) != 0xffffff;
    // GTK 3 StrokeView uses 0.47 opacity: yellow over white leaves about 135 blue.
    pass &= (pixel(surface, 40, 40) & 0xff) >= 134 && (pixel(surface, 40, 40) & 0xff) <= 136;
    prototype::Ink pdfHighlight{0, {{20, 70}, {60, 70}},
                                {prototype::Tool::Highlighter, {1, 1, 0}, 10}};
    pdfHighlight.fill = 60;
    pdfHighlight.capStyle = 1;
    prototype::drawInk(cr, pdfHighlight);
    pass &= (pixel(surface, 40, 70) & 0xff) >= 194 && (pixel(surface, 40, 70) & 0xff) <= 196;
    pass &= pixel(surface, 18, 70) == 0xffffff;
    // Highlighting preserves black document text under a stroke.
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_rectangle(cr, 150, 30, 10, 20);
    cairo_fill(cr);
    highlight.points = {{140, 40}, {200, 40}};
    prototype::drawInk(cr, highlight);
    pass &= pixel(surface, 155, 40) == 0;
    prototype::drawInk(cr, {0, {{20, 90}, {200, 90}}, {prototype::Tool::Pen, {1, 0, 0}, 4}});
    prototype::drawInk(cr, {0, {{20, 120}, {200, 120}}, {prototype::Tool::Pen, {0, 0, 1}, 12}});
    pass &= pixel(surface, 80, 90) == 0xff0000 && pixel(surface, 80, 120) == 0x0000ff;
    pass &= pixel(surface, 80, 94) == 0xffffff && pixel(surface, 80, 124) == 0x0000ff;
    // Whiteout (Tool::Eraser) paints opaque white at constant width over underlying ink/text,
    // while later pen strokes draw cleanly on top of whiteout.
    prototype::drawInk(cr, {0, {{60, 120, 0.1}, {120, 120, 0.2}}, {prototype::Tool::Eraser, {0, 0, 0}, 16}});
    pass &= pixel(surface, 90, 120) == 0xffffff && pixel(surface, 90, 126) == 0xffffff;
    pass &= pixel(surface, 40, 120) == 0x0000ff;
    prototype::drawInk(cr, {0, {{90, 105}, {90, 135}}, {prototype::Tool::Pen, {0, 1, 0}, 4}});
    pass &= pixel(surface, 90, 120) == 0x00ff00;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    prototype::Ink filledRectangle{0,
                                   {{20, 65}, {120, 65}, {120, 105}, {20, 105}, {20, 65}},
                                   {prototype::Tool::Pen, {1, 0, 0}, 4}};
    filledRectangle.fill = 128;
    prototype::drawInk(cr, filledRectangle);
    pass &= pixel(surface, 80, 85) >= 0xff7e7e && pixel(surface, 80, 85) <= 0xff8080 &&
            pixel(surface, 20, 85) == 0xff0000;
    prototype::Ink filledHighlight{0,
                                   {{130, 65}, {220, 65}, {220, 105}, {130, 105}, {130, 65}},
                                   {prototype::Tool::Highlighter, {1, 1, 0}, 4}};
    filledHighlight.fill = 128;
    prototype::drawInk(cr, filledHighlight);
    pass &= (pixel(surface, 170, 85) & 0xff) >= 126 && (pixel(surface, 170, 85) & 0xff) <= 128;
    // Selection preview transforms points; committed ink and autosave stay unchanged.
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    prototype::CanvasState state;
    state.ink = {{0, {{20, 20}, {20, 20}}, {prototype::Tool::Pen, {1, 0, 0}, 8}}};
    state.selectAllOnPage();
    state.beginSelection(20, 20);
    state.moveSelectionPointer(50, 40);
    prototype::drawInkPreview(cr, state, 0);
    pass &= pixel(surface, 50, 40) == 0xff0000 && pixel(surface, 20, 20) == 0xffffff;
    pass &= state.ink[0].points[0].x == 20 && state.revision() == 0;
    state.cancelSelection();
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    prototype::Ink line{0, {{20,80},{60,80}}, {prototype::Tool::Pen,{1,0,0},8}};
    // Stretch X by four: geometry doubles twice, width grows by sqrt(4)=2
    // in BOTH directions, unlike anisotropic Cairo scaling of an 8 pt pen.
    prototype::drawInk(cr,line,{4,0,0,1,-60,0,2});
    pass &= pixel(surface, 100,80)==0xff0000 && pixel(surface,100,85)==0xff0000;
    pass &= pixel(surface,100,88)==0xffffff && pixel(surface,190,80)==0xffffff;
    pass &= line.points[1].x==60 && line.brush.width==8;
    cairo_matrix_t matrix;
    cairo_get_matrix(cr, &matrix);

    pass &= matrix.x0 == 0 && matrix.y0 == 0;
    pass &= cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    pass &= cairo_get_operator(cr) == CAIRO_OPERATOR_OVER;
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    std::cout << (pass ? "PASS" : "FAIL") << ": color, width, uniform highlight opacity, readable text, Cairo state\n";
    return pass ? 0 : 1;
}
