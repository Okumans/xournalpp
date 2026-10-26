#include "Prototype.h"

namespace prototype::gtk4 {

bool Prototype::canOpenSearchBar() const { return !busy && !choosingFile && !messageDialog && !closed; }

int Prototype::searchPageCount() const { return pageCount; }

prototype::CanvasState& Prototype::searchCanvasState() { return state; }

PopplerPage* Prototype::pdfPageForSearch(int page) { return pdfPageForPage(page); }

PopplerPage* Prototype::layerPdfPageForSearch(int page, size_t origin) {
    if (origin >= layerDocuments.size() || !layerDocuments[origin]) {
        return nullptr;
    }
    return poppler_document_get_page(layerDocuments[origin], page);
}

std::optional<SearchPdfSource> Prototype::searchBackgroundPdfSource(int page) const {
    if (page < 0) {
        return std::nullopt;
    }
    if (!nativeSession) {
        if (!document || sourcePath.empty()) {
            return std::nullopt;
        }
        return SearchPdfSource{sourcePath, page};
    }
    if (static_cast<size_t>(page) >= nativeSession->pdfPageMap.size()) {
        return std::nullopt;
    }

    const int sourcePage = nativeSession->pdfPageMap[static_cast<size_t>(page)];
    const auto path = nativeSession->backgroundPdfPath();
    std::error_code error;
    if (sourcePage < 0 || !std::filesystem::is_regular_file(path, error) || error) {
        return std::nullopt;
    }
    return SearchPdfSource{path.string(), sourcePage};
}

std::optional<SearchPdfSource> Prototype::searchLayerPdfSource(int page, size_t origin) const {
    if (!nativeSession || page < 0 || origin >= layerDocuments.size() || !layerDocuments[origin] ||
        origin >= nativeSession->layerPreviewPaths.size()) {
        return std::nullopt;
    }
    return SearchPdfSource{nativeSession->layerPreviewPaths[origin].string(), page};
}

std::shared_ptr<const void> Prototype::searchDocumentLifetime() const { return nativeSession; }

const void* Prototype::searchDocumentIdentity() const { return nativeSession.get(); }

GtkWidget* Prototype::searchCanvasWidget() const { return canvas; }

void Prototype::finishTextEditingForSearch() { finishTextEditing(); }

void Prototype::resetInputForSearch() { resetInput(); }

void Prototype::focusSearchMatch(const SearchMatch& match) {
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

void Prototype::updateSearchCanvas() { update(); }

void Prototype::openSearchBar() { searchController.open(); }

void Prototype::closeSearchBar() { searchController.close(); }

void Prototype::searchTextChanged() { searchController.refreshFromEntry(); }

void Prototype::searchNext() { searchController.next(); }

void Prototype::searchPrevious() { searchController.previous(); }

void Prototype::ensureSearchMatchesFresh() { searchController.ensureFresh(); }

}  // namespace prototype::gtk4
