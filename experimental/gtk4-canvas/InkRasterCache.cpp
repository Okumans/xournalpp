#include "InkRasterCache.h"

#include <algorithm>
#include <array>
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
constexpr size_t maxLiveRasterBytes = 16 * 1024 * 1024;
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
    uint64_t usedFrame = 0;
};

struct LiveInkRaster {
    std::unique_ptr<cairo_surface_t, SurfaceDeleter> surface;
    std::array<double, 3> color{};
    std::vector<double> lineDashes;
    double width = 0;
    double x = 0;
    double y = 0;
    double scale = 1;
    double dashOffset = 0;
    Point lastPoint{};
    uint64_t strokeKey = 0;
    size_t pointCount = 0;
    int page = -1;
    Tool tool = Tool::Pen;
    bool pressureSensitive = false;
    size_t layerId = 0;
    size_t originLayer = 0;
    size_t bytes = 0;
    int widthPixels = 0;
    int heightPixels = 0;
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

}  // namespace

struct InkRasterCache::Impl {
    void beginFrame(const std::vector<int>& pages) {
        if (++frame == 0) {
            frame = 1;
            for (auto& [key, raster]: rasters) {
                raster.usedFrame = 0;
            }
        }
        visiblePages = pages;
    }

    void endFrame() {
        std::erase_if(rasters, [this](const auto& entry) {
            const auto& raster = entry.second;
            if (raster.usedFrame == frame ||
                std::find(visiblePages.begin(), visiblePages.end(), raster.page) == visiblePages.end()) {
                return false;
            }
            cachedBytes -= raster.bytes;
            return true;
        });
        visiblePages.clear();
    }

    void clear() {
        rasters.clear();
        cachedBytes = 0;
        clearActive();
        visiblePages.clear();
        frame = 0;
        nextStamp = 0;
        counters = {};
    }

    void clearActive() {
        liveRaster.reset();
        liveBytes = 0;
    }

    InkRasterCache::Stats stats() const {
        auto result = counters;
        result.cachedBytes = cachedBytes;
        result.liveBytes = liveBytes;
        return result;
    }

