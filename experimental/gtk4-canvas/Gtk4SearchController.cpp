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
                         static_cast<Gtk4SearchController*>(data)->textChanged();
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
    searchQuery = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    activeSearchMatch.reset();
    if (searchQuery.empty()) {
        searchMatches.clear();
        setStatus("");
    } else {
        refreshMatchesAndStatus();
    }
    gtk_widget_grab_focus(GTK_WIDGET(searchEntry));
    gtk_editable_set_position(GTK_EDITABLE(searchEntry), -1);
    host.updateSearchCanvas();
}

void Gtk4SearchController::close() {
    if (!searchBar) {
        return;
    }
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(searchBar), false);
    searchMatches.clear();
    activeSearchMatch.reset();
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

void Gtk4SearchController::appendPdfSearchMatches(int pageNumber, PopplerPage* page, const std::string& query,
                                                  std::vector<SearchMatch>& matches) {
    if (!page || query.empty()) {
        return;
    }
    GList* results = poppler_page_find_text(page, query.c_str());
    for (GList* result = results; result; result = result->next) {
        const auto* rectangle = static_cast<const PopplerRectangle*>(result->data);
        if (!rectangle || !std::isfinite(rectangle->x1) || !std::isfinite(rectangle->y1) ||
            !std::isfinite(rectangle->x2) || !std::isfinite(rectangle->y2)) {
            continue;
        }
        matches.push_back({pageNumber, std::min(rectangle->x1, rectangle->x2), std::min(rectangle->y1, rectangle->y2),
                           std::max(rectangle->x1, rectangle->x2), std::max(rectangle->y1, rectangle->y2)});
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

bool Gtk4SearchController::rebuildSearchMatches(std::string& error) {
    std::vector<SearchMatch> matches;
    if (searchQuery.empty()) {
        searchMatches.clear();
        searchResultRevision = host.searchCanvasState().revision();
        searchResultPageCount = host.searchPageCount();
        searchResultDocumentIdentity = host.searchDocumentIdentity();
        return true;
    }

    try {
        for (int pageNumber = 0; pageNumber < host.searchPageCount(); ++pageNumber) {
            if (auto* pdfPage = host.pdfPageForSearch(pageNumber)) {
                appendPdfSearchMatches(pageNumber, pdfPage, searchQuery, matches);
                g_object_unref(pdfPage);
            }

            const auto& layers = host.searchCanvasState().layersForPage(pageNumber).layers;
            for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
                const size_t layerId = layerIndex + 1;
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

                std::vector<size_t> textIndices;
                for (size_t index = 0; index < host.searchCanvasState().texts.size(); ++index) {
                    const auto& text = host.searchCanvasState().texts[index];
                    if (text.page == pageNumber && text.layerId == layerId && !text.text.empty()) {
                        textIndices.push_back(index);
                    }
                }
                std::stable_sort(textIndices.begin(), textIndices.end(), [this](size_t lhs, size_t rhs) {
                    return host.searchCanvasState().texts[lhs].layerOrder <
                           host.searchCanvasState().texts[rhs].layerOrder;
                });
                for (const size_t index: textIndices) {
                    appendTextSearchMatches(host.searchCanvasState().texts[index], searchQuery, matches);
                }
            }
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }

    searchMatches = std::move(matches);
    searchResultRevision = host.searchCanvasState().revision();
    searchResultPageCount = host.searchPageCount();
    searchResultDocumentIdentity = host.searchDocumentIdentity();
    return true;
}

size_t Gtk4SearchController::countMatchesOnPage(int page) const {
    return static_cast<size_t>(std::count_if(searchMatches.begin(), searchMatches.end(),
                                             [page](const auto& match) { return match.page == page; }));
}

void Gtk4SearchController::updateStatus() {
    if (!searchEntry) {
        return;
    }
    if (searchQuery.empty()) {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
        setStatus("");
        return;
    }

    const auto matchesOnPage = countMatchesOnPage(host.searchCanvasState().page);
    if (matchesOnPage == 0) {
        gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
        setStatus("Text not found");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
        setStatus(matchesOnPage == 1 ? "Text found once on this page" :
                                       "Text found " + std::to_string(matchesOnPage) + " times on this page");
    }
}

void Gtk4SearchController::refreshMatchesAndStatus() {
    std::string error;
    if (!rebuildSearchMatches(error)) {
        gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
        setStatus("Search failed: " + error);
        return;
    }
    updateStatus();
    host.updateSearchCanvas();
}

void Gtk4SearchController::textChanged() {
    if (!searchBar || !gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchBar))) {
        return;
    }
    searchQuery = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    searchStartPage = host.searchCanvasState().page;
    activeSearchMatch.reset();
    refreshMatchesAndStatus();
}

