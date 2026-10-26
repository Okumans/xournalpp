#include "SearchPdfWorker.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <poppler.h>

namespace prototype::gtk4 {

namespace {

class SearchDocuments {
public:
    ~SearchDocuments() {
        for (const auto& entry: documents) {
            g_object_unref(entry.second);
        }
    }

    PopplerDocument* get(const std::string& path) {
        if (const auto existing = documents.find(path); existing != documents.end()) {
            return existing->second;
        }

        GError* error = nullptr;
        gchar* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
        if (!uri) {
            const std::string message = error ? error->message : "Cannot convert the PDF path to a URI";
            g_clear_error(&error);
            throw std::runtime_error(path + ": " + message);
        }

        auto* document = poppler_document_new_from_file(uri, nullptr, &error);
        g_free(uri);
        if (!document) {
            const std::string message = error ? error->message : "Cannot open PDF";
            g_clear_error(&error);
            throw std::runtime_error(path + ": " + message);
        }

        documents.emplace(path, document);
        return document;
    }

private:
    std::unordered_map<std::string, PopplerDocument*> documents;
};

std::vector<SearchPdfMatch> findMatches(SearchDocuments& documents, const SearchPdfSource& source,
                                        const std::string& query) {
    std::vector<SearchPdfMatch> matches;
    if (source.path.empty() || source.page < 0) {
        return matches;
    }

    auto* document = documents.get(source.path);
    if (source.page >= poppler_document_get_n_pages(document)) {
        return matches;
    }
    auto* page = poppler_document_get_page(document, source.page);
    if (!page) {
        return matches;
    }

    double pageHeight = 0;
    poppler_page_get_size(page, nullptr, &pageHeight);
    if (!std::isfinite(pageHeight) || pageHeight <= 0) {
        g_object_unref(page);
        return matches;
    }

    GList* results = poppler_page_find_text(page, query.c_str());
    for (GList* result = results; result; result = result->next) {
        const auto* rectangle = static_cast<const PopplerRectangle*>(result->data);
        if (!rectangle || !std::isfinite(rectangle->x1) || !std::isfinite(rectangle->y1) ||
            !std::isfinite(rectangle->x2) || !std::isfinite(rectangle->y2)) {
            continue;
        }
        const double top = pageHeight - std::max(rectangle->y1, rectangle->y2);
        const double bottom = pageHeight - std::min(rectangle->y1, rectangle->y2);
        matches.push_back(
                {std::min(rectangle->x1, rectangle->x2), top, std::max(rectangle->x1, rectangle->x2), bottom});
    }
    g_list_free_full(results, [](gpointer data) { poppler_rectangle_free(static_cast<PopplerRectangle*>(data)); });
    g_object_unref(page);
    return matches;
}

}  // namespace

struct SearchPdfWorker::Impl {
    static constexpr size_t maxBufferedPageResults = 4;

    struct Job {
        uint64_t generation;
        SearchPdfScanRequest request;
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Job> pending;
    std::deque<std::pair<uint64_t, SearchPdfPageResult>> results;
    uint64_t generation = 0;
    bool stopping = false;
    std::thread thread;

    Impl(): thread([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            pending.reset();
            ++generation;
        }
        condition.notify_all();
        thread.join();
    }

    bool isCurrent(uint64_t candidate) {
        std::lock_guard lock(mutex);
        return !stopping && generation == candidate;
    }

    bool publish(uint64_t candidate, SearchPdfPageResult result) {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this, candidate] {
            return stopping || generation != candidate || results.size() < maxBufferedPageResults;
        });
        if (stopping || generation != candidate) {
            return false;
        }
        results.emplace_back(candidate, std::move(result));
        return true;
    }

    void run() {
        while (true) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) {
                    return;
                }
                job = std::move(pending);
                pending.reset();
            }

            process(*job);
        }
    }

    void process(const Job& job) {
        SearchDocuments documents;
        for (const auto& pageSources: job.request.pages) {
            if (!isCurrent(job.generation)) {
                return;
            }

            SearchPdfPageResult result;
            result.page = pageSources.page;
            result.layerMatches.resize(pageSources.layers.size());
            try {
                if (pageSources.background) {
                    result.backgroundMatches = findMatches(documents, *pageSources.background, job.request.query);
                }
                for (size_t layer = 0; layer < pageSources.layers.size(); ++layer) {
                    for (const auto& source: pageSources.layers[layer]) {
                        if (!isCurrent(job.generation)) {
                            return;
                        }
                        auto sourceMatches = findMatches(documents, source, job.request.query);
                        auto& layerMatches = result.layerMatches[layer];
                        layerMatches.insert(layerMatches.end(), sourceMatches.begin(), sourceMatches.end());
                    }
                }
            } catch (const std::exception& exception) {
                result.error = exception.what();
                publish(job.generation, std::move(result));
                return;
            }
            if (!publish(job.generation, std::move(result))) {
                return;
            }
        }

        SearchPdfPageResult finished;
        finished.finished = true;
        publish(job.generation, std::move(finished));
    }
};

SearchPdfWorker::SearchPdfWorker(): impl(std::make_unique<Impl>()) {}

SearchPdfWorker::~SearchPdfWorker() = default;

uint64_t SearchPdfWorker::submit(SearchPdfScanRequest request) {
    std::lock_guard lock(impl->mutex);
    const uint64_t generation = ++impl->generation;
    impl->pending = Impl::Job{generation, std::move(request)};
    impl->results.clear();
    impl->condition.notify_all();
    return generation;
}

void SearchPdfWorker::cancel() {
    std::lock_guard lock(impl->mutex);
    ++impl->generation;
    impl->pending.reset();
    impl->results.clear();
    impl->condition.notify_all();
}

std::optional<SearchPdfPageResult> SearchPdfWorker::takeResult(uint64_t generation) {
    std::lock_guard lock(impl->mutex);
    if (impl->results.empty() || impl->results.front().first != generation) {
        return std::nullopt;
    }
    auto result = std::move(impl->results.front().second);
    impl->results.pop_front();
    impl->condition.notify_all();
    return result;
}

}  // namespace prototype::gtk4
