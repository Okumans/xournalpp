#include "InkRasterCache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "CanvasState.h"
#include "InkRenderer.h"

namespace prototype {
namespace {

constexpr size_t maxCachedBytes = 64 * 1024 * 1024;
constexpr int maxRasterDimension = 16384;
constexpr double maxRasterScale = 8.0;
constexpr double minRasterScale = 0.125;
constexpr double rasterPadding = 1.0;

struct SurfaceDeleter {
    void operator()(cairo_surface_t* surface) const { cairo_surface_destroy(surface); }
};

struct RasterBounds {
    int page = -1;
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
};

struct CachedInkRun {
    std::unique_ptr<cairo_surface_t, SurfaceDeleter> surface;
    std::vector<uint64_t> strokeKeys;
    double x = 0;
    double y = 0;
    double scale = 1;
    int page = -1;
    bool highlighter = false;
    size_t bytes = 0;
    uint64_t stamp = 0;
};

std::optional<RasterBounds> rasterBounds(const Ink& stroke) {
    if (stroke.points.empty() || !std::isfinite(stroke.brush.width) || stroke.brush.width <= 0) {
        return std::nullopt;
    }

    const bool highlight = stroke.brush.tool == Tool::Highlighter;
    const bool whiteout = stroke.brush.tool == Tool::Eraser;
    const bool pressureSensitive = stroke.pressureSensitive && !highlight && !whiteout;
    double x0 = stroke.points.front().x;
    double y0 = stroke.points.front().y;
    double x1 = x0;
    double y1 = y0;
    double maxPressure = pressureSensitive ? stroke.points.front().pressure : 1.0;
    for (const auto& point: stroke.points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.pressure)) {
            return std::nullopt;
        }
        x0 = std::min(x0, point.x);
        y0 = std::min(y0, point.y);
        x1 = std::max(x1, point.x);
        y1 = std::max(y1, point.y);
        if (pressureSensitive) {
            maxPressure = std::max(maxPressure, point.pressure);
        }
    }

    const double radius = stroke.brush.width * maxPressure / 2.0 + rasterPadding;
    return RasterBounds{stroke.page, x0 - radius, y0 - radius, x1 + radius, y1 + radius};
}

std::optional<RasterBounds> alignedBounds(const RasterBounds& bounds, double scale) {
    RasterBounds aligned = bounds;
    aligned.x0 = std::floor(bounds.x0 * scale) / scale;
    aligned.y0 = std::floor(bounds.y0 * scale) / scale;
    aligned.x1 = std::ceil(bounds.x1 * scale) / scale;
    aligned.y1 = std::ceil(bounds.y1 * scale) / scale;
    const double width = (aligned.x1 - aligned.x0) * scale;
    const double height = (aligned.y1 - aligned.y0) * scale;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 || width > maxRasterDimension ||
        height > maxRasterDimension || width * height * 4 > maxCachedBytes) {
        return std::nullopt;
    }
    return aligned;
}

std::optional<double> scaleThatFits(const RasterBounds& bounds, double requestedScale) {
    double scale = std::clamp(requestedScale, minRasterScale, maxRasterScale);
    while (!alignedBounds(bounds, scale)) {
        if (scale <= minRasterScale) {
            return std::nullopt;
        }
        scale = std::max(minRasterScale, scale / 2.0);
    }
    return scale;
}

void initializeSurface(cairo_t* cr, bool highlighter) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    if (highlighter) {
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

void drawStroke(cairo_t* cr, const Ink& stroke, bool highlighter) {
    if (highlighter) {
        // A white-backed multiply image stores the exact per-pixel blend factor.
        drawInk(cr, stroke);
    } else {
        drawInkSource(cr, stroke);
    }
}

std::optional<CachedInkRun> renderRun(const std::vector<const Ink*>& strokes, const std::vector<uint64_t>& strokeKeys,
                                      const RasterBounds& bounds, double scale, bool highlighter) {
    const auto aligned = alignedBounds(bounds, scale);
    if (!aligned) {
        return std::nullopt;
    }
    const int width = static_cast<int>(std::ceil((aligned->x1 - aligned->x0) * scale));
    const int height = static_cast<int>(std::ceil((aligned->y1 - aligned->y0) * scale));

    CachedInkRun raster;
    raster.surface.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
    if (!raster.surface || cairo_surface_status(raster.surface.get()) != CAIRO_STATUS_SUCCESS) {
        return std::nullopt;
    }

    cairo_t* cr = cairo_create(raster.surface.get());
    initializeSurface(cr, highlighter);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -aligned->x0, -aligned->y0);
    for (const auto* stroke: strokes) {
        drawStroke(cr, *stroke, highlighter);
    }
    const auto status = cairo_status(cr);
    cairo_destroy(cr);
    if (status != CAIRO_STATUS_SUCCESS) {
        return std::nullopt;
    }

    cairo_surface_flush(raster.surface.get());
    raster.strokeKeys = strokeKeys;
    raster.x = aligned->x0;
    raster.y = aligned->y0;
    raster.scale = scale;
    raster.page = bounds.page;
    raster.highlighter = highlighter;
    raster.bytes = static_cast<size_t>(cairo_image_surface_get_stride(raster.surface.get())) *
                   static_cast<size_t>(cairo_image_surface_get_height(raster.surface.get()));
    return raster;
}

