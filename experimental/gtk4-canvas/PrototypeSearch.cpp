#include "Prototype.h"

namespace prototype::gtk4 {

GtkWidget* Prototype::button(const char* icon, const char* tooltip, GCallback callback) {
    auto* result = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(result, tooltip);
    g_signal_connect(result, "clicked", callback, this);
    return result;
}

GtkWidget* Prototype::createSearchBar() {
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
                         static_cast<Prototype*>(data)->searchTextChanged();
                     }),
                     this);
    g_signal_connect(searchEntry, "next-match",
                     G_CALLBACK(+[](GtkSearchEntry*, gpointer data) { static_cast<Prototype*>(data)->searchNext(); }),
                     this);
    g_signal_connect(
            searchEntry, "previous-match",
            G_CALLBACK(+[](GtkSearchEntry*, gpointer data) { static_cast<Prototype*>(data)->searchPrevious(); }), this);
    g_signal_connect(
            searchEntry, "stop-search",
            G_CALLBACK(+[](GtkSearchEntry*, gpointer data) { static_cast<Prototype*>(data)->closeSearchBar(); }), this);
    g_signal_connect(searchEntry, "activate",
                     G_CALLBACK(+[](GtkSearchEntry*, gpointer data) { static_cast<Prototype*>(data)->searchNext(); }),
                     this);

    auto* previous =
            button("go-up-symbolic", "Find previous occurrence (Shift+Enter)",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->searchPrevious(); }));
    auto* next = button("go-down-symbolic", "Find next occurrence (Enter)",
                        G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->searchNext(); }));
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
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->closeSearchBar(); }));

    gtk_box_append(GTK_BOX(row), GTK_WIDGET(searchEntry));
    gtk_box_append(GTK_BOX(row), navigation);
    gtk_box_append(GTK_BOX(row), searchStatus);
    gtk_box_append(GTK_BOX(row), close);

    auto* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed",
                     G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType modifiers,
                                    gpointer data) -> gboolean {
                         auto* self = static_cast<Prototype*>(data);
                         if (key == GDK_KEY_Escape) {
                             self->closeSearchBar();
                             return true;
                         }
                         if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
                             if (modifiers & GDK_SHIFT_MASK) {
                                 self->searchPrevious();
                             } else {
                                 self->searchNext();
                             }
                             return true;
                         }
                         return false;
                     }),
                     this);
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(GTK_WIDGET(searchEntry), keys);

    auto* searchBar = gtk_search_bar_new();
    gtk_search_bar_set_child(GTK_SEARCH_BAR(searchBar), row);
    return searchBar;
}

void Prototype::openSearchBar() {
    if (!searchBar || busy || choosingFile || messageDialog || closed) {
        return;
    }
    finishTextEditing();
    resetInput();
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(searchBar), true);
    searchStartPage = state.page;
    searchQuery = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    activeSearchMatch.reset();
    if (searchQuery.empty()) {
        searchMatches.clear();
        setSearchStatus("");
    } else {
        refreshSearchMatchesAndStatus();
    }
    gtk_widget_grab_focus(GTK_WIDGET(searchEntry));
    gtk_editable_set_position(GTK_EDITABLE(searchEntry), -1);
    update();
}

void Prototype::closeSearchBar() {
    if (!searchBar) {
        return;
    }
    gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(searchBar), false);
    searchMatches.clear();
    activeSearchMatch.reset();
    searchResultRevision = state.revision();
    searchResultPageCount = pageCount;
    searchResultNativeSession = nativeSession.get();
    if (searchEntry) {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    }
    setSearchStatus("");
    if (canvas) {
        gtk_widget_grab_focus(canvas);
        update();
    }
}

void Prototype::setSearchStatus(const std::string& text) {
    if (searchStatus) {
        gtk_label_set_text(GTK_LABEL(searchStatus), text.c_str());
    }
}

