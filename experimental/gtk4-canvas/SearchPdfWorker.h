#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prototype::gtk4 {

struct SearchPdfSource {
    std::string path;
    int page = -1;
};

struct SearchPdfPageSources {
    int page = 0;
    std::optional<SearchPdfSource> background;
    std::vector<std::vector<SearchPdfSource>> layers;
};

struct SearchPdfPageRequest {
    std::string query;
    // Search requests are deliberately limited to one page. The controller
    // submits the next page only after this one has completed.
    SearchPdfPageSources page;
    const void* documentIdentity = nullptr;
    size_t documentRevision = 0;
    // Keeps staged native PDF paths valid until the worker has stopped using them.
    std::shared_ptr<const void> sourceLifetime;
};

struct SearchPdfMatch {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
};

struct SearchPdfPageResult {
    int page = 0;
    std::vector<SearchPdfMatch> backgroundMatches;
    std::vector<std::vector<SearchPdfMatch>> layerMatches;
    std::string error;
};

// Owns its Poppler documents on a dedicated thread and publishes one page per request.
class SearchPdfWorker {
public:
    SearchPdfWorker();
    ~SearchPdfWorker();

    SearchPdfWorker(const SearchPdfWorker&) = delete;
    SearchPdfWorker& operator=(const SearchPdfWorker&) = delete;

    uint64_t submit(SearchPdfPageRequest request);
    void cancel();
    std::optional<SearchPdfPageResult> takeResult(uint64_t generation);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace prototype::gtk4