class InkRasterCache {
public:
    bool draw(cairo_t* cr, const std::vector<const Ink*>& strokes, double requestedScale) {
        if (strokes.empty() || !std::isfinite(requestedScale) ||
            std::any_of(strokes.begin(), strokes.end(), [](const Ink* stroke) { return !stroke; })) {
            return false;
        }

        const bool highlighter = strokes.front()->brush.tool == Tool::Highlighter;
        const int page = strokes.front()->page;
        std::vector<uint64_t> keys;
        keys.reserve(strokes.size());
        for (const auto* stroke: strokes) {
            if (stroke->page != page || (stroke->brush.tool == Tool::Highlighter) != highlighter) {
                return false;
            }
            keys.push_back(cacheKey(*stroke));
        }

        const uint64_t key = keys.front();
        auto existing = rasters.find(key);
        const double requested = std::clamp(requestedScale, minRasterScale, maxRasterScale);
        if (existing != rasters.end() && existing->second.page == page && existing->second.highlighter == highlighter &&
            existing->second.scale >= requested && existing->second.strokeKeys == keys) {
            existing->second.stamp = ++nextStamp;
            blit(cr, existing->second);
            return true;
        }

        std::optional<CachedInkRun> updated;
        if (existing != rasters.end() && existing->second.page == page && existing->second.highlighter == highlighter &&
            existing->second.scale >= requested && isPrefix(existing->second.strokeKeys, keys)) {
            auto bounds = cachedBounds(existing->second);
            for (size_t i = existing->second.strokeKeys.size(); i < strokes.size(); ++i) {
                const auto strokeBounds = rasterBounds(*strokes[i]);
                if (!strokeBounds) {
                    return false;
                }
                bounds.x0 = std::min(bounds.x0, strokeBounds->x0);
                bounds.y0 = std::min(bounds.y0, strokeBounds->y0);
                bounds.x1 = std::max(bounds.x1, strokeBounds->x1);
                bounds.y1 = std::max(bounds.y1, strokeBounds->y1);
            }
            const auto scale = scaleThatFits(bounds, existing->second.scale);
            if (!scale) {
                return false;
            }
            if (*scale < existing->second.scale) {
                updated = renderRun(strokes, keys, bounds, *scale, highlighter);
            } else {
                updated = extendRun(existing->second, strokes, keys, bounds);
            }
        } else {
            const auto bounds = runBounds(strokes, page);
            if (!bounds) {
                return false;
            }
            const auto scale = scaleThatFits(*bounds, requested);
            if (!scale) {
                return false;
            }
            updated = renderRun(strokes, keys, *bounds, *scale, highlighter);
        }
        if (!updated || updated->bytes > maxCachedBytes) {
            return false;
        }

        const size_t replacedBytes = existing == rasters.end() ? 0 : existing->second.bytes;
        if (!evictUntilFits(updated->bytes, key, replacedBytes)) {
            return false;
        }
        if (existing != rasters.end()) {
            cachedBytes -= existing->second.bytes;
            rasters.erase(existing);
        }
        updated->stamp = ++nextStamp;
        cachedBytes += updated->bytes;
        auto inserted = rasters.emplace(key, std::move(*updated)).first;
        blit(cr, inserted->second);
        return true;
    }

