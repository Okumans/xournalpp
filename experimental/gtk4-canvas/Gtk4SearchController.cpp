#include "Gtk4SearchController.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <pango/pangocairo.h>

#include "CanvasSearch.h"
#include "TextRenderer.h"

namespace prototype::gtk4 {

Gtk4SearchController::~Gtk4SearchController() { cancelSearch(); }

GtkWidget* Gtk4SearchController::button(const char* icon, const char* tooltip, GCallback callback) {
    auto* result = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(result, tooltip);
    g_signal_connect(result, "clicked", callback, this);
    return result;
}

GtkWidget* Gtk4SearchController::createWidget() {
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(row, "toolbar");
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_widget_set_margin_top(row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    searchEntry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(searchEntry, "Find in document");
    gtk_widget_set_size_request(GTK_WIDGET(searchEntry), 300, -1);
    gtk_widget_set_hexpand(GTK_WIDGET(searchEntry), false);
    g_signal_connect(searchEntry, "search-changed", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                         static_cast<Gtk4SearchController*>(data)->queryEdited();
                     }),
                     this);
    g_signal_connect(searchEntry, "next-match", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                         static_cast<Gtk4SearchController*>(data)->next();
                     }),
                     this);
    g_signal_connect(searchEntry, "previous-match", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                         static_cast<Gtk4SearchController*>(data)->previous();
                     }),
                     this);
    g_signal_connect(searchEntry, "stop-search", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                         static_cast<Gtk4SearchController*>(data)->close();
                     }),
                     this);
    g_signal_connect(searchEntry, "activate", G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                         static_cast<Gtk4SearchController*>(data)->next();
                     }),
                     this);

    auto* previous = button(
            "go-up-symbolic", "Find previous occurrence (Shift+Enter)",
            G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Gtk4SearchController*>(data)->previous(); }));
    auto* next =
            button("go-down-symbolic", "Find next occurrence (Enter)",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Gtk4SearchController*>(data)->next(); }));
    auto* navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(navigation, "linked");
    gtk_box_append(GTK_BOX(navigation), previous);
    gtk_box_append(GTK_BOX(navigation), next);

    searchStatus = gtk_label_new("");
    gtk_widget_add_css_class(searchStatus, "dim-label");
    gtk_widget_set_hexpand(searchStatus, true);
    gtk_widget_set_halign(searchStatus, GTK_ALIGN_START);
    gtk_widget_set_margin_start(searchStatus, 8);

    auto* close =
            button("window-close-symbolic", "Close search (Escape)",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Gtk4SearchController*>(data)->close(); }));

    gtk_box_append(GTK_BOX(row), GTK_WIDGET(searchEntry));
    gtk_box_append(GTK_BOX(row), navigation);
    gtk_box_append(GTK_BOX(row), searchStatus);
    gtk_box_append(GTK_BOX(row), close);

    auto* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType modifiers,
                                    gpointer data) -> gboolean {
                         auto* self = static_cast<Gtk4SearchController*>(data);
                         if (key == GDK_KEY_Escape) {
                             self->close();
                             return true;
                         }
                         if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
                             if (modifiers & GDK_SHIFT_MASK) {
                                 self->previous();
                             } else {
                                 self->next();
                             }
                             return true;
                         }
                         return false;
                     }),
                     this);
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(GTK_WIDGET(searchEntry), keys);

    searchBar = gtk_search_bar_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(searchBar), row);
    return searchBar;
}

void Gtk4SearchController::open() {
    if (!searchBar || !host.canOpenSearchBar()) {
        return;
    }
    host.finishTextEditingForSearch();
    host.resetInputForSearch();
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(searchBar), true);
    searchStartPage = host.searchCanvasState().page;
    cancelSearch();
    searchQuery.clear();
    searchHasRun = false;
    searchMatches.clear();
    searchMatchesByPage.clear();
    searchTextIndicesByPageAndLayer.clear();
    activeSearchMatch.reset();
    setStatus(gtk_editable_get_text(GTK_EDITABLE(searchEntry))[0] == '\0' ? "" : "Press Enter to search");
    gtk_widget_grab_focus(GTK_WIDGET(searchEntry));
    gtk_editable_set_position(GTK_EDITABLE(searchEntry), -1);
    host.updateSearchCanvas();
}

