#pragma once

#include <vector>

#include <cairo.h>

namespace prototype {

struct Ink;

bool drawCachedInkRun(cairo_t* cr, const std::vector<const Ink*>& strokes, double requestedScale);
void setInkRasterVisiblePages(const std::vector<int>& pages);

}  // namespace prototype
