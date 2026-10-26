#include "Prototype.h"

namespace prototype::gtk4 {

bool Prototype::changePluginPageBackground(std::string_view format, std::string_view config, std::string& error) {
    if (format != "plain" && format != "graph") {
        error = "This GTK 4 host currently supports the plain and graph page backgrounds";
        return false;
    }
    if (!nativeSession || options.worker.empty()) {
        error = "Changing a page background requires the document worker";
        return false;
    }
    if (busy || choosingFile || closed) {
        error = "A document operation is already in progress";
        return false;
    }
    if (autosaving) {
        const std::string formatCopy(format);
        const std::string configCopy(config);
        waitForAutosave([this, formatCopy, configCopy] {
            std::string deferredError;
            if (!changePluginPageBackground(formatCopy, configCopy, deferredError)) {
                showError(deferredError);
            }
        });
        return true;
    }
    finishTextEditing();
    resetInput(true);
    if (!state.changePageBackground(state.page, std::string(format), std::string(config))) {
        return true;
    }
    return true;
}

prototype::PluginDocumentStructure Prototype::pluginDocumentStructure() const {
    prototype::PluginDocumentStructure structure;
    structure.currentPage = static_cast<std::size_t>(std::max(0, state.page)) + 1;
    structure.xoppFilename = saveTarget;
    if (nativeSession) {
        structure.pdfBackgroundFilename = nativeSession->pdfBackgroundFilename();
    }

    structure.pages.reserve(static_cast<std::size_t>(state.pageCount()));
    for (int pageIndex = 0; pageIndex < state.pageCount(); ++pageIndex) {
        const auto& pageLayers = state.layersForPage(pageIndex);
        prototype::PluginDocumentPage page;
        if (static_cast<std::size_t>(pageIndex) < state.pages.size()) {
            page.width = state.pages[static_cast<std::size_t>(pageIndex)].width;
            page.height = state.pages[static_cast<std::size_t>(pageIndex)].height;
        }
        page.pageTypeFormat = pageLayers.backgroundFormat.empty() ? "plain" : pageLayers.backgroundFormat;
        page.pageTypeConfig = pageLayers.backgroundConfig;
        page.backgroundColor = pageLayers.backgroundColor & 0xffffffU;
        page.currentLayer = pageLayers.selectedLayer;
        if (nativeSession && static_cast<std::size_t>(pageIndex) < nativeSession->pdfPageMap.size() &&
            nativeSession->pdfPageMap[static_cast<std::size_t>(pageIndex)] >= 0) {
            page.pdfBackgroundPageNo = nativeSession->pdfPageMap[static_cast<std::size_t>(pageIndex)] + 1;
        }

        page.layers.reserve(pageLayers.layers.size() + 1);
        page.layers.push_back(
                {pageLayers.backgroundVisible, pageLayers.backgroundName.value_or("Background"), std::nullopt});
        for (std::size_t layerIndex = 0; layerIndex < pageLayers.layers.size(); ++layerIndex) {
            const auto& canvasLayer = pageLayers.layers[layerIndex];
            const auto layerId = layerIndex + 1;
            const bool hasEditableContent =
                    std::any_of(state.ink.begin(), state.ink.end(),
                                [pageIndex, layerId](const auto& item) {
                                    return item.page == pageIndex && item.layerId == layerId;
                                }) ||
                    (state.current && state.current->page == pageIndex && state.current->layerId == layerId) ||
                    std::any_of(state.texts.begin(), state.texts.end(),
                                [pageIndex, layerId](const auto& item) {
                                    return item.page == pageIndex && item.layerId == layerId;
                                }) ||
                    std::any_of(state.images.begin(), state.images.end(), [pageIndex, layerId](const auto& item) {
                        return item.page == pageIndex && item.layerId == layerId;
                    });
            const bool annotated = canvasLayer.hasNonEditableElements || hasEditableContent;
            page.isAnnotated = page.isAnnotated || annotated;
            page.layers.push_back({canvasLayer.visible, canvasLayer.name, annotated});
        }
        structure.pages.push_back(std::move(page));
    }
    return structure;
}

void Prototype::refreshNativeDocumentRevision(int changedPage) {
    if (!nativeSession || options.worker.empty() || closed || changedPage < 0 || changedPage >= state.pageCount()) {
        return;
    }
    if (autosaving) {
        waitForAutosave([this, changedPage] { refreshNativeDocumentRevision(changedPage); });
        return;
    }
    if (busy) {
        showError("Cannot update the page background while another document operation is running");
        return;
    }

    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto packets = prototype::bridge::transferInk(state.committedInk());
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    runDocumentJob(
            "Updating page background…",
            [session, worker, packets, texts, images, layers, pageSizes, staged] {
                *staged = session->stageRevision(worker, packets, texts, images, layers, pageSizes);
            },
            [this, session, staged] {
                PopplerDocument* candidate = nullptr;
                std::vector<PopplerDocument*> candidateLayers;
                std::vector<PopplerDocument*> candidateAudioLayers;
                std::vector<prototype::PageSize> sizes;
                prototype::bridge::LayerDocument layerState;
                try {
                    std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                    candidateLayers = openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                    candidateAudioLayers =
                            openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(), static_cast<int>(sizes.size()));
                    layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                } catch (...) {
                    if (candidate) {
                        g_object_unref(candidate);
                    }
                    releaseLayerPreviewPdfs(candidateLayers);
                    releaseLayerPreviewPdfs(candidateAudioLayers);
                    prototype::bridge::DocumentSession::discardRevision(*staged);
                    throw;
                }

                session->commitRevision(*staged);
                g_clear_object(&document);
                document = candidate;
                releaseLayerPreviewPdfs(layerDocuments);
                layerDocuments = std::move(candidateLayers);
                releaseLayerPreviewPdfs(audioLayerDocuments);
                audioLayerDocuments = std::move(candidateAudioLayers);
                audioTargets = session->audioTargets;
                state.setLayerDocument(layerState);
                g_clear_object(&pdfTextDocument);
                pdfTextDocumentPath.clear();
                canvasRenderer.invalidate();
                thumbnailRenderer.invalidate();
                thumbnailWidgets.clear();
                pageList = nullptr;
                adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                updateTitle();
            });
}