    void setVisiblePages(const std::vector<int>& pages) { visiblePages.assign(pages.begin(), pages.end()); }

private:
    static uint64_t cacheKey(const Ink& stroke) {
        static std::atomic_uint64_t nextKey{1};
        if (stroke.rasterCacheKey == 0) {
            stroke.rasterCacheKey = nextKey.fetch_add(1, std::memory_order_relaxed);
            if (stroke.rasterCacheKey == 0) {
                stroke.rasterCacheKey = nextKey.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return stroke.rasterCacheKey;
    }

    static bool isPrefix(const std::vector<uint64_t>& prefix, const std::vector<uint64_t>& values) {
        return prefix.size() < values.size() && std::equal(prefix.begin(), prefix.end(), values.begin());
    }

    static std::optional<RasterBounds> runBounds(const std::vector<const Ink*>& strokes, int page) {
        RasterBounds bounds{};
        bool hasBounds = false;
        for (const auto* stroke: strokes) {
            const auto strokeBounds = rasterBounds(*stroke);
            if (!strokeBounds) {
                return std::nullopt;
            }
            if (!hasBounds) {
                bounds = *strokeBounds;
                hasBounds = true;
            } else {
                bounds.x0 = std::min(bounds.x0, strokeBounds->x0);
                bounds.y0 = std::min(bounds.y0, strokeBounds->y0);
                bounds.x1 = std::max(bounds.x1, strokeBounds->x1);
                bounds.y1 = std::max(bounds.y1, strokeBounds->y1);
            }
        }
        if (!hasBounds) {
            return std::nullopt;
        }
        bounds.page = page;
        return bounds;
    }

    static RasterBounds cachedBounds(const CachedInkRun& raster) {
        return {raster.page, raster.x, raster.y,
                raster.x + cairo_image_surface_get_width(raster.surface.get()) / raster.scale,
                raster.y + cairo_image_surface_get_height(raster.surface.get()) / raster.scale};
    }

    static std::optional<CachedInkRun> extendRun(const CachedInkRun& previous, const std::vector<const Ink*>& strokes,
                                                 const std::vector<uint64_t>& keys, const RasterBounds& bounds) {
        const auto aligned = alignedBounds(bounds, previous.scale);
        if (!aligned) {
            return std::nullopt;
        }
        const int width = static_cast<int>(std::ceil((aligned->x1 - aligned->x0) * previous.scale));
        const int height = static_cast<int>(std::ceil((aligned->y1 - aligned->y0) * previous.scale));
        CachedInkRun updated;
        updated.surface.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (!updated.surface || cairo_surface_status(updated.surface.get()) != CAIRO_STATUS_SUCCESS) {
            return std::nullopt;
        }

        cairo_t* cr = cairo_create(updated.surface.get());
        initializeSurface(cr, previous.highlighter);
        const double offsetX = (previous.x - aligned->x0) * previous.scale;
        const double offsetY = (previous.y - aligned->y0) * previous.scale;
        cairo_rectangle(cr, offsetX, offsetY, cairo_image_surface_get_width(previous.surface.get()),
                        cairo_image_surface_get_height(previous.surface.get()));
        cairo_clip(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(cr, previous.surface.get(), offsetX, offsetY);
        cairo_paint(cr);
        cairo_reset_clip(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_scale(cr, previous.scale, previous.scale);
        cairo_translate(cr, -aligned->x0, -aligned->y0);
        for (size_t i = previous.strokeKeys.size(); i < strokes.size(); ++i) {
            drawStroke(cr, *strokes[i], previous.highlighter);
        }
        const auto status = cairo_status(cr);
        cairo_destroy(cr);
        if (status != CAIRO_STATUS_SUCCESS) {
            return std::nullopt;
        }

        cairo_surface_flush(updated.surface.get());
        updated.strokeKeys = keys;
        updated.x = aligned->x0;
        updated.y = aligned->y0;
        updated.scale = previous.scale;
        updated.page = previous.page;
        updated.highlighter = previous.highlighter;
        updated.bytes = static_cast<size_t>(cairo_image_surface_get_stride(updated.surface.get())) *
                        static_cast<size_t>(cairo_image_surface_get_height(updated.surface.get()));
        return updated;
    }

    static void blit(cairo_t* cr, const CachedInkRun& raster) {
        cairo_save(cr);
        cairo_translate(cr, raster.x, raster.y);
        cairo_scale(cr, 1.0 / raster.scale, 1.0 / raster.scale);
        cairo_set_source_surface(cr, raster.surface.get(), 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_set_operator(cr, raster.highlighter ? CAIRO_OPERATOR_MULTIPLY : CAIRO_OPERATOR_OVER);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    bool evictUntilFits(size_t bytes, uint64_t replacingKey, size_t replacedBytes) {
        while (cachedBytes - replacedBytes + bytes > maxCachedBytes && !rasters.empty()) {
            auto oldest = rasters.end();
            for (auto it = rasters.begin(); it != rasters.end(); ++it) {
                if (it->first == replacingKey ||
                    std::find(visiblePages.begin(), visiblePages.end(), it->second.page) != visiblePages.end()) {
                    continue;
                }
                if (oldest == rasters.end() || it->second.stamp < oldest->second.stamp) {
                    oldest = it;
                }
            }
            if (oldest == rasters.end()) {
                return false;
            }
            cachedBytes -= oldest->second.bytes;
            rasters.erase(oldest);
        }
        return true;
    }

    std::map<uint64_t, CachedInkRun> rasters;
    std::vector<int> visiblePages;
    size_t cachedBytes = 0;
    uint64_t nextStamp = 0;
};

InkRasterCache& inkRasterCache() {
    static InkRasterCache cache;
    return cache;
}

}  // namespace

bool drawCachedInkRun(cairo_t* cr, const std::vector<const Ink*>& strokes, double requestedScale) {
    return inkRasterCache().draw(cr, strokes, requestedScale);
}

void setInkRasterVisiblePages(const std::vector<int>& pages) { inkRasterCache().setVisiblePages(pages); }

}  // namespace prototype
