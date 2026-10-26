#include "PageRasterRenderer.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <gio/gio.h>
#include <poppler.h>

namespace prototype {

void PageRasterSurfaceDeleter::operator()(cairo_surface_t* surface) const { cairo_surface_destroy(surface); }

namespace detail {
constexpr size_t defaultMaxCachedCanvasMediumLodPages = 12;
constexpr size_t defaultMaxCachedCanvasLodPages = 128;
constexpr size_t defaultCanvasMediumLodCacheBytes = 32 * 1024 * 1024;
constexpr size_t defaultCanvasLodCacheBytes = 32 * 1024 * 1024;

size_t rasterBytes(const PageRaster& raster) {
    size_t bytes = 0;
    const auto addSurface = [&bytes](cairo_surface_t* surface) {
        if (surface) {
            bytes += static_cast<size_t>(cairo_image_surface_get_stride(surface)) *
                     static_cast<size_t>(cairo_image_surface_get_height(surface));
        }
    };
    addSurface(raster.background.get());
    for (const auto& layer: raster.layers) {
        addSurface(layer.second.get());
    }
    return bytes;
}

size_t rasterCacheBytes(const std::map<int, PageRaster>& cache) {
    size_t bytes = 0;
    for (const auto& [page, raster]: cache) {
        bytes += rasterBytes(raster);
    }
    return bytes;
}

struct PageRasterTask;

struct PreviewWidgetReference {
    PreviewWidgetReference() { g_weak_ref_init(&reference, nullptr); }
    ~PreviewWidgetReference() { g_weak_ref_clear(&reference); }

    GWeakRef reference;
};

struct PageRasterRendererState {
    explicit PageRasterRendererState(std::string workerExecutable, size_t maxCachedPages, size_t maxConcurrentTasks,
                                     size_t maxQueuedRequests):
            workerExecutable(std::move(workerExecutable)),
            maxCachedPages(maxCachedPages),
            maxConcurrentTasks(maxConcurrentTasks),
            maxQueuedRequests(maxQueuedRequests) {
        g_weak_ref_init(&redrawWidget, nullptr);
    }
    ~PageRasterRendererState() { g_weak_ref_clear(&redrawWidget); }