int Prototype::pluginDisplayDpi() const {
    constexpr int defaultDpi = 72;
    auto* native = canvas ? gtk_widget_get_native(canvas) : nullptr;
    auto* surface = native ? gtk_native_get_surface(native) : nullptr;
    auto* display = canvas ? gtk_widget_get_display(canvas) : nullptr;
    if (!surface || !display) {
        return defaultDpi;
    }
    auto* monitor = gdk_display_get_monitor_at_surface(display, surface);
    if (!monitor) {
        return defaultDpi;
    }
    GdkRectangle geometry{};
    gdk_monitor_get_geometry(monitor, &geometry);
    const int heightMillimeters = gdk_monitor_get_height_mm(monitor);
    if (heightMillimeters <= 0 || geometry.height <= 0) {
        return defaultDpi;
    }
    const double detectedDpi = static_cast<double>(geometry.height) * 25.4 / heightMillimeters;
    return static_cast<int>(std::lround(std::clamp(detectedDpi, 36.0, 720.0)));
}

bool Prototype::getPluginPageLabel(std::size_t pageNumber, std::string& label, std::string& error) const {
    if (!nativeSession || pageNumber >= nativeSession->pdfPageLabels.size()) {
        error = "page nr " + std::to_string(pageNumber) + " is out of range";
        return false;
    }
    label = nativeSession->pdfPageLabels[pageNumber];
    return true;
}

std::vector<prototype::PluginPaletteColor> Prototype::getPluginColorPalette() const {
    std::vector<prototype::PluginPaletteColor> palette;
    palette.reserve(importedConfig.palette.swatches.size());
    const auto channelValue = [](double channel) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(channel, 0.0, 1.0) * 255.0));
    };
    for (const auto& swatch: importedConfig.palette.swatches) {
        const auto red = channelValue(swatch.color[0]);
        const auto green = channelValue(swatch.color[1]);
        const auto blue = channelValue(swatch.color[2]);
        palette.push_back({swatch.name, (red << 16) | (green << 8) | blue});
    }
    return palette;
}

bool Prototype::refreshPluginPage(std::string& error) {
    if (state.page < 0 || state.page >= state.pageCount()) {
        error = "Called applib_refreshPage, but there is no current page.";
        return false;
    }
    canvasRenderer.invalidate();
    thumbnailRenderer.invalidate();
    update();
    return true;
}

