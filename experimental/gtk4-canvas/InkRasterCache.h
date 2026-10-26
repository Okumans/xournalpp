#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <cairo.h>

namespace prototype {

struct Ink;

class InkRasterCache {
public:
    struct Stats {
        uint64_t hits = 0;
        uint64_t renders = 0;
        uint64_t extensions = 0;
        uint64_t liveRebuilds = 0;
        uint64_t liveAppends = 0;
        uint64_t fallbacks = 0;
        size_t bytesCopied = 0;
        size_t cachedBytes = 0;
        size_t liveBytes = 0;
    };

    InkRasterCache();
    ~InkRasterCache();

    InkRasterCache(const InkRasterCache&) = delete;
    InkRasterCache& operator=(const InkRasterCache&) = delete;

    void beginFrame(const std::vector<int>& visiblePages);
    void endFrame();
    void clear();
    void clearActive();

    bool drawCommitted(cairo_t* cr, const std::vector<const Ink*>& strokes, double requestedScale,
                       const cairo_matrix_t* transform = nullptr);
    // Set appendOnly only when the existing point prefix cannot change during this stroke.
    bool drawActive(cairo_t* cr, const Ink& stroke, double requestedScale, double visibleX0, double visibleY0,
                    double visibleX1, double visibleY1, bool appendOnly);

    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace prototype