void Gtk4SearchController::ensureMatchesFresh() {
    if (!searchBar || !gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchBar)) || searchQuery.empty() ||
        (searchResultRevision == host.searchCanvasState().revision() &&
         searchResultPageCount == host.searchPageCount() &&
         searchResultDocumentIdentity == host.searchDocumentIdentity())) {
        return;
    }
    activeSearchMatch.reset();
    refreshMatchesAndStatus();
}

void Gtk4SearchController::matchesFromEntry() {
    if (!searchEntry) {
        return;
    }
    const std::string entryQuery = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    if (entryQuery != searchQuery) {
        textChanged();
    } else {
        ensureMatchesFresh();
    }
}

void Gtk4SearchController::refreshFromEntry() { textChanged(); }

void Gtk4SearchController::ensureFresh() { ensureMatchesFresh(); }

void Gtk4SearchController::scrollToMatch(const SearchMatch& match) { host.focusSearchMatch(match); }

void Gtk4SearchController::next() {
    matchesFromEntry();
    if (searchMatches.empty()) {
        if (!searchQuery.empty()) {
            gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
            setStatus("Text not found, searched on all pages");
        }
        return;
    }

    size_t next = 0;
    if (activeSearchMatch) {
        next = (*activeSearchMatch + 1) % searchMatches.size();
    } else {
        const auto first = std::lower_bound(searchMatches.begin(), searchMatches.end(), searchStartPage,
                                            [](const SearchMatch& match, int page) { return match.page < page; });
        next = first == searchMatches.end() ? 0 : static_cast<size_t>(first - searchMatches.begin());
    }
    activeSearchMatch = next;
    scrollToMatch(searchMatches[next]);
    const auto matchesOnPage = countMatchesOnPage(searchMatches[next].page);
    const auto resultOnPage = static_cast<size_t>(
            std::count_if(searchMatches.begin(), searchMatches.begin() + static_cast<std::ptrdiff_t>(next + 1),
                          [page = searchMatches[next].page](const auto& match) { return match.page == page; }));
    gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    setStatus((matchesOnPage == 1 ? "Text found once on page " :
                                    "Text found " + std::to_string(matchesOnPage) + " times on page ") +
              std::to_string(searchMatches[next].page + 1) + " (" + std::to_string(resultOnPage) + " of " +
              std::to_string(matchesOnPage) + ")");
}

void Gtk4SearchController::previous() {
    matchesFromEntry();
    if (searchMatches.empty()) {
        if (!searchQuery.empty()) {
            gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
            setStatus("Text not found, searched on all pages");
        }
        return;
    }

    size_t previous = searchMatches.size() - 1;
    if (activeSearchMatch) {
        previous = *activeSearchMatch == 0 ? searchMatches.size() - 1 : *activeSearchMatch - 1;
    } else {
        const auto first = std::lower_bound(searchMatches.begin(), searchMatches.end(), searchStartPage,
                                            [](const SearchMatch& match, int page) { return match.page < page; });
        previous = first == searchMatches.begin() ? searchMatches.size() - 1 :
                                                    static_cast<size_t>(first - searchMatches.begin() - 1);
    }
    activeSearchMatch = previous;
    scrollToMatch(searchMatches[previous]);
    const auto matchesOnPage = countMatchesOnPage(searchMatches[previous].page);
    const auto resultOnPage = static_cast<size_t>(
            std::count_if(searchMatches.begin(), searchMatches.begin() + static_cast<std::ptrdiff_t>(previous + 1),
                          [page = searchMatches[previous].page](const auto& match) { return match.page == page; }));
    gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    setStatus((matchesOnPage == 1 ? "Text found once on page " :
                                    "Text found " + std::to_string(matchesOnPage) + " times on page ") +
              std::to_string(searchMatches[previous].page + 1) + " (" + std::to_string(resultOnPage) + " of " +
              std::to_string(matchesOnPage) + ")");
}

bool Gtk4SearchController::isOpen() const {
    return searchBar && gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchBar));
}

void Gtk4SearchController::drawMatches(cairo_t* cr, int page) const {
    for (size_t index = 0; index < searchMatches.size(); ++index) {
        const auto& match = searchMatches[index];
        if (match.page != page) {
            continue;
        }
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