bool Prototype::setPluginFont(const prototype::PluginFont& font, std::string& error) {
    if (busy || choosingFile || messageDialog || closed) {
        error = "Wait for the current document operation to finish before changing the text font";
        return false;
    }
    if (font.name.empty() || !std::isfinite(font.size) || font.size <= 0) {
        error = "The requested text font is invalid";
        return false;
    }

    state.textFontName = font.name;
    state.textFontSize = font.size;

    if (textEditor && textEditor->active()) {
        textEditor->setBaseFont(font.name, font.size);
        updateTextEditorPosition();
    } else if (!state.selectedTexts.empty()) {
        if (state.updateSelectedTexts([&](prototype::TextItem& item) {
                item.fontName = font.name;
                item.fontSize = font.size;
                const auto [width, height] = prototype::textNaturalSize(item);
                item.layoutWidth = width;
                item.layoutHeight = height;
            })) {
            update();
        }
    }

    if (textFontEntry && gtk_editable_get_text(GTK_EDITABLE(textFontEntry)) != font.name) {
        gtk_editable_set_text(GTK_EDITABLE(textFontEntry), font.name.c_str());
    }
    if (textFontSize) {
        const double minimum = std::min(1.0, font.size);
        const double maximum = std::max(512.0, font.size);
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(textFontSize), minimum, maximum);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), font.size);
    }
    update();
    return true;
}

prototype::PluginScrollPosition Prototype::getPluginScrollPosition() const {
    const double width = canvas ? static_cast<double>(gtk_widget_get_width(canvas)) : 0.0;
    const double height = canvas ? static_cast<double>(gtk_widget_get_height(canvas)) : 0.0;
    const double originX = std::max(0.0, (width - state.layoutWidth() * state.zoom) / 2.0);
    const double originY = std::max(0.0, (height - state.layoutHeight() * state.zoom) / 2.0);
    return {
            std::max(0.0, originX - state.offsetX),
            std::max(0.0, originY - state.offsetY),
            width,
            height,
    };
}

bool Prototype::setPluginZoom(double value, std::string& error) {
    if (presentationMode) {
        error = "Zoom is calculated per page while Presentation Mode is active";
        return false;
    }
    if (busy || choosingFile || messageDialog || closed || autosaving || input.active()) {
        error = "Wait for the current document operation or input gesture to finish before changing zoom";
        return false;
    }
    if (!std::isfinite(value)) {
        error = "Zoom must be a finite number";
        return false;
    }
    const int width = canvas ? gtk_widget_get_width(canvas) : 0;
    const int height = canvas ? gtk_widget_get_height(canvas) : 0;
    if (width <= 0 || height <= 0) {
        error = "The canvas viewport is not ready for zoom changes";
        return false;
    }
    zoom(std::clamp(value, 0.1, 8.0), width / 2.0, height / 2.0);
    return true;
}

bool Prototype::setPluginCurrentPage(std::size_t pageNumber, std::string& error) {
    if (busy || choosingFile || messageDialog || closed) {
        error = "Wait for the current document operation to finish before selecting a page";
        return false;
    }
    const auto pageCount = state.pageCount();
    if (pageCount < 1) {
        error = "There are no pages to select";
        return false;
    }
    const auto targetPage = std::clamp(pageNumber, std::size_t{1}, static_cast<std::size_t>(pageCount)) - 1;
    resetInput();
    if (static_cast<int>(targetPage) != state.page) {
        selectedImageIndex.reset();
    }
    state.selectActivePage(static_cast<int>(targetPage));
    update();
    return true;
}

