#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cairo.h>
#include <gtk/gtk.h>

#include "CanvasState.h"

namespace prototype {

inline constexpr int maxPageThumbnailWidth = 144;
inline constexpr int maxPageThumbnailHeight = 112;

struct PageRasterSurfaceDeleter {
    void operator()(cairo_surface_t* surface) const;
};

struct PageRasterLayerSource {
    size_t origin = 0;
    std::string path;
};

enum class PageRasterQuality { Thumbnail, Full, MediumDetail, LowDetail };

struct PageRasterRequest {
    int page = -1;
    uint64_t generation = 0;
    uint64_t requestId = 0;
    double priority = std::numeric_limits<double>::infinity();
    double renderScale = 0;
    double mediumDetailScale = 0;
    double lowDetailScale = 0;
    PageRasterQuality quality = PageRasterQuality::Thumbnail;
    bool audioMode = false;
    PageSize pageSize;
    std::string backgroundPath;
    std::vector<PageRasterLayerSource> layers;
};

struct PageRaster {
    double scale = 1;
    bool audioMode = false;
    uint64_t stamp = 0;
    std::unique_ptr<cairo_surface_t, PageRasterSurfaceDeleter> background;
    std::map<size_t, std::unique_ptr<cairo_surface_t, PageRasterSurfaceDeleter>> layers;
};

namespace detail {
struct PageRasterRendererState;
}

class PageRasterRenderer {
public:
    explicit PageRasterRenderer(std::string workerExecutable, size_t maxCachedPages = 16, size_t maxConcurrentTasks = 2,
                                size_t maxQueuedRequests = 12);
    ~PageRasterRenderer();

    void setRedrawWidget(GtkWidget* widget);
    void setCacheLimits(size_t fullPages, size_t mediumPages, size_t mediumBytes, size_t lowPages, size_t lowBytes);
    void bindThumbnailWidget(int page, GtkWidget* widget);
    void unbindThumbnailWidget(int page, GtkWidget* widget);
    void invalidate();
    void setVisiblePages(std::map<int, double> visiblePages, int selectedPage);
    void setFullDetailPages(std::map<int, double> fullDetailPages);
    void setRetainedPages(std::map<int, double> mediumRetainedPages, std::map<int, double> lowRetainedPages);
    void request(PageRasterRequest request);
    const PageRaster* find(int page);
    bool matches(const PageRasterRequest& request) const;
    bool empty() const;
    size_t cacheBytes() const;
    size_t fullDetailCachePages() const;
    size_t mediumDetailCacheBytes() const;
    size_t mediumDetailCachePages() const;
    size_t lowDetailCacheBytes() const;
    size_t lowDetailCachePages() const;
    size_t completedRequests() const;
    size_t completedFullRequests() const;
    size_t completedMediumDetailRequests() const;
    size_t completedLowDetailRequests() const;
    size_t pendingCount() const;
    size_t activeCount() const;
    size_t offscreenCancellations() const;
    bool hasPending(int page) const;
    bool hasFailed(int page, PageRasterQuality quality = PageRasterQuality::Thumbnail) const;
    int nextQueuedPage() const;
    int preferredVisiblePage() const;
    int firstDispatchedPage() const;

private:
    std::shared_ptr<detail::PageRasterRendererState> state;
};

int runPageRasterWorker(int argc, char** argv);

}  // namespace prototype
