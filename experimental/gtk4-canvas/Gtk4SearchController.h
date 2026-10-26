#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "CanvasState.h"
#include "SearchPdfWorker.h"

namespace prototype::gtk4 {

struct SearchMatch {
    int page = 0;
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
};

class SearchControllerHost {
public:
    virtual ~SearchControllerHost() = default;

    virtual bool canOpenSearchBar() const = 0;
    virtual int searchPageCount() const = 0;
    virtual prototype::CanvasState& searchCanvasState() = 0;
    virtual std::optional<SearchPdfSource> searchBackgroundPdfSource(int page) const = 0;
    virtual std::optional<SearchPdfSource> searchLayerPdfSource(int page, size_t origin) const = 0;
    virtual std::shared_ptr<const void> searchDocumentLifetime() const = 0;
    virtual const void* searchDocumentIdentity() const = 0;
    virtual GtkWidget* searchCanvasWidget() const = 0;
    virtual void finishTextEditingForSearch() = 0;
    virtual void resetInputForSearch() = 0;
    virtual void focusSearchMatch(const SearchMatch& match) = 0;
    virtual void updateSearchCanvas() = 0;
};

class Gtk4SearchController {
public:
    explicit Gtk4SearchController(SearchControllerHost& host): host(host) {}
    ~Gtk4SearchController();

    GtkWidget* createWidget();
    void open();
    void close();
    void next();
    void previous();
    void refreshFromEntry();
    void ensureFresh();
    void drawMatches(cairo_t* cr, int page) const;

    GtkWidget* widget() const { return searchBar; }
    GtkSearchEntry* entry() const { return searchEntry; }
    const std::vector<SearchMatch>& matches() const { return searchMatches; }
    const std::optional<size_t>& activeMatch() const { return activeSearchMatch; }
    bool isOpen() const;
    bool isSearching() const { return searchInProgress; }
    int pagesScanned() const { return searchPagesScanned; }

private:
    GtkWidget* button(const char* icon, const char* tooltip, GCallback callback);
    void setStatus(const std::string& text);
    void cancelSearch();
    void beginSearch(std::string query, int direction);
    void queryEdited();
    static gboolean processSearchPageCallback(gpointer data);
    void appendTextSearchMatches(const prototype::TextItem& item, const std::string& query,
                                 std::vector<SearchMatch>& matches);
    bool appendPageSearchMatches(const SearchPdfPageResult& pdfResults, std::vector<SearchMatch>& matches,
                                 std::string& error);
    void updateStatus();
    void ensureMatchesFresh();
    bool matchesFromEntry(int direction);
    void navigate(int direction);
    void activateMatch(size_t index);
    void scrollToMatch(const SearchMatch& match);

    SearchControllerHost& host;
    SearchPdfWorker searchPdfWorker;
    GtkWidget* searchBar = nullptr;
    GtkSearchEntry* searchEntry = nullptr;
    GtkWidget* searchStatus = nullptr;
    std::vector<SearchMatch> searchMatches;
    std::vector<std::vector<size_t>> searchMatchesByPage;
    std::vector<std::vector<std::vector<size_t>>> searchTextIndicesByPageAndLayer;
    std::optional<size_t> activeSearchMatch;
    std::string searchQuery;
    int searchStartPage = 0;
    int searchScanPage = 0;
    int searchScanDirection = 1;
    int searchPagesScanned = 0;
    int pendingNavigationDirection = 0;
    uint64_t searchPdfGeneration = 0;
    guint searchPollSource = 0;
    bool searchInProgress = false;
    bool searchHasRun = false;
    size_t searchResultRevision = std::numeric_limits<size_t>::max();
    int searchResultPageCount = -1;
    const void* searchResultDocumentIdentity = nullptr;
};

}  // namespace prototype::gtk4