    bool drawCommitted(cairo_t* cr, const std::vector<const Ink*>& strokes, double requestedScale,
                       const cairo_matrix_t* transform) {
        if (strokes.empty() || !std::isfinite(requestedScale) ||
            std::any_of(strokes.begin(), strokes.end(), [](const Ink* stroke) { return !stroke; })) {
            ++counters.fallbacks;
            return false;
        }

        const bool highlighter = strokes.front()->brush.tool == Tool::Highlighter;
        const int page = strokes.front()->page;
        std::vector<uint64_t> keys;
        keys.reserve(strokes.size());
        for (const auto* stroke: strokes) {
            if (stroke->page != page || (stroke->brush.tool == Tool::Highlighter) != highlighter) {
                ++counters.fallbacks;
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
            existing->second.usedFrame = frame;
            ++counters.hits;
            blit(cr, existing->second, transform);
            return true;
        }

        std::optional<CachedInkRun> updated;
        bool appended = false;
        if (existing != rasters.end() && existing->second.page == page && existing->second.highlighter == highlighter &&
            existing->second.scale >= requested && isPrefix(existing->second.strokeKeys, keys)) {
            auto bounds = cachedBounds(existing->second);
            for (size_t i = existing->second.strokeKeys.size(); i < strokes.size(); ++i) {
                const auto strokeBounds = rasterBounds(*strokes[i]);
                if (!strokeBounds) {
                    ++counters.fallbacks;
                    return false;
                }
                bounds.x0 = std::min(bounds.x0, strokeBounds->x0);
                bounds.y0 = std::min(bounds.y0, strokeBounds->y0);
                bounds.x1 = std::max(bounds.x1, strokeBounds->x1);
                bounds.y1 = std::max(bounds.y1, strokeBounds->y1);
            }
            const auto scale = scaleThatFits(bounds, existing->second.scale);
            if (!scale) {
                ++counters.fallbacks;
                return false;
            }
            const auto aligned = alignedBounds(bounds, *scale);
            const bool sameBounds = aligned && *scale == existing->second.scale && aligned->x0 == existing->second.x &&
                                    aligned->y0 == existing->second.y &&
                                    static_cast<int>(std::ceil((aligned->x1 - aligned->x0) * *scale)) ==
                                            cairo_image_surface_get_width(existing->second.surface.get()) &&
                                    static_cast<int>(std::ceil((aligned->y1 - aligned->y0) * *scale)) ==
                                            cairo_image_surface_get_height(existing->second.surface.get());
            if (sameBounds) {
                if (appendRunInPlace(existing->second, strokes, keys)) {
                    existing->second.stamp = ++nextStamp;
                    existing->second.usedFrame = frame;
                    ++counters.extensions;
                    blit(cr, existing->second, transform);
                    return true;
                }
                // appendRunInPlace may have partially changed the surface before Cairo reported an error.
                // Rebuild from source instead of extending that possibly damaged raster.
                updated = renderRun(strokes, keys, bounds, *scale, highlighter);
            } else if (*scale < existing->second.scale) {
                updated = renderRun(strokes, keys, bounds, *scale, highlighter);
            } else {
                updated = extendRun(existing->second, strokes, keys, bounds);
                if (updated) {
                    counters.bytesCopied += existing->second.bytes;
                }
            }
            appended = true;
        } else {
            const auto bounds = runBounds(strokes, page);
            if (!bounds) {
                ++counters.fallbacks;
                return false;
            }
            const auto scale = scaleThatFits(*bounds, requested);
            if (!scale) {
                ++counters.fallbacks;
                return false;
            }
            updated = renderRun(strokes, keys, *bounds, *scale, highlighter);
        }
        if (!updated || updated->bytes > maxCachedBytes) {
            ++counters.fallbacks;
            return false;
        }

        const size_t replacedBytes = existing == rasters.end() ? 0 : existing->second.bytes;
        if (!evictUntilFits(updated->bytes, key, replacedBytes)) {
            ++counters.fallbacks;
            return false;
        }
        if (existing != rasters.end()) {
            cachedBytes -= existing->second.bytes;
            rasters.erase(existing);
        }
        updated->stamp = ++nextStamp;
        updated->usedFrame = frame;
        cachedBytes += updated->bytes;
        auto inserted = rasters.emplace(key, std::move(*updated)).first;
        if (appended) {
            ++counters.extensions;
        } else {
            ++counters.renders;
        }
        blit(cr, inserted->second, transform);
        return true;
    }

    bool drawActive(cairo_t* cr, const Ink& stroke, double requestedScale, double visibleX0, double visibleY0,
                    double visibleX1, double visibleY1, bool appendOnly) {
        const bool cacheableTool = stroke.brush.tool == Tool::Pen || stroke.brush.tool == Tool::Highlighter ||
                                   stroke.brush.tool == Tool::Eraser;
        if (!cr || !cacheableTool || stroke.points.empty() || stroke.fill >= 0 || stroke.capStyle != 0 ||
            !std::isfinite(requestedScale) || !std::isfinite(visibleX0) || !std::isfinite(visibleY0) ||
            !std::isfinite(visibleX1) || !std::isfinite(visibleY1) || visibleX1 <= visibleX0 ||
            visibleY1 <= visibleY0) {
            ++counters.fallbacks;
            clearActive();
            return false;
        }

        const double scale = std::clamp(requestedScale, minRasterScale, maxRasterScale);
        RasterBounds visible{stroke.page, visibleX0 - rasterPadding, visibleY0 - rasterPadding,
                             visibleX1 + rasterPadding, visibleY1 + rasterPadding};
        const auto aligned = alignedBounds(visible, scale);
        if (!aligned) {
            ++counters.fallbacks;
            clearActive();
            return false;
        }
        const int width = static_cast<int>(std::ceil((aligned->x1 - aligned->x0) * scale));
        const int height = static_cast<int>(std::ceil((aligned->y1 - aligned->y0) * scale));
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        if (bytes > maxLiveRasterBytes) {
            ++counters.fallbacks;
            clearActive();
            return false;
        }

        const uint64_t key = cacheKey(stroke);
        const bool sameRaster = liveRaster && liveRaster->strokeKey == key && liveRaster->page == stroke.page &&
                                liveRaster->scale == scale && liveRaster->x == aligned->x0 &&
                                liveRaster->y == aligned->y0 && liveRaster->widthPixels == width &&
                                liveRaster->heightPixels == height && sameStyle(*liveRaster, stroke);
        const bool extends = appendOnly && sameRaster && liveRaster->pointCount > 0 &&
                             stroke.points.size() >= liveRaster->pointCount &&
                             stroke.points[liveRaster->pointCount - 1] == liveRaster->lastPoint;

        if (extends && stroke.points.size() == liveRaster->pointCount) {
            blitActive(cr);
            return true;
        }

        if (extends) {
            if (!appendActive(stroke)) {
                clearActive();
                ++counters.fallbacks;
                return false;
            }
            ++counters.liveAppends;
            blitActive(cr);
            return true;
        }

        clearActive();
        if (!evictUntilFits(bytes, 0, 0)) {
            ++counters.fallbacks;
            return false;
        }
        auto raster = createLiveRaster(stroke, key, *aligned, scale, width, height);
        if (!raster) {
            ++counters.fallbacks;
            return false;
        }
        liveRaster = std::move(raster);
        liveBytes = liveRaster->bytes;
        ++counters.liveRebuilds;
        blitActive(cr);
        return true;
    }

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

    static bool sameStyle(const LiveInkRaster& raster, const Ink& stroke) {
        return raster.color == stroke.brush.color && raster.width == stroke.brush.width &&
               raster.lineDashes == stroke.brush.lineDashes && raster.tool == stroke.brush.tool &&
               raster.pressureSensitive == stroke.pressureSensitive && raster.layerId == stroke.layerId &&
               raster.originLayer == stroke.originLayer;
    }

    static std::optional<LiveInkRaster> createLiveRaster(const Ink& stroke, uint64_t key, const RasterBounds& bounds,
                                                         double scale, int width, int height) {
        LiveInkRaster raster;
        raster.surface.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (!raster.surface || cairo_surface_status(raster.surface.get()) != CAIRO_STATUS_SUCCESS) {
            return std::nullopt;
        }
        cairo_t* cr = cairo_create(raster.surface.get());
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_scale(cr, scale, scale);
        cairo_translate(cr, -bounds.x0, -bounds.y0);
        drawInkSource(cr, stroke);
        const auto status = cairo_status(cr);
        cairo_destroy(cr);
        if (status != CAIRO_STATUS_SUCCESS) {
            return std::nullopt;
        }
        cairo_surface_flush(raster.surface.get());
        raster.color = stroke.brush.color;
        raster.lineDashes = stroke.brush.lineDashes;
        raster.width = stroke.brush.width;
        raster.x = bounds.x0;
        raster.y = bounds.y0;
        raster.scale = scale;
        raster.dashOffset = pathLength(stroke.points);
        raster.strokeKey = key;
        raster.pointCount = stroke.points.size();
        raster.lastPoint = stroke.points.back();
        raster.page = stroke.page;
        raster.tool = stroke.brush.tool;
        raster.pressureSensitive = stroke.pressureSensitive;
        raster.layerId = stroke.layerId;
        raster.originLayer = stroke.originLayer;
        raster.bytes = static_cast<size_t>(cairo_image_surface_get_stride(raster.surface.get())) *
                       static_cast<size_t>(cairo_image_surface_get_height(raster.surface.get()));
        raster.widthPixels = width;
        raster.heightPixels = height;
        return raster;
    }

    static double pathLength(const std::vector<Point>& points) {
        double length = 0;
        for (size_t i = 1; i < points.size(); ++i) {
            length += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
        }
        return length;
    }

    bool appendActive(const Ink& stroke) {
        const size_t previousPointCount = liveRaster->pointCount;
        Ink segment{stroke.page,        {},
                    stroke.brush,       stroke.pressureSensitive,
                    stroke.layerOrder,  stroke.layerId,
                    stroke.originLayer, stroke.fill,
                    stroke.capStyle,    stroke.pluginReference};
        segment.points.assign(stroke.points.begin() + static_cast<std::ptrdiff_t>(previousPointCount - 1),
                              stroke.points.end());
        cairo_t* cr = cairo_create(liveRaster->surface.get());
        cairo_scale(cr, liveRaster->scale, liveRaster->scale);
        cairo_translate(cr, -liveRaster->x, -liveRaster->y);
        drawInkSource(cr, segment, {}, liveRaster->dashOffset);
        if (cairo_status(cr) == CAIRO_STATUS_SUCCESS) {
            repairActiveJoin(cr, stroke, previousPointCount);
        }
        const auto status = cairo_status(cr);
        cairo_destroy(cr);
        if (status != CAIRO_STATUS_SUCCESS) {
            return false;
        }
        liveRaster->dashOffset += pathLength(segment.points);
        liveRaster->pointCount = stroke.points.size();
        liveRaster->lastPoint = stroke.points.back();
        cairo_surface_flush(liveRaster->surface.get());
        liveRaster->bytes = static_cast<size_t>(cairo_image_surface_get_stride(liveRaster->surface.get())) *
                            static_cast<size_t>(cairo_image_surface_get_height(liveRaster->surface.get()));
        liveBytes = liveRaster->bytes;
        return true;
    }

    void repairActiveJoin(cairo_t* cr, const Ink& stroke, size_t previousPointCount) const {
        const Point& join = stroke.points[previousPointCount - 1];
        double maxPressure = 1.0;
        if (stroke.pressureSensitive && stroke.brush.tool != Tool::Highlighter && stroke.brush.tool != Tool::Eraser) {
            const size_t first = previousPointCount > 1 ? previousPointCount - 2 : 0;
            const size_t last = std::min(stroke.points.size(), previousPointCount + 1);
            for (size_t i = first; i < last; ++i) {
                maxPressure = std::max(maxPressure, stroke.points[i].pressure);
            }
        }
        const double radius = stroke.brush.width * maxPressure / 2.0 + 2.0 / liveRaster->scale;
        const double x0 =
                liveRaster->x + std::floor((join.x - radius - liveRaster->x) * liveRaster->scale) / liveRaster->scale;
        const double y0 =
                liveRaster->y + std::floor((join.y - radius - liveRaster->y) * liveRaster->scale) / liveRaster->scale;
        const double x1 =
                liveRaster->x + std::ceil((join.x + radius - liveRaster->x) * liveRaster->scale) / liveRaster->scale;
        const double y1 =
                liveRaster->y + std::ceil((join.y + radius - liveRaster->y) * liveRaster->scale) / liveRaster->scale;

        cairo_save(cr);
        cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
        cairo_clip(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        drawInkSource(cr, stroke);
        cairo_restore(cr);
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

    static bool appendRunInPlace(CachedInkRun& raster, const std::vector<const Ink*>& strokes,
                                 const std::vector<uint64_t>& keys) {
        cairo_t* cr = cairo_create(raster.surface.get());
        cairo_scale(cr, raster.scale, raster.scale);
        cairo_translate(cr, -raster.x, -raster.y);
        for (size_t i = raster.strokeKeys.size(); i < strokes.size(); ++i) {
            drawStroke(cr, *strokes[i], raster.highlighter);
        }
        const auto status = cairo_status(cr);
        cairo_destroy(cr);
        if (status != CAIRO_STATUS_SUCCESS) {
            return false;
        }
        cairo_surface_flush(raster.surface.get());
        raster.strokeKeys = keys;
        return true;
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

    static void blit(cairo_t* cr, const CachedInkRun& raster, const cairo_matrix_t* transform = nullptr) {
        cairo_save(cr);
        if (transform) {
            cairo_transform(cr, transform);
        }
        cairo_translate(cr, raster.x, raster.y);
        cairo_scale(cr, 1.0 / raster.scale, 1.0 / raster.scale);
        cairo_set_source_surface(cr, raster.surface.get(), 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_set_operator(cr, raster.highlighter ? CAIRO_OPERATOR_MULTIPLY : CAIRO_OPERATOR_OVER);
        cairo_paint(cr);
        cairo_restore(cr);
    }

    void blitActive(cairo_t* cr) const {
        cairo_save(cr);
        cairo_translate(cr, liveRaster->x, liveRaster->y);
        cairo_scale(cr, 1.0 / liveRaster->scale, 1.0 / liveRaster->scale);
        cairo_set_source_surface(cr, liveRaster->surface.get(), 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        if (liveRaster->tool == Tool::Highlighter) {
            cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
            cairo_paint_with_alpha(cr, behavior::highlightOpacity);
        } else {
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_paint(cr);
        }
        cairo_restore(cr);
    }

    bool evictUntilFits(size_t bytes, uint64_t replacingKey, size_t replacedBytes) {
        if (replacedBytes > cachedBytes + liveBytes) {
            return false;
        }
        while (cachedBytes + liveBytes - replacedBytes + bytes > maxCachedBytes && !rasters.empty()) {
            auto oldest = rasters.end();
            for (auto it = rasters.begin(); it != rasters.end(); ++it) {
                if (it->first == replacingKey || it->second.usedFrame == frame) {
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
        return cachedBytes + liveBytes - replacedBytes + bytes <= maxCachedBytes;
    }

    std::map<uint64_t, CachedInkRun> rasters;
    std::vector<int> visiblePages;
    std::optional<LiveInkRaster> liveRaster;
    size_t cachedBytes = 0;
    size_t liveBytes = 0;
    uint64_t frame = 0;
    uint64_t nextStamp = 0;
    InkRasterCache::Stats counters;
};

InkRasterCache::InkRasterCache(): impl(std::make_unique<Impl>()) {}
InkRasterCache::~InkRasterCache() = default;

void InkRasterCache::beginFrame(const std::vector<int>& visiblePages) { impl->beginFrame(visiblePages); }
void InkRasterCache::endFrame() { impl->endFrame(); }
void InkRasterCache::clear() { impl->clear(); }
void InkRasterCache::clearActive() { impl->clearActive(); }
bool InkRasterCache::drawCommitted(cairo_t* cr, const std::vector<const Ink*>& strokes, double requestedScale,
                                   const cairo_matrix_t* transform) {
    return impl->drawCommitted(cr, strokes, requestedScale, transform);
}
bool InkRasterCache::drawActive(cairo_t* cr, const Ink& stroke, double requestedScale, double visibleX0,
                                double visibleY0, double visibleX1, double visibleY1, bool appendOnly) {
    return impl->drawActive(cr, stroke, requestedScale, visibleX0, visibleY0, visibleX1, visibleY1, appendOnly);
}
InkRasterCache::Stats InkRasterCache::stats() const { return impl->stats(); }

}  // namespace prototype