    GWeakRef redrawWidget;
    std::string workerExecutable;
    size_t maxCachedPages;
    size_t maxConcurrentTasks;
    size_t maxQueuedRequests;
    size_t maxCachedMediumDetailPages = defaultMaxCachedCanvasMediumLodPages;
    size_t maxCachedLowDetailPages = defaultMaxCachedCanvasLodPages;
    size_t maxMediumDetailCacheBytes = defaultCanvasMediumLodCacheBytes;
    size_t maxLowDetailCacheBytes = defaultCanvasLodCacheBytes;
    std::map<int, PageRaster> previews;
    std::map<int, PageRaster> mediumDetailPreviews;
    std::map<int, PageRaster> lowDetailPreviews;
    std::map<int, uint64_t> pending;
    std::map<std::pair<int, PageRasterQuality>, uint64_t> failed;
    std::map<int, double> visiblePages;
    std::map<int, double> fullDetailPages;
    std::map<int, double> mediumRetainedPages;
    std::map<int, double> lowRetainedPages;
    std::map<int, std::unique_ptr<PreviewWidgetReference>> boundThumbnails;
    std::deque<PageRasterRequest> queue;
    std::vector<std::shared_ptr<PageRasterTask>> activeTasks;
    std::vector<int> lastDispatchOrder;
    size_t offscreenCancellations = 0;
    uint64_t generation = 0;
    uint64_t requestSequence = 0;
    uint64_t stamp = 0;
    size_t completedRequests = 0;
    size_t completedFullRequests = 0;
    size_t completedMediumDetailRequests = 0;
    size_t completedLowDetailRequests = 0;
    int selectedPage = -1;
    guint dispatchSource = 0;
    bool stopping = false;
};

struct PageRasterTask {
    PageRasterRequest request;
    PageRaster preview;
    PageRaster mediumDetailPreview;
    PageRaster lowDetailPreview;
    std::string outputDirectory;
    std::string renderError;
    GSubprocess* process = nullptr;
    bool cancelled = false;
};

struct PageRasterCompletion {
    std::shared_ptr<PageRasterRendererState> state;
    std::shared_ptr<PageRasterTask> task;
};

void schedulePageRasterTasks(const std::shared_ptr<PageRasterRendererState>& state);
void startPageRasterTasks(const std::shared_ptr<PageRasterRendererState>& state);

std::string_view rasterPrefix(PageRasterQuality quality) {
    switch (quality) {
        case PageRasterQuality::MediumDetail:
            return "medium-";
        case PageRasterQuality::LowDetail:
            return "lod-";
        default:
            return "";
    }
}

int rasterQualityRank(PageRasterQuality quality) {
    switch (quality) {
        case PageRasterQuality::Full:
            return 3;
        case PageRasterQuality::MediumDetail:
            return 2;
        case PageRasterQuality::LowDetail:
            return 1;
        case PageRasterQuality::Thumbnail:
            return 0;
    }
    return 0;
}

bool sameRasterRequest(const PageRasterRequest& left, const PageRasterRequest& right) {
    return left.quality == right.quality && left.pageSize.width == right.pageSize.width &&
           left.pageSize.height == right.pageSize.height && left.audioMode == right.audioMode &&
           std::abs(left.renderScale - right.renderScale) < 0.0001 &&
           std::abs(left.mediumDetailScale - right.mediumDetailScale) < 0.0001 &&
           std::abs(left.lowDetailScale - right.lowDetailScale) < 0.0001 &&
           left.backgroundPath == right.backgroundPath && left.layers.size() == right.layers.size() &&
           std::equal(
                   left.layers.begin(), left.layers.end(), right.layers.begin(),
                   [](const auto& a, const auto& b) { return a.origin == b.origin && a.path == b.path; });
}

std::string pageRasterLayerPath(const std::filesystem::path& directory, size_t origin, std::string_view prefix) {
    return (directory / (std::string(prefix) + "layer-" + std::to_string(origin) + ".png")).string();
}

void writeRaster(const std::filesystem::path& directory, const PageRaster& raster, std::string_view prefix) {
    const auto backgroundPath = (directory / (std::string(prefix) + "background.png")).string();
    if (cairo_surface_write_to_png(raster.background.get(), backgroundPath.c_str()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("Cannot write page raster background");
    }
    for (const auto& [origin, surface]: raster.layers) {
        if (cairo_surface_write_to_png(surface.get(), pageRasterLayerPath(directory, origin, prefix).c_str()) !=
            CAIRO_STATUS_SUCCESS) {
            throw std::runtime_error("Cannot write page raster layer");
        }
    }
    std::ofstream scale(directory / (std::string(prefix) + "scale.txt"));
    scale << std::setprecision(17) << raster.scale;
    if (!scale) {
        throw std::runtime_error("Cannot write page raster scale");
    }
}

void writePageRasterTask(const PageRasterTask& task) {
    const std::filesystem::path directory(task.outputDirectory);
    writeRaster(directory, task.preview, rasterPrefix(task.request.quality));
    if (task.mediumDetailPreview.background) {
        writeRaster(directory, task.mediumDetailPreview, "medium-");
    }
    if (task.lowDetailPreview.background) {
        writeRaster(directory, task.lowDetailPreview, "lod-");
    }
    if (!task.renderError.empty()) {
        std::ofstream error(directory / "error.txt");
        error << task.renderError;
        if (!error) {
            throw std::runtime_error("Cannot write page raster error");
        }
    }
}


void loadRaster(const std::filesystem::path& directory, const PageRasterRequest& request, PageRaster& raster,
                std::string_view prefix) {
    const auto backgroundPath = (directory / (std::string(prefix) + "background.png")).string();
    raster.background.reset(cairo_image_surface_create_from_png(backgroundPath.c_str()));
    if (cairo_surface_status(raster.background.get()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("Cannot load page raster background");
    }
    std::ifstream scale(directory / (std::string(prefix) + "scale.txt"));
    if (!(scale >> raster.scale) || !std::isfinite(raster.scale)) {
        throw std::runtime_error("Cannot load page raster scale");
    }
    raster.audioMode = request.audioMode;
    for (const auto& layer: request.layers) {
        const auto path = pageRasterLayerPath(directory, layer.origin, prefix);
        if (!std::filesystem::exists(path)) {
            continue;
        }
        auto surface = std::unique_ptr<cairo_surface_t, PageRasterSurfaceDeleter>(
                cairo_image_surface_create_from_png(path.c_str()));
        if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
            throw std::runtime_error("Cannot load page raster layer");
        }
        raster.layers.emplace(layer.origin, std::move(surface));
    }
}

void loadPageRasterTask(PageRasterTask& task) {
    const std::filesystem::path directory(task.outputDirectory);
    loadRaster(directory, task.request, task.preview, rasterPrefix(task.request.quality));
    if (task.request.quality == PageRasterQuality::Full) {
        loadRaster(directory, task.request, task.mediumDetailPreview, "medium-");
        loadRaster(directory, task.request, task.lowDetailPreview, "lod-");
    } else if (task.request.quality == PageRasterQuality::MediumDetail) {
        loadRaster(directory, task.request, task.lowDetailPreview, "lod-");
    }
    std::ifstream renderError(directory / "error.txt");
    if (renderError) {
        std::getline(renderError, task.renderError, '\0');
    }
}

void removePageRasterTaskFiles(const PageRasterTask& task) {
    std::error_code error;
    std::filesystem::remove_all(task.outputDirectory, error);
}

double pageRasterPriority(const PageRasterRendererState& state, int page) {
    if (page == state.selectedPage) {
        return -1;
    }
    if (const auto found = state.visiblePages.find(page); found != state.visiblePages.end()) {
        return found->second;
    }
    return std::numeric_limits<double>::infinity();
}

double rasterPriority(const PageRasterRendererState& state, const PageRasterRequest& request) {
    if (request.quality == PageRasterQuality::Full && !state.fullDetailPages.empty()) {
        const auto found = state.fullDetailPages.find(request.page);
        if (found == state.fullDetailPages.end()) {
            return std::numeric_limits<double>::infinity();
        }
        return request.page == state.selectedPage ? -1 : found->second;
    }
    if (request.quality == PageRasterQuality::MediumDetail || request.quality == PageRasterQuality::LowDetail) {
        const double tierPriority = request.quality == PageRasterQuality::MediumDetail ? 10000 : 20000;
        if (state.visiblePages.contains(request.page)) {
            return pageRasterPriority(state, request.page) + tierPriority;
        }
        const auto& retained =
                request.quality == PageRasterQuality::MediumDetail ? state.mediumRetainedPages : state.lowRetainedPages;
        if (const auto found = retained.find(request.page); found != retained.end()) {
            return found->second + tierPriority + 10000;
        }
        return std::numeric_limits<double>::infinity();
    }
    return pageRasterPriority(state, request.page);
}

void sortPageRasterQueue(PageRasterRendererState& state) {
    std::stable_sort(state.queue.begin(), state.queue.end(),
                     [](const auto& left, const auto& right) { return left.priority < right.priority; });
}

std::map<int, PageRaster>& pageRasterCache(PageRasterRendererState& state, PageRasterQuality quality) {
    if (quality == PageRasterQuality::MediumDetail) {
        return state.mediumDetailPreviews;
    }
    if (quality == PageRasterQuality::LowDetail) {
        return state.lowDetailPreviews;
    }
    return state.previews;
}

const std::map<int, double>& retainedRasterPages(const PageRasterRendererState& state, PageRasterQuality quality) {
    if (quality == PageRasterQuality::Full) {
        return state.fullDetailPages;
    }
    if (quality == PageRasterQuality::MediumDetail) {
        return state.mediumRetainedPages;
    }
    if (quality == PageRasterQuality::LowDetail) {
        return state.lowRetainedPages;
    }
    return state.visiblePages;
}

bool evictLeastRecentRaster(PageRasterRendererState& state, PageRasterQuality quality, bool offscreenOnly) {
    auto& cache = pageRasterCache(state, quality);
    const auto& retainedPages = retainedRasterPages(state, quality);
    auto oldest = cache.end();
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if ((!offscreenOnly || !retainedPages.contains(it->first)) &&
            (oldest == cache.end() || it->second.stamp < oldest->second.stamp)) {
            oldest = it;
        }
    }
    if (oldest == cache.end()) {
        return false;
    }
    cache.erase(oldest);
    return true;
}

void trimPageRasterCache(PageRasterRendererState& state, PageRasterQuality quality) {
    auto& cache = pageRasterCache(state, quality);
    const size_t maxPages = quality == PageRasterQuality::MediumDetail ? state.maxCachedMediumDetailPages :
                            quality == PageRasterQuality::LowDetail    ? state.maxCachedLowDetailPages :
                                                                         state.maxCachedPages;
    const size_t maxBytes = quality == PageRasterQuality::MediumDetail ? state.maxMediumDetailCacheBytes :
                            quality == PageRasterQuality::LowDetail    ? state.maxLowDetailCacheBytes :
                                                                         std::numeric_limits<size_t>::max();
    while (cache.size() > maxPages || rasterCacheBytes(cache) > maxBytes) {
        if (!evictLeastRecentRaster(state, quality, true) && !evictLeastRecentRaster(state, quality, false)) {
            break;
        }
    }
}

void cachePageRaster(PageRasterRendererState& state, PageRasterQuality quality, int page, PageRaster raster) {
    auto& cache = pageRasterCache(state, quality);
    raster.stamp = ++state.stamp;
    cache.insert_or_assign(page, std::move(raster));
    trimPageRasterCache(state, quality);
}

void finishPageRasterTask(const std::shared_ptr<PageRasterRendererState>& state,
                          const std::shared_ptr<PageRasterTask>& task, const std::string& error) {
    const auto active = std::find(state->activeTasks.begin(), state->activeTasks.end(), task);
    if (active != state->activeTasks.end()) {
        state->activeTasks.erase(active);
    }
    if (const auto found = state->pending.find(task->request.page);
        found != state->pending.end() && found->second == task->request.requestId) {
        state->pending.erase(found);
    }
    removePageRasterTaskFiles(*task);
    if (state->stopping) {
        schedulePageRasterTasks(state);
        return;
    }
    if (!task->cancelled && task->request.generation == state->generation) {
        if (!task->preview.background || !error.empty()) {
            if (state->failed.size() == 16) {
                state->failed.erase(state->failed.begin());
            }
            state->failed[{task->request.page, task->request.quality}] = task->request.generation;
        }
        if (!task->preview.background) {
            if (!error.empty()) {
                std::cerr << "GTK4 page raster failed: " << error << '\n';
            }
        } else {
            ++state->completedRequests;
            cachePageRaster(*state, task->request.quality, task->request.page, std::move(task->preview));
            if (task->request.quality == PageRasterQuality::Full) {
                ++state->completedFullRequests;
                cachePageRaster(*state, PageRasterQuality::MediumDetail, task->request.page,
                                std::move(task->mediumDetailPreview));
                cachePageRaster(*state, PageRasterQuality::LowDetail, task->request.page,
                                std::move(task->lowDetailPreview));
            } else if (task->request.quality == PageRasterQuality::MediumDetail) {
                ++state->completedMediumDetailRequests;
                cachePageRaster(*state, PageRasterQuality::LowDetail, task->request.page,
                                std::move(task->lowDetailPreview));
            } else if (task->request.quality == PageRasterQuality::LowDetail) {
                ++state->completedLowDetailRequests;
            }
            if (!error.empty()) {
                std::cerr << "GTK4 page raster partially rendered: " << error << '\n';
            }
            if (const auto found = state->boundThumbnails.find(task->request.page);
                found != state->boundThumbnails.end()) {
                if (auto* thumbnail = GTK_WIDGET(g_weak_ref_get(&found->second->reference))) {
                    gtk_widget_queue_draw(thumbnail);
                    g_object_unref(thumbnail);
                }
            }
            if (auto* widget = GTK_WIDGET(g_weak_ref_get(&state->redrawWidget))) {
                gtk_widget_queue_draw(widget);
                g_object_unref(widget);
            }
        }
    }
    schedulePageRasterTasks(state);
}

PopplerDocument* openPageRasterDocument(const std::string& path) {
    GError* error = nullptr;
    const auto absolutePath = std::filesystem::absolute(path).string();
    auto* uri = g_filename_to_uri(absolutePath.c_str(), nullptr, &error);
    if (!uri) {
        const std::string message = error ? error->message : "Cannot create PDF URI";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
    auto* document = poppler_document_new_from_file(uri, nullptr, &error);
    g_free(uri);
    if (!document) {
        const std::string message = error ? error->message : "Cannot open PDF thumbnail source";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }
    return document;
}

void renderPageRasterPdf(const std::string& path, int pageNumber, double scale, cairo_surface_t* surface) {
    auto* document = openPageRasterDocument(path);
    auto* page = poppler_document_get_page(document, pageNumber);
    g_object_unref(document);
    if (!page) {
        throw std::runtime_error("Cannot open PDF thumbnail page " + std::to_string(pageNumber + 1));
    }
    auto* context = cairo_create(surface);
    cairo_set_source_rgba(context, 0, 0, 0, 0);
    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    cairo_scale(context, scale, scale);
    poppler_page_render(page, context);
    const auto status = cairo_status(context);
    cairo_destroy(context);
    g_object_unref(page);
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(cairo_status_to_string(status));
    }
}

std::unique_ptr<cairo_surface_t, PageRasterSurfaceDeleter> downsampleRaster(cairo_surface_t* source, int width,
                                                                            int height) {
    auto target = std::unique_ptr<cairo_surface_t, PageRasterSurfaceDeleter>(
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
    if (cairo_surface_status(target.get()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(cairo_status_to_string(cairo_surface_status(target.get())));
    }
    const int sourceWidth = cairo_image_surface_get_width(source);
    const int sourceHeight = cairo_image_surface_get_height(source);
    auto* context = cairo_create(target.get());
    cairo_scale(context, static_cast<double>(width) / sourceWidth, static_cast<double>(height) / sourceHeight);
    cairo_set_source_surface(context, source, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_BILINEAR);
    cairo_paint(context);
    const auto status = cairo_status(context);
    cairo_destroy(context);
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(cairo_status_to_string(status));
    }
    return target;
}

PageRaster makeLowDetailRaster(const PageRaster& source, prototype::PageSize pageSize, double scale) {
    const double pageWidth = pageSize.width > 0 ? pageSize.width : 595;
    const double pageHeight = pageSize.height > 0 ? pageSize.height : 842;
    PageRaster raster;
    raster.scale = scale;
    raster.audioMode = source.audioMode;
    const int width = std::max(1, static_cast<int>(std::ceil(pageWidth * raster.scale)));
    const int height = std::max(1, static_cast<int>(std::ceil(pageHeight * raster.scale)));
    raster.background = downsampleRaster(source.background.get(), width, height);
    for (const auto& [origin, surface]: source.layers) {
        raster.layers.emplace(origin, downsampleRaster(surface.get(), width, height));
    }
    return raster;
}

void renderPageRasterTask(const std::shared_ptr<PageRasterTask>& task) {
    const auto& request = task->request;
    const double pageWidth = request.pageSize.width > 0 ? request.pageSize.width : 595;
    const double pageHeight = request.pageSize.height > 0 ? request.pageSize.height : 842;
    task->preview.scale = request.renderScale > 0 ?
                                  request.renderScale :
                                  std::min(maxPageThumbnailWidth / pageWidth, maxPageThumbnailHeight / pageHeight);
    const int width = std::max(1, static_cast<int>(std::ceil(pageWidth * task->preview.scale)));
    const int height = std::max(1, static_cast<int>(std::ceil(pageHeight * task->preview.scale)));
    task->preview.background.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
    if (cairo_surface_status(task->preview.background.get()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(cairo_status_to_string(cairo_surface_status(task->preview.background.get())));
    }
    auto* context = cairo_create(task->preview.background.get());
    cairo_set_source_rgb(context, 1, 1, 1);
    cairo_paint(context);
    cairo_destroy(context);

    std::string firstError;
    if (!request.backgroundPath.empty()) {
        try {
            auto* document = openPageRasterDocument(request.backgroundPath);
            auto* page = poppler_document_get_page(document, request.page);
            if (!page) {
                g_object_unref(document);
                throw std::runtime_error("Cannot open PDF thumbnail page " + std::to_string(request.page + 1));
            }
            context = cairo_create(task->preview.background.get());
            cairo_scale(context, task->preview.scale, task->preview.scale);
            poppler_page_render(page, context);
            const auto status = cairo_status(context);
            cairo_destroy(context);
            g_object_unref(page);
            g_object_unref(document);
            if (status != CAIRO_STATUS_SUCCESS) {
                throw std::runtime_error(cairo_status_to_string(status));
            }
        } catch (const std::exception& error) {
            firstError = error.what();
        }
    }
    for (const auto& layer: request.layers) {
        auto surface = std::unique_ptr<cairo_surface_t, PageRasterSurfaceDeleter>(
                cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
            if (firstError.empty()) {
                firstError = cairo_status_to_string(cairo_surface_status(surface.get()));
            }
            continue;
        }
        try {
            renderPageRasterPdf(layer.path, request.page, task->preview.scale, surface.get());
            task->preview.layers.emplace(layer.origin, std::move(surface));
        } catch (const std::exception& error) {
            if (firstError.empty()) {
                firstError = error.what();
            }
        }
    }
    if (request.quality == PageRasterQuality::Full || request.quality == PageRasterQuality::MediumDetail) {
        if (!std::isfinite(request.lowDetailScale) || request.lowDetailScale <= 0) {
            throw std::runtime_error("Invalid low-detail raster scale");
        }
        task->lowDetailPreview = makeLowDetailRaster(task->preview, request.pageSize, request.lowDetailScale);
    }
    if (request.quality == PageRasterQuality::Full) {
        if (!std::isfinite(request.mediumDetailScale) || request.mediumDetailScale <= 0) {
            throw std::runtime_error("Invalid medium-detail raster scale");
        }
        task->mediumDetailPreview = makeLowDetailRaster(task->preview, request.pageSize, request.mediumDetailScale);
    }
    task->renderError = std::move(firstError);
}

int runPageRasterWorkerImpl(int argc, char** argv) {
    try {
        auto task = std::make_shared<PageRasterTask>();
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--page" && index + 1 < argc) {
                task->request.page = std::stoi(argv[++index]);
            } else if (argument == "--width" && index + 1 < argc) {
                task->request.pageSize.width = std::stod(argv[++index]);
            } else if (argument == "--height" && index + 1 < argc) {
                task->request.pageSize.height = std::stod(argv[++index]);
            } else if (argument == "--scale" && index + 1 < argc) {
                task->request.renderScale = std::stod(argv[++index]);
                if (!std::isfinite(task->request.renderScale) || task->request.renderScale <= 0) {
                    throw std::runtime_error("Invalid PDF render scale");
                }
            } else if (argument == "--lod-scale" && index + 1 < argc) {
                task->request.lowDetailScale = std::stod(argv[++index]);
                if (!std::isfinite(task->request.lowDetailScale) || task->request.lowDetailScale <= 0) {
                    throw std::runtime_error("Invalid low-detail raster scale");
                }
            } else if (argument == "--medium-scale" && index + 1 < argc) {
                task->request.mediumDetailScale = std::stod(argv[++index]);
                if (!std::isfinite(task->request.mediumDetailScale) || task->request.mediumDetailScale <= 0) {
                    throw std::runtime_error("Invalid medium-detail raster scale");
                }
            } else if (argument == "--quality" && index + 1 < argc) {
                const std::string_view quality(argv[++index]);
                if (quality == "full") {
                    task->request.quality = PageRasterQuality::Full;
                } else if (quality == "medium") {
                    task->request.quality = PageRasterQuality::MediumDetail;
                } else if (quality == "lod") {
                    task->request.quality = PageRasterQuality::LowDetail;
                } else {
                    throw std::runtime_error("Invalid page raster quality");
                }
            } else if (argument == "--background" && index + 1 < argc) {
                task->request.backgroundPath = argv[++index];
            } else if (argument == "--layer" && index + 2 < argc) {
                const auto origin = static_cast<size_t>(std::stoull(argv[++index]));
                task->request.layers.push_back({origin, argv[++index]});
            } else if (argument == "--output-dir" && index + 1 < argc) {
                task->outputDirectory = argv[++index];
            } else {
                throw std::runtime_error("Invalid page raster worker arguments");
            }
        }
        if (task->request.page < 0 || task->outputDirectory.empty()) {
            throw std::runtime_error("Incomplete page raster worker request");
        }
        renderPageRasterTask(task);
        writePageRasterTask(*task);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

void completePageRasterProcess(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<PageRasterCompletion> completion(static_cast<PageRasterCompletion*>(data));
    const auto& state = completion->state;
    const auto& task = completion->task;
    GError* waitError = nullptr;
    const bool waited = g_subprocess_wait_finish(G_SUBPROCESS(source), result, &waitError);
    std::string error;
    if (!task->cancelled && !waited) {
        error = waitError ? waitError->message : "page raster process failed";
    } else if (!task->cancelled && !g_subprocess_get_successful(G_SUBPROCESS(source))) {
        error = "page raster renderer exited unsuccessfully";
    }
    g_clear_error(&waitError);
    if (task->process) {
        g_object_unref(task->process);
        task->process = nullptr;
    }
    if (!task->cancelled && error.empty()) {
        try {
            loadPageRasterTask(*task);
            task->preview.audioMode = task->request.audioMode;
            error = task->renderError;
        } catch (const std::exception& loadError) {
            error = loadError.what();
        }
    }
    finishPageRasterTask(state, task, error);
}

void startPageRasterTasks(const std::shared_ptr<PageRasterRendererState>& state) {
    if (state->activeTasks.empty()) {
        state->lastDispatchOrder.clear();
    }
    while (!state->stopping && state->activeTasks.size() < state->maxConcurrentTasks && !state->queue.empty()) {
        auto request = std::move(state->queue.front());
        state->queue.pop_front();
        if (request.generation != state->generation ||
            (!state->visiblePages.empty() && !std::isfinite(rasterPriority(*state, request)))) {
            if (const auto found = state->pending.find(request.page);
                found != state->pending.end() && found->second == request.requestId) {
                state->pending.erase(found);
            }
            continue;
        }
        auto task = std::make_shared<PageRasterTask>();
        task->request = std::move(request);
        GError* directoryError = nullptr;
        auto* outputDirectory = g_dir_make_tmp("xournalpp-page-raster-XXXXXX", &directoryError);
        if (!outputDirectory) {
            const std::string error = directoryError ? directoryError->message : "Cannot create thumbnail directory";
            g_clear_error(&directoryError);
            finishPageRasterTask(state, task, error);
            continue;
        }
        task->outputDirectory = outputDirectory;
        g_free(outputDirectory);

        std::vector<std::string> arguments{state->workerExecutable,
                                           "--gtk4-page-raster-worker",
                                           "--page",
                                           std::to_string(task->request.page),
                                           "--width",
                                           std::to_string(task->request.pageSize.width),
                                           "--height",
                                           std::to_string(task->request.pageSize.height),
                                           "--output-dir",
                                           task->outputDirectory};
        if (task->request.quality != PageRasterQuality::Thumbnail) {
            arguments.push_back("--quality");
            arguments.push_back(task->request.quality == PageRasterQuality::Full         ? "full" :
                                task->request.quality == PageRasterQuality::MediumDetail ? "medium" :
                                                                                           "lod");
        }
        if (task->request.quality == PageRasterQuality::Full) {
            arguments.push_back("--medium-scale");
            arguments.push_back(std::to_string(task->request.mediumDetailScale));
            arguments.push_back("--lod-scale");
            arguments.push_back(std::to_string(task->request.lowDetailScale));
        } else if (task->request.quality == PageRasterQuality::MediumDetail) {
            arguments.push_back("--lod-scale");
            arguments.push_back(std::to_string(task->request.lowDetailScale));
        }
        if (task->request.renderScale > 0) {
            arguments.push_back("--scale");
            arguments.push_back(std::to_string(task->request.renderScale));
        }
        if (!task->request.backgroundPath.empty()) {
            arguments.push_back("--background");
            arguments.push_back(task->request.backgroundPath);
        }
        for (const auto& layer: task->request.layers) {
            arguments.push_back("--layer");
            arguments.push_back(std::to_string(layer.origin));
            arguments.push_back(layer.path);
        }
        std::vector<const gchar*> argumentPointers;
        argumentPointers.reserve(arguments.size() + 1);
        for (const auto& argument: arguments) {
            argumentPointers.push_back(argument.c_str());
        }
        argumentPointers.push_back(nullptr);
        GError* spawnError = nullptr;
        task->process = g_subprocess_newv(argumentPointers.data(), G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &spawnError);
        if (!task->process) {
            const std::string error = spawnError ? spawnError->message : "Cannot start page raster renderer";
            g_clear_error(&spawnError);
            finishPageRasterTask(state, task, error);
            continue;
        }
        state->activeTasks.push_back(task);
        state->lastDispatchOrder.push_back(task->request.page);
        auto* process = task->process;
        g_subprocess_wait_async(process, nullptr, completePageRasterProcess, new PageRasterCompletion{state, task});
    }
}

void schedulePageRasterTasks(const std::shared_ptr<PageRasterRendererState>& state) {
    if (state->stopping || state->dispatchSource) {
        return;
    }
    state->dispatchSource = g_idle_add_full(
            G_PRIORITY_DEFAULT_IDLE,
            +[](gpointer data) -> gboolean {
                auto state = *static_cast<std::shared_ptr<PageRasterRendererState>*>(data);
                state->dispatchSource = 0;
                startPageRasterTasks(state);
                return G_SOURCE_REMOVE;
            },
            new std::shared_ptr<PageRasterRendererState>(state),
            +[](gpointer data) { delete static_cast<std::shared_ptr<PageRasterRendererState>*>(data); });
}

}  // namespace detail

using namespace detail;

PageRasterRenderer::PageRasterRenderer(std::string workerExecutable, size_t maxCachedPages, size_t maxConcurrentTasks,
                                       size_t maxQueuedRequests):
        state(std::make_shared<detail::PageRasterRendererState>(std::move(workerExecutable), maxCachedPages,
                                                                maxConcurrentTasks, maxQueuedRequests)) {}

PageRasterRenderer::~PageRasterRenderer() {
    state->stopping = true;
    state->queue.clear();
    state->pending.clear();
    if (state->dispatchSource) {
        g_source_remove(state->dispatchSource);
        state->dispatchSource = 0;
    }
    for (const auto& task: state->activeTasks) {
        task->cancelled = true;
        if (task->process) {
            g_subprocess_force_exit(task->process);
        }
    }
}

void PageRasterRenderer::setRedrawWidget(GtkWidget* widget) { g_weak_ref_set(&state->redrawWidget, G_OBJECT(widget)); }

void PageRasterRenderer::setCacheLimits(size_t fullPages, size_t mediumPages, size_t mediumBytes, size_t lowPages,
                                        size_t lowBytes) {
    state->maxCachedPages = fullPages;
    state->maxCachedMediumDetailPages = mediumPages;
    state->maxMediumDetailCacheBytes = mediumBytes;
    state->maxCachedLowDetailPages = lowPages;
    state->maxLowDetailCacheBytes = lowBytes;
    trimPageRasterCache(*state, PageRasterQuality::Full);
    trimPageRasterCache(*state, PageRasterQuality::MediumDetail);
    trimPageRasterCache(*state, PageRasterQuality::LowDetail);
}

void PageRasterRenderer::bindThumbnailWidget(int page, GtkWidget* widget) {
    auto& reference = state->boundThumbnails[page];
    if (!reference) {
        reference = std::make_unique<PreviewWidgetReference>();
    }
    g_weak_ref_set(&reference->reference, G_OBJECT(widget));
}

void PageRasterRenderer::unbindThumbnailWidget(int page, GtkWidget* widget) {
    const auto found = state->boundThumbnails.find(page);
    if (found == state->boundThumbnails.end()) {
        return;
    }
    auto* bound = GTK_WIDGET(g_weak_ref_get(&found->second->reference));
    const bool matches = bound == widget;
    if (bound) {
        g_object_unref(bound);
    }
    if (matches) {
        state->boundThumbnails.erase(found);
    }
}

void PageRasterRenderer::invalidate() {
    ++state->generation;
    state->previews.clear();
    state->mediumDetailPreviews.clear();
    state->lowDetailPreviews.clear();
    state->pending.clear();
    state->failed.clear();
    state->queue.clear();
    for (const auto& task: state->activeTasks) {
        task->cancelled = true;
        if (task->process) {
            g_subprocess_force_exit(task->process);
        }
    }
}

void PageRasterRenderer::setVisiblePages(std::map<int, double> visiblePages, int selectedPage) {
    if (visiblePages.empty()) {
        return;
    }
    state->visiblePages = std::move(visiblePages);
    state->selectedPage = state->visiblePages.contains(selectedPage) ? selectedPage : -1;
    for (auto it = state->queue.begin(); it != state->queue.end();) {
        if (const auto priority = rasterPriority(*state, *it); std::isfinite(priority)) {
            it->priority = priority;
            ++it;
        } else {
            if (const auto found = state->pending.find(it->page);
                found != state->pending.end() && found->second == it->requestId) {
                state->pending.erase(found);
            }
            ++state->offscreenCancellations;
            it = state->queue.erase(it);
        }
    }
    sortPageRasterQueue(*state);
    for (const auto& task: state->activeTasks) {
        if (!std::isfinite(rasterPriority(*state, task->request))) {
            if (!task->cancelled) {
                task->cancelled = true;
                ++state->offscreenCancellations;
            }
            if (const auto found = state->pending.find(task->request.page);
                found != state->pending.end() && found->second == task->request.requestId) {
                state->pending.erase(found);
            }
            if (task->process) {
                g_subprocess_force_exit(task->process);
            }
        }
    }
    schedulePageRasterTasks(state);
}

void PageRasterRenderer::setFullDetailPages(std::map<int, double> fullDetailPages) {
    state->fullDetailPages = std::move(fullDetailPages);
    for (auto it = state->queue.begin(); it != state->queue.end();) {
        if (it->quality == PageRasterQuality::Full && !state->fullDetailPages.contains(it->page)) {
            if (const auto pending = state->pending.find(it->page);
                pending != state->pending.end() && pending->second == it->requestId) {
                state->pending.erase(pending);
            }
            ++state->offscreenCancellations;
            it = state->queue.erase(it);
        } else {
            it->priority = rasterPriority(*state, *it);
            ++it;
        }
    }
    for (const auto& task: state->activeTasks) {
        if (task->request.quality == PageRasterQuality::Full && !state->fullDetailPages.contains(task->request.page)) {
            task->cancelled = true;
            if (const auto pending = state->pending.find(task->request.page);
                pending != state->pending.end() && pending->second == task->request.requestId) {
                state->pending.erase(pending);
            }
            if (task->process) {
                g_subprocess_force_exit(task->process);
            }
        }
    }
    sortPageRasterQueue(*state);
    schedulePageRasterTasks(state);
}

void PageRasterRenderer::setRetainedPages(std::map<int, double> mediumRetainedPages,
                                          std::map<int, double> lowRetainedPages) {
    state->mediumRetainedPages = std::move(mediumRetainedPages);
    state->lowRetainedPages = std::move(lowRetainedPages);
    for (const auto& [page, distance]: state->visiblePages) {
        state->mediumRetainedPages.try_emplace(page, distance);
        state->lowRetainedPages.try_emplace(page, distance);
    }
    std::erase_if(state->mediumDetailPreviews,
                  [this](const auto& entry) { return !state->mediumRetainedPages.contains(entry.first); });
    std::erase_if(state->lowDetailPreviews,
                  [this](const auto& entry) { return !state->lowRetainedPages.contains(entry.first); });
    for (auto it = state->queue.begin(); it != state->queue.end();) {
        const auto& retainedPages =
                it->quality == PageRasterQuality::MediumDetail ? state->mediumRetainedPages : state->lowRetainedPages;
        if ((it->quality == PageRasterQuality::MediumDetail || it->quality == PageRasterQuality::LowDetail) &&
            !retainedPages.contains(it->page)) {
            if (const auto pending = state->pending.find(it->page);
                pending != state->pending.end() && pending->second == it->requestId) {
                state->pending.erase(pending);
            }
            it = state->queue.erase(it);
        } else {
            it->priority = rasterPriority(*state, *it);
            ++it;
        }
    }
    for (const auto& task: state->activeTasks) {
        const auto& retainedPages = task->request.quality == PageRasterQuality::MediumDetail ?
                                            state->mediumRetainedPages :
                                            state->lowRetainedPages;
        if ((task->request.quality == PageRasterQuality::MediumDetail ||
             task->request.quality == PageRasterQuality::LowDetail) &&
            !retainedPages.contains(task->request.page)) {
            task->cancelled = true;
            if (const auto pending = state->pending.find(task->request.page);
                pending != state->pending.end() && pending->second == task->request.requestId) {
                state->pending.erase(pending);
            }
            if (task->process) {
                g_subprocess_force_exit(task->process);
            }
        }
    }
    sortPageRasterQueue(*state);
    schedulePageRasterTasks(state);
}

void PageRasterRenderer::request(PageRasterRequest request) {
    const auto generation = state->generation;
    request.priority = rasterPriority(*state, request);
    if (!state->visiblePages.empty() && !std::isfinite(request.priority)) {
        return;
    }
    const auto failedKey = std::make_pair(request.page, request.quality);
    if (matches(request) || (state->failed.contains(failedKey) && state->failed.at(failedKey) == generation)) {
        return;
    }
    if (const auto pending = state->pending.find(request.page); pending != state->pending.end()) {
        const uint64_t pendingId = pending->second;
        const PageRasterRequest* existing = nullptr;
        for (const auto& queued: state->queue) {
            if (queued.requestId == pendingId) {
                existing = &queued;
                break;
            }
        }
        if (!existing) {
            for (const auto& task: state->activeTasks) {
                if (task->request.requestId == pendingId) {
                    existing = &task->request;
                    break;
                }
            }
        }
        if (existing && (rasterQualityRank(existing->quality) > rasterQualityRank(request.quality) ||
                         sameRasterRequest(*existing, request))) {
            return;
        }
        std::erase_if(state->queue, [pendingId](const auto& queued) { return queued.requestId == pendingId; });
        for (const auto& task: state->activeTasks) {
            if (task->request.requestId == pendingId && !task->cancelled) {
                task->cancelled = true;
                if (task->process) {
                    g_subprocess_force_exit(task->process);
                }
            }
        }
        state->pending.erase(pending);
    }
    request.generation = generation;
    request.requestId = ++state->requestSequence;
    state->pending[request.page] = request.requestId;
    state->queue.push_back(std::move(request));
    sortPageRasterQueue(*state);
    // shortcut: keep 12 visible requests queued; grow only if large-document measurements justify it.
    while (state->queue.size() > state->maxQueuedRequests) {
        const auto dropped = state->queue.back();
        state->queue.pop_back();
        if (const auto found = state->pending.find(dropped.page);
            found != state->pending.end() && found->second == dropped.requestId) {
            state->pending.erase(found);
        }
    }
    schedulePageRasterTasks(state);
}

const PageRaster* PageRasterRenderer::find(int page) {
    PageRaster* best = nullptr;
    const auto preferSharper = [page, &best](auto& cache) {
        const auto found = cache.find(page);
        if (found != cache.end() && (!best || found->second.scale > best->scale)) {
            best = &found->second;
        }
    };
    preferSharper(state->previews);
    preferSharper(state->mediumDetailPreviews);
    preferSharper(state->lowDetailPreviews);
    if (best) {
        best->stamp = ++state->stamp;
    }
    return best;
}

bool PageRasterRenderer::matches(const PageRasterRequest& request) const {
    const auto& cache = pageRasterCache(*state, request.quality);
    const auto found = cache.find(request.page);
    if (found == cache.end()) {
        return false;
    }
    const auto& preview = found->second;
    if (request.renderScale <= 0) {
        return true;
    }
    return std::abs(preview.scale - request.renderScale) < 0.0001 && preview.audioMode == request.audioMode &&
           std::all_of(
                   request.layers.begin(), request.layers.end(),
                   [&preview](const auto& layer) { return preview.layers.contains(layer.origin); });
}

bool PageRasterRenderer::empty() const {
    return state->previews.empty() && state->mediumDetailPreviews.empty() && state->lowDetailPreviews.empty();
}

size_t PageRasterRenderer::cacheBytes() const {
    return rasterCacheBytes(state->previews) + rasterCacheBytes(state->mediumDetailPreviews) +
           rasterCacheBytes(state->lowDetailPreviews);
}

size_t PageRasterRenderer::fullDetailCachePages() const { return state->previews.size(); }

size_t PageRasterRenderer::mediumDetailCacheBytes() const { return rasterCacheBytes(state->mediumDetailPreviews); }

size_t PageRasterRenderer::mediumDetailCachePages() const { return state->mediumDetailPreviews.size(); }

size_t PageRasterRenderer::lowDetailCacheBytes() const { return rasterCacheBytes(state->lowDetailPreviews); }

size_t PageRasterRenderer::lowDetailCachePages() const { return state->lowDetailPreviews.size(); }

size_t PageRasterRenderer::completedRequests() const { return state->completedRequests; }

size_t PageRasterRenderer::completedFullRequests() const { return state->completedFullRequests; }

size_t PageRasterRenderer::completedMediumDetailRequests() const { return state->completedMediumDetailRequests; }

size_t PageRasterRenderer::completedLowDetailRequests() const { return state->completedLowDetailRequests; }

size_t PageRasterRenderer::pendingCount() const { return state->pending.size(); }

size_t PageRasterRenderer::activeCount() const { return state->activeTasks.size(); }

size_t PageRasterRenderer::offscreenCancellations() const { return state->offscreenCancellations; }

bool PageRasterRenderer::hasPending(int page) const { return state->pending.contains(page); }

bool PageRasterRenderer::hasFailed(int page, PageRasterQuality quality) const {
    return state->failed.contains({page, quality});
}

int PageRasterRenderer::nextQueuedPage() const { return state->queue.empty() ? -1 : state->queue.front().page; }

int PageRasterRenderer::preferredVisiblePage() const {
    if (state->selectedPage >= 0) {
        return state->selectedPage;
    }
    if (state->visiblePages.empty()) {
        return -1;
    }
    return std::min_element(state->visiblePages.begin(), state->visiblePages.end(),
                            [](const auto& left, const auto& right) { return left.second < right.second; })
            ->first;
}

int PageRasterRenderer::firstDispatchedPage() const {
    return state->lastDispatchOrder.empty() ? -1 : state->lastDispatchOrder.front();
}

int runPageRasterWorker(int argc, char** argv) { return detail::runPageRasterWorkerImpl(argc, argv); }

}  // namespace prototype