void Gtk4SearchController::close() {
    if (!searchBar) {
        return;
    }
    cancelSearch();
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(searchBar), false);
    searchMatches.clear();
    searchMatchesByPage.clear();
    searchTextIndicesByPageAndLayer.clear();
    activeSearchMatch.reset();
    searchQuery.clear();
    searchHasRun = false;
    searchResultRevision = host.searchCanvasState().revision();
    searchResultPageCount = host.searchPageCount();
    searchResultDocumentIdentity = host.searchDocumentIdentity();
    if (searchEntry) {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    }
    setStatus("");
    if (auto* canvas = host.searchCanvasWidget()) {
        gtk_widget_grab_focus(canvas);
        host.updateSearchCanvas();
    }
}

void Gtk4SearchController::setStatus(const std::string& text) {
    if (searchStatus) {
        gtk_label_set_text(GTK_LABEL(searchStatus), text.c_str());
    }
}

void Gtk4SearchController::cancelSearch() {
    if (searchIdleSource) {
        g_source_remove(searchIdleSource);
        searchIdleSource = 0;
    }
    searchInProgress = false;
    pendingNavigationDirection = 0;
}

void Gtk4SearchController::appendPdfSearchMatches(int pageNumber, PopplerPage* page, const std::string& query,
                                                  std::vector<SearchMatch>& matches) {
    if (!page || query.empty()) {
        return;
    }
    double pageHeight = 0;
    poppler_page_get_size(page, nullptr, &pageHeight);
    if (!std::isfinite(pageHeight) || pageHeight <= 0) {
        return;
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
        matches.push_back({pageNumber, std::min(rectangle->x1, rectangle->x2), top,
                           std::max(rectangle->x1, rectangle->x2), bottom});
    }
    g_list_free_full(results, [](gpointer data) { poppler_rectangle_free(static_cast<PopplerRectangle*>(data)); });
}

