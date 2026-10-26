#include "SearchPdfWorker.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
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
    ~SearchDocuments() { clear(); }

    void clear() {
        for (const auto& entry: documents) {
            g_object_unref(entry.second);
        }
        documents.clear();
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
    struct Job {
        uint64_t generation;
        SearchPdfPageRequest request;
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Job> pending;
    std::optional<std::pair<uint64_t, SearchPdfPageResult>> publishedResult;
    uint64_t generation = 0;
    bool stopping = false;
    SearchDocuments documents;
    const void* documentIdentity = nullptr;
    size_t documentRevision = 0;
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
        condition.wait(lock, [this, candidate] { return stopping || generation != candidate || !publishedResult; });
        if (stopping || generation != candidate) {
            return false;
        }
        publishedResult.emplace(candidate, std::move(result));
        return true;
    }

    void run() {
        while (true) {
            std::optional<Job> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) {
                    documents.clear();
                    return;
                }
                job = std::move(pending);
                pending.reset();
            }

            process(*job);
        }
    }

    void process(const Job& job) {
        if (!isCurrent(job.generation)) {
            return;
        }
        if (documentIdentity != job.request.documentIdentity || documentRevision != job.request.documentRevision) {
            documents.clear();
            documentIdentity = job.request.documentIdentity;
            documentRevision = job.request.documentRevision;
        }

        SearchPdfPageResult result;
        result.page = job.request.page.page;
        result.layerMatches.resize(job.request.page.layers.size());
        try {
            if (job.request.page.background) {
                result.backgroundMatches = findMatches(documents, *job.request.page.background, job.request.query);
            }
            for (size_t layer = 0; layer < job.request.page.layers.size(); ++layer) {
                for (const auto& source: job.request.page.layers[layer]) {
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
};

SearchPdfWorker::SearchPdfWorker(): impl(std::make_unique<Impl>()) {}

SearchPdfWorker::~SearchPdfWorker() = default;

uint64_t SearchPdfWorker::submit(SearchPdfPageRequest request) {
    std::lock_guard lock(impl->mutex);
    const uint64_t generation = ++impl->generation;
    impl->pending = Impl::Job{generation, std::move(request)};
    impl->publishedResult.reset();
    impl->condition.notify_all();
    return generation;
}

void SearchPdfWorker::cancel() {
    std::lock_guard lock(impl->mutex);
    ++impl->generation;
    impl->pending.reset();
    impl->publishedResult.reset();
    impl->condition.notify_all();
}

std::optional<SearchPdfPageResult> SearchPdfWorker::takeResult(uint64_t generation) {
    std::lock_guard lock(impl->mutex);
    if (!impl->publishedResult || impl->publishedResult->first != generation) {
        return std::nullopt;
    }
    auto result = std::move(impl->publishedResult->second);
    impl->publishedResult.reset();
    impl->condition.notify_all();
    return result;
}

}  // namespace prototype::gtk4