void Prototype::appendPdfSearchMatches(int pageNumber, PopplerPage* page, const std::string& query,
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

void Prototype::appendTextSearchMatches(const prototype::TextItem& item, const std::string& query,
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

bool Prototype::rebuildSearchMatches(std::string& error) {
    std::vector<SearchMatch> matches;
    if (searchQuery.empty()) {
        searchMatches.clear();
        searchResultRevision = state.revision();
        searchResultPageCount = pageCount;
        searchResultNativeSession = nativeSession.get();
        return true;
    }

    try {
        for (int pageNumber = 0; pageNumber < pageCount; ++pageNumber) {
            if (auto* pdfPage = pdfPageForPage(pageNumber)) {
                appendPdfSearchMatches(pageNumber, pdfPage, searchQuery, matches);
                g_object_unref(pdfPage);
            }

            const auto& layers = state.layersForPage(pageNumber).layers;
            for (size_t layerIndex = 0; layerIndex < layers.size(); ++layerIndex) {
                const size_t layerId = layerIndex + 1;
                const auto& layer = layers[layerIndex];
                if (!layer.visible) {
                    continue;
                }

                for (const size_t origin: layer.origins) {
                    if (origin >= layerDocuments.size()) {
                        continue;
                    }
                    if (auto* pdfPage = poppler_document_get_page(layerDocuments[origin], pageNumber)) {
                        appendPdfSearchMatches(pageNumber, pdfPage, searchQuery, matches);
                        g_object_unref(pdfPage);
                    }
                }

                std::vector<size_t> textIndices;
                for (size_t index = 0; index < state.texts.size(); ++index) {
                    const auto& text = state.texts[index];
                    if (text.page == pageNumber && text.layerId == layerId && !text.text.empty()) {
                        textIndices.push_back(index);
                    }
                }
                std::stable_sort(textIndices.begin(), textIndices.end(), [this](size_t lhs, size_t rhs) {
                    return state.texts[lhs].layerOrder < state.texts[rhs].layerOrder;
                });
                for (const size_t index: textIndices) {
                    appendTextSearchMatches(state.texts[index], searchQuery, matches);
                }
            }
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }

    searchMatches = std::move(matches);
    searchResultRevision = state.revision();
    searchResultPageCount = pageCount;
    searchResultNativeSession = nativeSession.get();
    return true;
}

size_t Prototype::searchMatchesOnPage(int page) const {
    return static_cast<size_t>(std::count_if(searchMatches.begin(), searchMatches.end(),
                                             [page](const auto& match) { return match.page == page; }));
}

void Prototype::updateSearchStatus() {
    if (!searchEntry) {
        return;
    }
    if (searchQuery.empty()) {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
        setSearchStatus("");
        return;
    }

    const auto matchesOnPage = searchMatchesOnPage(state.page);
    if (matchesOnPage == 0) {
        gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
        setSearchStatus("Text not found");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
        setSearchStatus(matchesOnPage == 1 ? "Text found once on this page" :
                                             "Text found " + std::to_string(matchesOnPage) + " times on this page");
    }
}

void Prototype::refreshSearchMatchesAndStatus() {
    std::string error;
    if (!rebuildSearchMatches(error)) {
        gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
        setSearchStatus("Search failed: " + error);
        return;
    }
    updateSearchStatus();
    update();
}

void Prototype::searchTextChanged() {
    if (!searchBar || !gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchBar))) {
        return;
    }
    searchQuery = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    searchStartPage = state.page;
    activeSearchMatch.reset();
    refreshSearchMatchesAndStatus();
}

void Prototype::ensureSearchMatchesFresh() {
    if (!searchBar || !gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchBar)) || searchQuery.empty() ||
        (searchResultRevision == state.revision() && searchResultPageCount == pageCount &&
         searchResultNativeSession == nativeSession.get())) {
        return;
    }
    activeSearchMatch.reset();
    refreshSearchMatchesAndStatus();
}

void Prototype::searchMatchesFromEntry() {
    if (!searchEntry) {
        return;
    }
    const std::string entryQuery = gtk_editable_get_text(GTK_EDITABLE(searchEntry));
    if (entryQuery != searchQuery) {
        searchTextChanged();
    } else {
        ensureSearchMatchesFresh();
    }
}

void Prototype::scrollToSearchMatch(const SearchMatch& match) {
    if (match.page != state.page) {
        navigateToPage(match.page);
    }
    if (!canvas) {
        return;
    }

    const double viewportWidth = gtk_widget_get_width(canvas);
    const double viewportHeight = gtk_widget_get_height(canvas);
    const double centerX = state.pageLeft(match.page) + (match.x0 + match.x1) / 2.0;
    const double centerY = state.pageTop(match.page) + (match.y0 + match.y1) / 2.0;
    const double layoutWidth = state.layoutWidth() * state.zoom;
    const double layoutHeight = state.layoutHeight() * state.zoom;
    if (layoutWidth > viewportWidth) {
        state.offsetX = std::clamp(viewportWidth / 2.0 - centerX * state.zoom, viewportWidth - layoutWidth, 0.0);
    }
    if (layoutHeight > viewportHeight) {
        state.offsetY = std::clamp(viewportHeight / 2.0 - centerY * state.zoom, viewportHeight - layoutHeight, 0.0);
    }
    fitMode = false;
    fitted = true;
    update();
}

void Prototype::searchNext() {
    searchMatchesFromEntry();
    if (searchMatches.empty()) {
        if (!searchQuery.empty()) {
            gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
            setSearchStatus("Text not found, searched on all pages");
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
    scrollToSearchMatch(searchMatches[next]);
    const auto matchesOnPage = searchMatchesOnPage(searchMatches[next].page);
    const auto resultOnPage = static_cast<size_t>(
            std::count_if(searchMatches.begin(), searchMatches.begin() + static_cast<std::ptrdiff_t>(next + 1),
                          [page = searchMatches[next].page](const auto& match) { return match.page == page; }));
    gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    setSearchStatus((matchesOnPage == 1 ? "Text found once on page " :
                                          "Text found " + std::to_string(matchesOnPage) + " times on page ") +
                    std::to_string(searchMatches[next].page + 1) + " (" + std::to_string(resultOnPage) + " of " +
                    std::to_string(matchesOnPage) + ")");
}

void Prototype::searchPrevious() {
    searchMatchesFromEntry();
    if (searchMatches.empty()) {
        if (!searchQuery.empty()) {
            gtk_widget_add_css_class(GTK_WIDGET(searchEntry), "error");
            setSearchStatus("Text not found, searched on all pages");
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
    scrollToSearchMatch(searchMatches[previous]);
    const auto matchesOnPage = searchMatchesOnPage(searchMatches[previous].page);
    const auto resultOnPage = static_cast<size_t>(
            std::count_if(searchMatches.begin(), searchMatches.begin() + static_cast<std::ptrdiff_t>(previous + 1),
                          [page = searchMatches[previous].page](const auto& match) { return match.page == page; }));
    gtk_widget_remove_css_class(GTK_WIDGET(searchEntry), "error");
    setSearchStatus((matchesOnPage == 1 ? "Text found once on page " :
                                          "Text found " + std::to_string(matchesOnPage) + " times on page ") +
                    std::to_string(searchMatches[previous].page + 1) + " (" + std::to_string(resultOnPage) + " of " +
                    std::to_string(matchesOnPage) + ")");
}

}  // namespace prototype::gtk4