bool Prototype::scrollPluginToPage(int pageNumber, bool relative, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving || input.active()) {
        error = "Wait for the current document operation or input gesture to finish before scrolling to a page";
        return false;
    }
    const int width = canvas ? gtk_widget_get_width(canvas) : 0;
    const int height = canvas ? gtk_widget_get_height(canvas) : 0;
    if (width <= 0 || height <= 0) {
        error = "The canvas viewport is not ready for page scrolling";
        return false;
    }

    const auto requestedPage =
            relative ? static_cast<std::int64_t>(state.page) + pageNumber : static_cast<std::int64_t>(pageNumber) - 1;
    const auto targetPage = static_cast<int>(
            std::clamp(requestedPage, std::int64_t{0}, static_cast<std::int64_t>(state.pageCount() - 1)));
    resetInput();
    if (targetPage != state.page) {
        selectedImageIndex.reset();
    }
    state.cancel();
    const auto targetSize = state.pageSize(targetPage);
    const double pageLeft = state.pageLeft(targetPage) * state.zoom;
    const double pageTop = state.pageTop(targetPage) * state.zoom;
    const double pageWidth = targetSize.width * state.zoom;
    const double pageHeight = targetSize.height * state.zoom;
    const auto keepAxisVisible = [](double pageStart, double pageExtent, double viewportExtent, double leadingMargin,
                                    double trailingMargin, double& offset) {
        const double visibleStart = offset + pageStart - leadingMargin;
        const double visibleEnd = offset + pageStart + pageExtent + trailingMargin;
        if (visibleStart < 0) {
            offset -= visibleStart;
        } else if (visibleEnd > viewportExtent) {
            offset -= visibleEnd - viewportExtent;
        }
    };
    keepAxisVisible(pageLeft, pageWidth, width, 5, 10, state.offsetX);
    keepAxisVisible(pageTop, pageHeight, height, 5, 10, state.offsetY);
    state.selectActivePage(targetPage);
    fitMode = false;
    fitted = false;
    update();
    return true;
}

bool Prototype::scrollPluginToPosition(double x, double y, bool relative, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving || input.active()) {
        error = "Wait for the current document operation or input gesture to finish before scrolling";
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        error = "Scroll positions must be finite numbers";
        return false;
    }
    const int width = canvas ? gtk_widget_get_width(canvas) : 0;
    const int height = canvas ? gtk_widget_get_height(canvas) : 0;
    if (width <= 0 || height <= 0) {
        error = "The canvas viewport is not ready for scrolling";
        return false;
    }

    resetInput();
    const int previousPage = state.page;
    const auto currentScrollPosition = getPluginScrollPosition();
    const double originX = std::max(0.0, (width - state.layoutWidth() * state.zoom) / 2.0);
    const double originY = std::max(0.0, (height - state.layoutHeight() * state.zoom) / 2.0);
    const double targetX = std::max(0.0, relative ? currentScrollPosition.x + x : x);
    const double targetY = std::max(0.0, relative ? currentScrollPosition.y + y : y);
    if (relative) {
        state.offsetX += currentScrollPosition.x - targetX;
        state.offsetY += currentScrollPosition.y - targetY;
    } else {
        state.offsetX = originX - targetX;
        state.offsetY = originY - targetY;
    }
    state.syncActivePage(width, height);
    if (state.page != previousPage) {
        selectedImageIndex.reset();
    }
    fitMode = false;
    fitted = false;
    update();
    return true;
}

bool Prototype::setPluginCurrentLayer(std::size_t layerNumber, bool updateVisibility, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving || input.active()) {
        error = "Wait for the current document operation or input gesture to finish before selecting a layer";
        return false;
    }
    if (state.page < 0 || static_cast<std::size_t>(state.page) >= state.pageLayers.size()) {
        error = "No page!";
        return false;
    }

    auto& pageLayers = state.pageLayers[static_cast<std::size_t>(state.page)];
    if (layerNumber > pageLayers.layers.size()) {
        error = "No layer with layer ID " + std::to_string(static_cast<std::int64_t>(layerNumber));
        return false;
    }

    finishTextEditing();
    resetInput(true);
    const auto selectedLayer = std::max<std::size_t>(1, layerNumber);
    if (!state.selectLayer(state.page, selectedLayer)) {
        error = "No layer with layer ID " + std::to_string(static_cast<std::int64_t>(layerNumber));
        return false;
    }
    selectedImageIndex.reset();

    if (updateVisibility) {
        for (std::size_t layerId = 1; layerId <= pageLayers.layers.size(); ++layerId) {
            pageLayers.layers[layerId - 1].visible = layerId <= layerNumber;
        }
    }

    if (updateVisibility) {
        thumbnailRenderer.invalidate();
    }
    rebuildLayerStack();
    update();
    return true;
}

bool Prototype::setPluginPageSize(double width, double height, bool relative, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before changing page size";
        return false;
    }
    const auto current = state.pageSize(state.page);
    if (relative) {
        width += current.width;
        height += current.height;
    }
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
        error = "Page dimensions must be finite and greater than zero";
        return false;
    }
    if (!state.resizePage(state.page, width, height) &&
        (state.pageSize(state.page).width != width || state.pageSize(state.page).height != height)) {
        error = "The current page size could not be changed";
        return false;
    }
    update();
    return true;
}