void Gtk4SearchController::appendTextSearchMatches(const prototype::TextItem& item, const std::string& query,
                                                   std::vector<SearchMatch>& matches) {
    if (item.text.empty() || item.text.size() > static_cast<size_t>(G_MAXINT)) {
        return;
    }
    const auto ranges = prototype::search::caseInsensitiveMatches(item.text, query);
    if (ranges.empty()) {
        return;
    }

    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto* cr = cairo_create(surface);
    auto* layout = prototype::createTextLayout(cr, item);
    const auto transform = [&](double x, double y) {
        return prototype::Point{item.xx * x + item.xy * y + item.x, item.yx * x + item.yy * y + item.y};
    };

    for (const auto& range: ranges) {
        if (range.start >= range.end || range.end > item.text.size() || range.start > G_MAXINT ||
            range.end - 1 > G_MAXINT) {
            continue;
        }
        PangoRectangle first{};
        PangoRectangle last{};
        pango_layout_index_to_pos(layout, static_cast<int>(range.start), &first);
        pango_layout_index_to_pos(layout, static_cast<int>(range.end - 1), &last);

        const double x0 =
                std::min(static_cast<double>(first.x), static_cast<double>(first.x + first.width)) / PANGO_SCALE;
        const double x1 = std::max(static_cast<double>(last.x), static_cast<double>(last.x + last.width)) / PANGO_SCALE;
        const double y0 =
                std::min(static_cast<double>(first.y), static_cast<double>(first.y + first.height)) / PANGO_SCALE;
        const double y1 =
                std::max(static_cast<double>(last.y), static_cast<double>(last.y + last.height)) / PANGO_SCALE;
        const std::array<prototype::Point, 4> corners{transform(x0, y0), transform(x1, y0), transform(x1, y1),
                                                      transform(x0, y1)};
        double minX = corners.front().x;
        double maxX = corners.front().x;
        double minY = corners.front().y;
        double maxY = corners.front().y;
        for (const auto& corner: corners) {
            minX = std::min(minX, corner.x);
            maxX = std::max(maxX, corner.x);
            minY = std::min(minY, corner.y);
            maxY = std::max(maxY, corner.y);
        }
        if (std::isfinite(minX) && std::isfinite(minY) && std::isfinite(maxX) && std::isfinite(maxY)) {
            matches.push_back({item.page, minX, minY, maxX, maxY});
        }
    }
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

bool Gtk4SearchController::appendPageSearchMatches(int pageNumber, std::vector<SearchMatch>& matches,
                                                   std::string& error) {
    try {
        if (auto* pdfPage = host.pdfPageForSearch(pageNumber)) {
            appendPdfSearchMatches(pageNumber, pdfPage, searchQuery, matches);
            g_object_unref(pdfPage);
        }

        const auto& layers = host.searchCanvasState().layersForPage(pageNumber).layers;
        const auto& textIndicesByLayer = searchTextIndicesByPageAndLayer[static_cast<size_t>(pageNumber)];
        for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
            const auto& layer = layers[layerIndex];
            if (!layer.visible) {
                continue;
            }

            for (const size_t origin: layer.origins) {
                if (auto* pdfPage = host.layerPdfPageForSearch(pageNumber, origin)) {
                    appendPdfSearchMatches(pageNumber, pdfPage, searchQuery, matches);
                    g_object_unref(pdfPage);
                }
            }

            if (layerIndex < textIndicesByLayer.size()) {
                for (const size_t index: textIndicesByLayer[layerIndex]) {
                    appendTextSearchMatches(host.searchCanvasState().texts[index], searchQuery, matches);
                }
            }
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    return true;
}

void Gtk4SearchController::beginSearch(std::string query, int direction) {
    cancelSearch();
    searchMatches.clear();
    searchMatchesByPage.clear();
    searchTextIndicesByPageAndLayer.clear();
    activeSearchMatch.reset();
    searchQuery = std::move(query);
    searchHasRun = !searchQuery.empty();
    searchStartPage = host.searchCanvasState().page;
    searchScanDirection = direction < 0 ? -1 : 1;
    searchResultRevision = host.searchCanvasState().revision();
    searchResultPageCount = host.searchPageCount();
    searchResultDocumentIdentity = host.searchDocumentIdentity();

    if (searchEntry) {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    }
    if (searchQuery.empty() || searchResultPageCount <= 0) {
        setStatus("");
        host.updateSearchCanvas();
        return;
    }

    const size_t pageCount = static_cast<size_t>(searchResultPageCount);
    searchMatchesByPage.resize(pageCount);
    searchTextIndicesByPageAndLayer.resize(pageCount);
    for (size_t page = 0; page < pageCount; ++page) {
        searchTextIndicesByPageAndLayer[page].resize(
                host.searchCanvasState().layersForPage(static_cast<int>(page)).layers.size());
    }
    const auto& texts = host.searchCanvasState().texts;
    for (size_t index = 0; index < texts.size(); ++index) {
        const auto& item = texts[index];
        if (item.text.empty() || item.page < 0 || item.page >= searchResultPageCount || item.layerId == 0) {
            continue;
        }
        auto& pageLayers = searchTextIndicesByPageAndLayer[static_cast<size_t>(item.page)];
        const size_t layerIndex = item.layerId - 1;
        if (layerIndex < pageLayers.size()) {
            pageLayers[layerIndex].push_back(index);
        }
    }
    for (auto& page: searchTextIndicesByPageAndLayer) {
        for (auto& layer: page) {
            std::stable_sort(layer.begin(), layer.end(), [this](size_t lhs, size_t rhs) {
                return host.searchCanvasState().texts[lhs].layerOrder < host.searchCanvasState().texts[rhs].layerOrder;
            });
        }
    }

    searchPagesScanned = 0;
    const int pageCountInt = searchResultPageCount;
    const int startPage = std::clamp(searchStartPage, 0, pageCountInt - 1);
    searchScanPage = searchScanDirection > 0 ? startPage : (startPage + pageCountInt - 1) % pageCountInt;
    searchInProgress = true;
    setStatus("Searching document…");
    host.updateSearchCanvas();
    searchIdleSource =
            g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, &Gtk4SearchController::processSearchPageCallback, this, nullptr);
}

void Gtk4SearchController::queryEdited() {
    if (!isOpen()) {
        return;
    }
    const auto query = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    if (query != searchQuery) {
        const bool hadResults = searchInProgress || !searchMatches.empty();
        cancelSearch();
        searchMatches.clear();
        searchMatchesByPage.clear();
        activeSearchMatch.reset();
        searchQuery.clear();
        searchHasRun = false;
        if (searchEntry) {
            gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
        }
        setStatus(query[0] == '\0' ? "" : "Press Enter to search");
        if (hadResults) {
            host.updateSearchCanvas();
        }
    } else {
        updateStatus();
    }
}

gboolean Gtk4SearchController::processSearchPageCallback(gpointer data) {
    auto* self = static_cast<Gtk4SearchController*>(data);
    if (!self->searchInProgress) {
        self->searchIdleSource = 0;
        return G_SOURCE_REMOVE;
    }

    if (self->searchResultRevision != self->host.searchCanvasState().revision() ||
        self->searchResultPageCount != self->host.searchPageCount() ||
        self->searchResultDocumentIdentity != self->host.searchDocumentIdentity()) {
        const std::string query = self->searchQuery;
        const int direction = self->searchScanDirection;
        self->searchIdleSource = 0;
        self->searchInProgress = false;
        self->beginSearch(query, direction);
        return G_SOURCE_REMOVE;
    }

    std::vector<SearchMatch> pageMatches;
    std::string error;
    if (!self->appendPageSearchMatches(self->searchScanPage, pageMatches, error)) {
        self->searchIdleSource = 0;
        self->searchInProgress = false;
        gtk_widget_add_css_class(GTK_WIDGET(self->searchEntry), "error");
        self->setStatus("Search failed: " + error);
        return G_SOURCE_REMOVE;
    }
    if (self->searchScanDirection < 0) {
        std::reverse(pageMatches.begin(), pageMatches.end());
    }

    const size_t firstNewMatch = self->searchMatches.size();
    auto& pageIndices = self->searchMatchesByPage[static_cast<size_t>(self->searchScanPage)];
    for (auto& match: pageMatches) {
        pageIndices.push_back(self->searchMatches.size());
        self->searchMatches.push_back(std::move(match));
    }
    ++self->searchPagesScanned;

    if (!self->activeSearchMatch && self->searchMatches.size() > firstNewMatch) {
        self->activateMatch(firstNewMatch);
    } else if (self->pendingNavigationDirection > 0 && self->searchMatches.size() > firstNewMatch) {
        self->pendingNavigationDirection = 0;
        self->activateMatch(firstNewMatch);
    }
    if (!pageMatches.empty()) {
        self->host.updateSearchCanvas();
    }

    if (self->searchPagesScanned >= self->searchResultPageCount) {
        self->searchInProgress = false;
        self->searchIdleSource = 0;
        if (!self->searchMatches.empty() && self->pendingNavigationDirection != 0) {
            const size_t target = self->pendingNavigationDirection > 0 ? 0 : self->searchMatches.size() - 1;
            self->pendingNavigationDirection = 0;
            self->activateMatch(target);
        } else {
            self->updateStatus();
        }
        return G_SOURCE_REMOVE;
    }

    const int pageCount = self->searchResultPageCount;
    self->searchScanPage = (self->searchScanPage + self->searchScanDirection + pageCount) % pageCount;
    self->updateStatus();
    return G_SOURCE_CONTINUE;
}

void Gtk4SearchController::updateStatus() {
    if (!searchEntry) {
        return;
    }
    if (gtk_editable_get_text(GTK_EDITABLE(searchEntry)) != searchQuery) {
        setStatus(gtk_editable_get_text(GTK_EDITABLE(searchEntry))[0] == '\0' ? "" : "Press Enter to search");
        return;
    }
    if (searchQuery.empty()) {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
        setStatus("");
        return;
    }
    if (!activeSearchMatch) {
        if (searchInProgress) {
            setStatus("Searching document…");
        } else if (searchHasRun) {
            gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
            setStatus("Text not found, searched on all pages");
        }
        return;
    }

    const auto& match = searchMatches[*activeSearchMatch];
    const auto& pageMatches = searchMatchesByPage[static_cast<size_t>(match.page)];
    const auto position = std::find(pageMatches.begin(), pageMatches.end(), *activeSearchMatch);
    const size_t resultOnPage = static_cast<size_t>(position - pageMatches.begin()) + 1;
    const size_t matchesOnPage = pageMatches.size();
    gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    setStatus((matchesOnPage == 1 ? "Text found once on page " :
                                    "Text found " + std::to_string(matchesOnPage) + " times on page ") +
              std::to_string(match.page + 1) + " (" + std::to_string(resultOnPage) + " of " +
              std::to_string(matchesOnPage) + ")" + (searchInProgress ? " · Searching…" : ""));
}

void Gtk4SearchController::ensureMatchesFresh() {
    if (!isOpen() || !searchHasRun || searchQuery.empty() ||
        (searchResultRevision == host.searchCanvasState().revision() &&
         searchResultPageCount == host.searchPageCount() &&
         searchResultDocumentIdentity == host.searchDocumentIdentity())) {
        return;
    }
    beginSearch(searchQuery, 1);
}

bool Gtk4SearchController::matchesFromEntry(int direction) {
    if (!searchEntry) {
        return false;
    }
    const std::string entryQuery = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    if (entryQuery.empty()) {
        if (searchHasRun || !searchMatches.empty()) {
            beginSearch({}, direction);
        }
        return false;
    }
    if (entryQuery != searchQuery || !searchHasRun) {
        beginSearch(entryQuery, direction);
        return false;
    }
    if (searchResultRevision != host.searchCanvasState().revision() ||
        searchResultPageCount != host.searchPageCount() ||
        searchResultDocumentIdentity != host.searchDocumentIdentity()) {
        beginSearch(entryQuery, direction);
        return false;
    }
    return true;
}

void Gtk4SearchController::refreshFromEntry() {
    if (!isOpen() || !searchEntry) {
        return;
    }
    const std::string query = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    if (query != searchQuery || !searchHasRun) {
        beginSearch(query, 1);
    }
}

void Gtk4SearchController::ensureFresh() { ensureMatchesFresh(); }

void Gtk4SearchController::scrollToMatch(const SearchMatch& match) { host.focusSearchMatch(match); }

void Gtk4SearchController::activateMatch(size_t index) {
    if (index >= searchMatches.size()) {
        return;
    }
    activeSearchMatch = index;
    scrollToMatch(searchMatches[index]);
    updateStatus();
}

void Gtk4SearchController::navigate(int direction) {
    if (searchMatches.empty()) {
        if (searchInProgress) {
            pendingNavigationDirection = direction;
            setStatus("Searching document…");
        } else {
            updateStatus();
        }
        return;
    }

    size_t target = 0;
    if (direction > 0) {
        target = activeSearchMatch ? *activeSearchMatch + 1 : 0;
        if (target >= searchMatches.size()) {
            if (searchInProgress) {
                pendingNavigationDirection = 1;
                updateStatus();
                return;
            }
            target = 0;
        }
    } else {
        if (activeSearchMatch && *activeSearchMatch > 0) {
            target = *activeSearchMatch - 1;
        } else if (searchInProgress) {
            pendingNavigationDirection = -1;
            updateStatus();
            return;
        } else {
            target = searchMatches.size() - 1;
        }
    }
    activateMatch(target);
}

void Gtk4SearchController::next() {
    if (matchesFromEntry(1)) {
        navigate(1);
    }
}

void Gtk4SearchController::previous() {
    if (matchesFromEntry(-1)) {
        navigate(-1);
    }
}

bool Gtk4SearchController::isOpen() const {
    return searchBar && gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchBar));
}

void Gtk4SearchController::drawMatches(cairo_t* cr, int page) const {
    if (page < 0 || static_cast<size_t>(page) >= searchMatchesByPage.size()) {
        return;
    }
    for (const size_t index: searchMatchesByPage[static_cast<size_t>(page)]) {
        const auto& match = searchMatches[index];
        const bool active = activeSearchMatch && *activeSearchMatch == index;
        cairo_rectangle(cr, match.x0, match.y0, match.x1 - match.x0, match.y1 - match.y0);
        if (active) {
            cairo_set_source_rgba(cr, 1.0, 0.48, 0.12, 0.25);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.92, 0.28, 0.04, 0.95);
        } else {
            cairo_set_source_rgba(cr, 0.98, 0.78, 0.18, 0.22);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.84, 0.56, 0.02, 0.82);
        }
        cairo_set_line_width(cr, 1.0 / host.searchCanvasState().zoom);
        cairo_stroke(cr);
    }
}

}  // namespace prototype::gtk4