bool Prototype::setPluginLayerVisibility(bool visible, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before changing layer visibility";
        return false;
    }
    if (state.page < 0 || static_cast<size_t>(state.page) >= state.pageLayers.size()) {
        error = "There is no current layer";
        return false;
    }
    state.setLayerVisible(state.page, state.activeLayerId(state.page), visible);
    update();
    return true;
}

bool Prototype::setPluginCurrentLayerName(std::string_view name, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before renaming the layer";
        return false;
    }
    if (!state.renameLayer(state.page, state.activeLayerId(state.page), std::string(name))) {
        const auto& layers = state.layersForPage(state.page).layers;
        const auto layer = state.activeLayerId(state.page);
        if (layer == 0 || layer > layers.size()) {
            error = "There is no current layer to rename";
            return false;
        }
    }
    update();
    return true;
}

bool Prototype::setPluginBackgroundName(std::string_view name, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before renaming the background";
        return false;
    }
    if (!state.setBackgroundName(state.page, std::string(name))) {
        error = "No page!";
        return false;
    }
    rebuildLayerStack();
    update();
    return true;
}

bool Prototype::changePdfBackgroundPageFromPlugin(int pdfPage, std::string& error) {
    if (!nativeSession || options.worker.empty()) {
        error = "Beamer animation navigation requires a native document and document worker";
        return false;
    }
    if (busy || choosingFile || messageDialog || closed) {
        error = "A document operation is already in progress";
        return false;
    }
    if (autosaving) {
        waitForAutosave([this, pdfPage] {
            std::string deferredError;
            if (!changePdfBackgroundPageFromPlugin(pdfPage, deferredError)) {
                showError(deferredError);
            }
        });
        return true;
    }
    if (pdfPage < 0 || static_cast<size_t>(pdfPage) >= nativeSession->pdfPageLabels.size()) {
        error = "The requested Beamer PDF page is unavailable";
        return false;
    }

    finishTextEditing();
    resetInput(true);
    const int documentPage = state.page;
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto packets = prototype::bridge::transferInk(state.committedInk());
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    runDocumentJob(
            "Advancing Beamer animation…",
            [session, worker, documentPage, pdfPage, packets, texts, images, layers, pageSizes, staged] {
                *staged = session->stageSetPdfPageBackground(worker, documentPage, pdfPage, packets, texts, images,
                                                             layers, pageSizes);
            },
            [this, session, documentPage, staged] {
                PopplerDocument* candidate = nullptr;
                std::vector<PopplerDocument*> candidateLayers;
                std::vector<PopplerDocument*> candidateAudioLayers;
                std::vector<prototype::PageSize> sizes;
                prototype::bridge::LayerDocument layerState;
                try {
                    std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                    candidateLayers = openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                    candidateAudioLayers =
                            openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(), static_cast<int>(sizes.size()));
                    layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                } catch (...) {
                    if (candidate) {
                        g_object_unref(candidate);
                    }
                    releaseLayerPreviewPdfs(candidateLayers);
                    releaseLayerPreviewPdfs(candidateAudioLayers);
                    prototype::bridge::DocumentSession::discardRevision(*staged);
                    throw;
                }

                session->commitRevision(*staged);
                g_clear_object(&document);
                document = candidate;
                releaseLayerPreviewPdfs(layerDocuments);
                layerDocuments = std::move(candidateLayers);
                releaseLayerPreviewPdfs(audioLayerDocuments);
                audioLayerDocuments = std::move(candidateAudioLayers);
                audioTargets = session->audioTargets;
                state.setPages(std::move(sizes));
                state.selectActivePage(documentPage);
                state.setLayerDocument(layerState);
                savedRevision.reset();
                savedNormally = false;
                savedCopyPath.clear();
                g_clear_object(&pdfTextDocument);
                pdfTextDocumentPath.clear();
                canvasRenderer.invalidate();
                thumbnailRenderer.invalidate();
                thumbnailWidgets.clear();
                pageList = nullptr;
                adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                navigateToPage(documentPage);
                updateTitle();
            });
    return true;
}

}  // namespace prototype::gtk4
