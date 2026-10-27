#include "Prototype.h"

namespace prototype::gtk4 {

PageRasterRequest Prototype::canvasRenderRequest(int page, double scale) const {
    PageRasterRequest request;
    request.page = page;
    request.pageSize = state.pageSize(page);
    const int deviceScale = canvas ? gtk_widget_get_scale_factor(canvas) : 1;
    request.mediumDetailScale = canvasMediumDetailScale(page, deviceScale);
    request.lowDetailScale = canvasLodScale(page, deviceScale);
    request.renderScale = std::max(scale, request.mediumDetailScale);
    request.quality = PageRasterQuality::Full;
    request.audioMode = state.brush.tool == Tool::PlayObject;
    const auto& pageLayers = state.layersForPage(page);
    if (document && pageLayers.backgroundVisible) {
        request.backgroundPath = nativeSession ? nativeSession->previewPath().string() : sourcePath;
    }
    if (nativeSession) {
        const auto& paths =
                request.audioMode ? nativeSession->audioLayerPreviewPaths : nativeSession->layerPreviewPaths;
        const auto& documents = request.audioMode ? audioLayerDocuments : layerDocuments;
        for (const auto& layer: pageLayers.layers) {
            if (!layer.visible) {
                continue;
            }
            for (const size_t origin: layer.origins) {
                if (origin < documents.size() && origin < paths.size() &&
                    std::none_of(
                            request.layers.begin(), request.layers.end(),
                            [origin](const PageRasterLayerSource& source) { return source.origin == origin; })) {
                    request.layers.push_back({origin, paths[origin].string()});
                }
            }
        }
    }
    // shortcut: cap each page at 64 MiB; raise after measuring large native-layer stacks.
    const auto size = request.pageSize;
    const double pixelsPerSurface =
            static_cast<double>(64 * 1024 * 1024 / sizeof(uint32_t)) / (1 + request.layers.size());
    request.renderScale = std::min(request.renderScale, std::sqrt(pixelsPerSurface / (size.width * size.height)));
    return request;
}

double Prototype::canvasLodScale(int page, int deviceScale) const {
    return canvasCacheScale(page, deviceScale, 1.0, maxCanvasLowLodDimension);
}

double Prototype::canvasMediumDetailScale(int page, int deviceScale) const {
    return canvasCacheScale(page, deviceScale, 2.0, maxCanvasMediumLodDimension);
}

double Prototype::canvasCacheScale(int page, int deviceScale, double zoomMultiplier, int maxDimension) const {
    const auto size = state.pageSize(page);
    const double desiredScale = state.zoom * deviceScale * zoomMultiplier;
    const double roundedScale = std::max(0.125, std::round(desiredScale * 4) / 4);
    return std::min({roundedScale, maxDimension / size.width, maxDimension / size.height});
}

PageRasterRequest Prototype::canvasMediumDetailRequest(int page) const {
    const int deviceScale = canvas ? gtk_widget_get_scale_factor(canvas) : 1;
    auto request = canvasRenderRequest(page, canvasMediumDetailScale(page, deviceScale));
    request.renderScale = request.mediumDetailScale;
    request.quality = PageRasterQuality::MediumDetail;
    return request;
}

PageRasterRequest Prototype::canvasLodRequest(int page) const {
    const int deviceScale = canvas ? gtk_widget_get_scale_factor(canvas) : 1;
    auto request = canvasRenderRequest(page, canvasLodScale(page, deviceScale));
    request.renderScale = request.lowDetailScale;
    request.quality = PageRasterQuality::LowDetail;
    return request;
}

PageRasterRequest Prototype::canvasPreloadRequest(int page) const {
    switch (importedConfig.settings.preloadDetail) {
        case prototype::CanvasPreloadDetail::Medium:
            return canvasMediumDetailRequest(page);
        case prototype::CanvasPreloadDetail::Low:
            return canvasLodRequest(page);
        case prototype::CanvasPreloadDetail::Automatic:
            return canvasUsesMediumDetail() ? canvasMediumDetailRequest(page) : canvasLodRequest(page);
    }
    return canvasLodRequest(page);
}

void Prototype::applyCanvasCacheLimits() {
    const auto& settings = importedConfig.settings;
    constexpr size_t bytesPerMegabyte = 1024 * 1024;
    canvasRenderer.setCacheLimits(static_cast<size_t>(settings.fullDetailCachePages),
                                  static_cast<size_t>(settings.mediumDetailCachePages),
                                  static_cast<size_t>(settings.mediumDetailCacheMegabytes) * bytesPerMegabyte,
                                  static_cast<size_t>(settings.lowDetailCachePages),
                                  static_cast<size_t>(settings.lowDetailCacheMegabytes) * bytesPerMegabyte);
}

bool Prototype::canvasUsesMediumDetail() const {
    return state.zoom > importedConfig.settings.lowDetailZoomThresholdPercent / 100.0;
}

bool Prototype::canvasOverviewMode(size_t visiblePageCount) const {
    return state.zoom <= importedConfig.settings.overviewZoomThresholdPercent / 100.0 || visiblePageCount > 4;
}

bool Prototype::canvasOverviewMode() const {
    return canvas &&
           canvasOverviewMode(
                   state.visiblePageIndices(gtk_widget_get_width(canvas), gtk_widget_get_height(canvas)).size());
}

int Prototype::canvasDetailPage(const std::map<int, double>& visiblePages) const {
    if (state.selecting() && visiblePages.contains(state.page)) {
        return state.page;
    }
    if (pointerPosition) {
        if (const auto hit = state.hitPage(pointerPosition->x, pointerPosition->y);
            hit && visiblePages.contains(hit->page)) {
            return hit->page;
        }
    }
    if (visiblePages.contains(state.page)) {
        return state.page;
    }
    return std::min_element(visiblePages.begin(), visiblePages.end(),
                            [](const auto& left, const auto& right) { return left.second < right.second; })
            ->first;
}

double Prototype::canvasRenderScale(int page, int deviceScale) const {
    const auto size = state.pageSize(page);
    const auto scale = std::min({state.zoom * deviceScale, 4096.0 / size.width, 4096.0 / size.height});
    return canvasRenderRequest(page, scale).renderScale;
}

double Prototype::canvasDetailRenderScale(int page, int deviceScale, bool overview) const {
    const double scale = canvasRenderScale(page, deviceScale);
    return overview ? std::max(scale, canvasMediumDetailRequest(page).renderScale * 2) : scale;
}

bool Prototype::waitForCanvasRaster(int page, double scale, int timeoutMilliseconds) {
    const auto request = canvasRenderRequest(page, scale);
    canvasRenderer.setVisiblePages({{page, 0}}, page);
    canvasRenderer.setFullDetailPages({{page, 0}});
    canvasRenderer.setRetainedPages({{page, 0}}, {{page, 0}});
    const auto deadline = g_get_monotonic_time() + timeoutMilliseconds * 1000LL;
    while (g_get_monotonic_time() < deadline) {
        canvasRenderer.request(request);
        if (canvasRenderer.matches(request)) {
            canvasRenderer.find(page);
            return true;
        }
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    return false;
}

size_t Prototype::totalCacheBytes() const { return canvasRenderer.cacheBytes(); }

void Prototype::draw(cairo_t* cr, int width, int height) {
    if (!fitted || (fitMode && (width != lastWidth || height != lastHeight)) ||
        (presentationMode && presentationFitPage != state.page)) {
        input.cancel();
        fitCurrentView(width, height);
        fitted = true;
        fitMode = true;
    }
    lastWidth = width;
    lastHeight = height;
    const int deviceScale = gtk_widget_get_scale_factor(canvas);
    const auto theme = importedConfig.settings.themeVariant;
    const bool darkCanvas = options.explicitDark || theme == prototype::ThemeVariant::ForceDark ||
                            (theme == prototype::ThemeVariant::UseSystem &&
                             adw_style_manager_get_dark(adw_style_manager_get_default()));
    if (darkCanvas) {
        cairo_set_source_rgb(cr, 0.13, 0.15, 0.19);
    } else {
        cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
    }
    cairo_paint(cr);
    if (darkCanvas) {
        cairo_new_path(cr);
        for (int y = 12; y < height; y += 26) {
            for (int x = 12; x < width; x += 26) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, x, y, 1.25, 0, 2 * G_PI);
            }
        }
        cairo_set_source_rgba(cr, 0.25, 0.28, 0.33, 0.55);
        cairo_fill(cr);
    }
    if (sidebarCanvasRenderSuppressed) {
        return;
    }
    const auto visiblePageIndices = state.visiblePageIndices(width, height);
    std::map<int, double> visiblePages;
    for (const int page: visiblePageIndices) {
        const auto size = state.pageSize(page);
        const double centerY = state.offsetY + (state.pageTop(page) + size.height / 2.0) * state.zoom;
        visiblePages.emplace(page, std::abs(centerY - height / 2.0));
    }
    if (visiblePages.empty()) {
        visiblePages.emplace(state.page, 0);
    }
    inkRasterCache.beginFrame(visiblePageIndices);
    bool activeStrokeDrawn = false;
    if (!state.current) {
        inkRasterCache.clearActive();
    }
    const auto retainedPages = [this, width, height](double margin) {
        std::map<int, double> pages;
        for (const int page: state.visiblePageIndices(width, height, width * margin, height * margin)) {
            const auto size = state.pageSize(page);
            const double centerY = state.offsetY + (state.pageTop(page) + size.height / 2.0) * state.zoom;
            pages.emplace(page, std::abs(centerY - height / 2.0));
        }
        if (pages.empty()) {
            pages.emplace(state.page, 0);
        }
        return pages;
    };
    auto mediumRetainedPages = retainedPages(1.5);
    auto lowRetainedPages = retainedPages(6.0);
    for (const auto& [page, order]: canvasPreloadedPages) {
        mediumRetainedPages.try_emplace(page, height * 8.0 + order);
        lowRetainedPages.try_emplace(page, height * 8.0 + order);
    }
    canvasRenderer.setVisiblePages(visiblePages, state.page);
    const bool overview = canvasOverviewMode(visiblePageIndices.size());
    const int detailPage = canvasDetailPage(visiblePages);
    canvasRenderer.setFullDetailPages(overview ? std::map<int, double>{{detailPage, visiblePages.at(detailPage)}} :
                                                 visiblePages);
    canvasRenderer.setRetainedPages(std::move(mediumRetainedPages), std::move(lowRetainedPages));
    if (!zoomPreview) {
        if (overview) {
            const double scale = canvasDetailRenderScale(detailPage, deviceScale, true);
            canvasRenderer.request(canvasRenderRequest(detailPage, scale));
            for (const auto& [page, distance]: visiblePages) {
                if (page != detailPage) {
                    canvasRenderer.request(canvasUsesMediumDetail() ? canvasMediumDetailRequest(page) :
                                                                      canvasLodRequest(page));
                }
            }
        } else {
            for (const auto& [page, distance]: visiblePages) {
                const auto size = state.pageSize(page);
                const double scale = std::min({state.zoom * deviceScale, 4096.0 / size.width, 4096.0 / size.height});
                canvasRenderer.request(canvasRenderRequest(page, scale));
            }
        }
    }
    if (!zoomPreview) {
        for (const auto& preload: canvasPreloadedPages) {
            const int page = preload.first;
            if (visiblePages.contains(page)) {
                continue;
            }
            canvasRenderer.request(canvasPreloadRequest(page));
        }
    }
    for (const int i: visiblePageIndices) {
        const auto sz = state.pageSize(i);
        const double sx0 = state.offsetX + state.pageLeft(i) * state.zoom;
        const double sy0 = state.offsetY + state.pageTop(i) * state.zoom;
        const double sx1 = sx0 + sz.width * state.zoom;
        const double sy1 = sy0 + sz.height * state.zoom;
        if (sx1 <= 0 || sy1 <= 0 || sx0 >= width || sy0 >= height) {
            continue;
        }
        const auto* cached = canvasRenderer.find(i);
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, darkCanvas ? 0.45 : 0.18);
        cairo_rectangle(cr, sx0 - 0.5, sy0 - 0.5, (sx1 - sx0) + 1.0, (sy1 - sy0) + 1.0);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_restore(cr);

        cairo_save(cr);
        cairo_translate(cr, sx0, sy0);
        cairo_scale(cr, state.zoom, state.zoom);
        cairo_rectangle(cr, 0, 0, sz.width, sz.height);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        if (state.layerVisible(i, 0) && cached && cached->background) {
            cairo_save(cr);
            cairo_scale(cr, 1.0 / cached->scale, 1.0 / cached->scale);
            cairo_set_source_surface(cr, cached->background.get(), 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        enum class EditableKind { Ink, Text, Image };
        struct LayerItem {
            size_t order;
            EditableKind kind;
            size_t index;
        };
        const bool fadeEditableObjects = state.brush.tool == Tool::PlayObject;
        const auto& layerSources = fadeEditableObjects ? audioLayerDocuments : layerDocuments;
        const auto drawEditableObject = [&](auto&& drawObject) {
            if (!fadeEditableObjects) {
                drawObject();
                return;
            }
            cairo_push_group(cr);
            drawObject();
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, 0.3);
        };
        for (size_t layerIndex = 0; layerIndex < state.layersForPage(i).layers.size(); ++layerIndex) {
            const size_t layerId = layerIndex + 1;
            const auto& layer = state.layersForPage(i).layers[layerIndex];
            if (!layer.visible) {
                continue;
            }
            for (size_t origin: layer.origins) {
                if (origin < layerSources.size() && cached && cached->audioMode == fadeEditableObjects) {
                    const auto nativeLayer = cached->layers.find(origin);
                    if (nativeLayer != cached->layers.end()) {
                        cairo_save(cr);
                        cairo_scale(cr, 1.0 / cached->scale, 1.0 / cached->scale);
                        cairo_set_source_surface(cr, nativeLayer->second.get(), 0, 0);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                        cairo_paint(cr);
                        cairo_restore(cr);
                    }
                }
                std::vector<LayerItem> layerItems;
                layerItems.reserve(state.ink.size() + state.texts.size());
                for (size_t index = 0; index < state.ink.size(); ++index) {
                    if (state.displayPage(index) == i && state.displayLayerId(index) == layerId &&
                        state.displayLayerOrigin(index) == origin && !state.verticalSpaceAffectsInk(index)) {
                        layerItems.push_back({state.ink[index].layerOrder, EditableKind::Ink, index});
                    }
                }
                for (size_t index = 0; index < state.texts.size(); ++index) {
                    if (textEditor && textEditor->active() && textEditor->existingItemIndex() == index) {
                        continue;
                    }
                    if (state.verticalSpaceAffectsText(index)) {
                        continue;
                    }
                    const auto text = state.displayText(index);
                    if (text.page == i && text.layerId == layerId && text.originLayer == origin) {
                        layerItems.push_back({text.layerOrder, EditableKind::Text, index});
                    }
                }
                for (size_t index = 0; index < state.images.size(); ++index) {
                    if (state.verticalSpaceAffectsImage(index)) {
                        continue;
                    }
                    const auto image = displayedImage(index);
                    if (image.page == i && image.layerId == layerId && image.originLayer == origin) {
                        layerItems.push_back({image.layerOrder, EditableKind::Image, index});
                    }
                }
                std::stable_sort(layerItems.begin(), layerItems.end(),
                                 [](const LayerItem& a, const LayerItem& b) { return a.order < b.order; });
                for (size_t itemIndex = 0; itemIndex < layerItems.size();) {
                    const auto& item = layerItems[itemIndex];
                    if (item.kind == EditableKind::Text) {
                        const auto text = state.displayText(item.index);
                        drawEditableObject([&] { prototype::drawText(cr, text); });
                    } else if (item.kind == EditableKind::Image) {
                        drawEditableObject([&] { prototype::drawCanvasImage(cr, displayedImage(item.index)); });
                    } else {
                        const auto& first = state.ink[item.index];
                        const bool cacheableTool = first.brush.tool == Tool::Pen ||
                                                   first.brush.tool == Tool::Highlighter ||
                                                   first.brush.tool == Tool::Eraser;
                        const auto hasIdentityTransform = [this](size_t index) {
                            const auto transform = state.selectionMatrix(index);
                            return transform.identity() && std::abs(transform.widthScale - 1.0) < 1e-12;
                        };
                        const auto isRigidPreviewTransform = [](const prototype::SelectionTransform& transform) {
                            const double xScale = std::hypot(transform.xx, transform.yx);
                            const double yScale = std::hypot(transform.xy, transform.yy);
                            return std::abs(xScale - 1.0) < 1e-9 && std::abs(yScale - 1.0) < 1e-9 &&
                                   std::abs(transform.xx * transform.xy + transform.yx * transform.yy) < 1e-9 &&
                                   std::abs(transform.widthScale - 1.0) < 1e-9;
                        };
                        const auto sameTransform = [](const prototype::SelectionTransform& left,
                                                      const prototype::SelectionTransform& right) {
                            return left.xx == right.xx && left.yx == right.yx && left.xy == right.xy &&
                                   left.yy == right.yy && left.x0 == right.x0 && left.y0 == right.y0 &&
                                   left.widthScale == right.widthScale;
                        };
                        const bool canCacheRun =
                                !fadeEditableObjects && cacheableTool && hasIdentityTransform(item.index);
                        if (canCacheRun) {
                            const bool highlighter = first.brush.tool == Tool::Highlighter;
                            std::vector<const Ink*> strokeRun;
                            size_t runEnd = itemIndex;
                            while (runEnd < layerItems.size() && layerItems[runEnd].kind == EditableKind::Ink) {
                                const auto& candidate = state.ink[layerItems[runEnd].index];
                                const bool candidateCacheable = candidate.brush.tool == Tool::Pen ||
                                                                candidate.brush.tool == Tool::Highlighter ||
                                                                candidate.brush.tool == Tool::Eraser;
                                if (!candidateCacheable || (candidate.brush.tool == Tool::Highlighter) != highlighter ||
                                    !hasIdentityTransform(layerItems[runEnd].index)) {
                                    break;
                                }
                                strokeRun.push_back(&candidate);
                                ++runEnd;
                            }

                            const double rasterScale = zoomPreview ? deviceScale : state.zoom * deviceScale;
                            if (inkRasterCache.drawCommitted(cr, strokeRun, rasterScale)) {
                                itemIndex = runEnd;
                                continue;
                            }
                            for (const auto* stroke: strokeRun) {
                                prototype::drawInk(cr, *stroke);
                            }
                            itemIndex = runEnd;
                            continue;
                        }
                        const auto selectionTransform = state.selectionMatrix(item.index);
                        if (!fadeEditableObjects && cacheableTool && !selectionTransform.identity() &&
                            isRigidPreviewTransform(selectionTransform)) {
                            std::vector<const Ink*> strokeRun;
                            size_t runEnd = itemIndex;
                            while (runEnd < layerItems.size() && layerItems[runEnd].kind == EditableKind::Ink) {
                                const auto& candidate = state.ink[layerItems[runEnd].index];
                                const auto candidateTransform = state.selectionMatrix(layerItems[runEnd].index);
                                const bool candidateCacheable = candidate.brush.tool == Tool::Pen ||
                                                                candidate.brush.tool == Tool::Highlighter ||
                                                                candidate.brush.tool == Tool::Eraser;
                                if (!candidateCacheable ||
                                    (candidate.brush.tool == Tool::Highlighter) !=
                                            (first.brush.tool == Tool::Highlighter) ||
                                    !sameTransform(candidateTransform, selectionTransform) ||
                                    !isRigidPreviewTransform(candidateTransform)) {
                                    break;
                                }
                                strokeRun.push_back(&candidate);
                                ++runEnd;
                            }

                            cairo_matrix_t matrix;
                            cairo_matrix_init(&matrix, selectionTransform.xx, selectionTransform.yx,
                                              selectionTransform.xy, selectionTransform.yy, selectionTransform.x0,
                                              selectionTransform.y0);
                            const double rasterScale = zoomPreview ? deviceScale : state.zoom * deviceScale;
                            if (inkRasterCache.drawCommitted(cr, strokeRun, rasterScale, &matrix)) {
                                itemIndex = runEnd;
                                continue;
                            }
                            for (size_t selectedIndex = itemIndex; selectedIndex < runEnd; ++selectedIndex) {
                                const auto index = layerItems[selectedIndex].index;
                                prototype::drawInk(cr, state.ink[index], state.selectionMatrix(index));
                            }
                            itemIndex = runEnd;
                            continue;
                        }
                        drawEditableObject([&] { prototype::drawInk(cr, first, state.selectionMatrix(item.index)); });
                    }
                    ++itemIndex;
                }
                if (state.current && state.current->page == i && state.current->layerId == layerId &&
                    state.current->originLayer == origin) {
                    activeStrokeDrawn = true;
                    const double visibleX0 = std::max(0.0, -sx0 / state.zoom);
                    const double visibleY0 = std::max(0.0, -sy0 / state.zoom);
                    const double visibleX1 = std::min(sz.width, (width - sx0) / state.zoom);
                    const double visibleY1 = std::min(sz.height, (height - sy0) / state.zoom);
                    const bool appendOnly = state.current->brush.tool == Tool::Eraser ||
                                            state.drawingMode == prototype::DrawingMode::Freehand ||
                                            state.drawingMode == prototype::DrawingMode::ShapeRecognizer;
                    const double rasterScale = zoomPreview ? deviceScale : state.zoom * deviceScale;
                    if (!inkRasterCache.drawActive(cr, *state.current, rasterScale, visibleX0, visibleY0, visibleX1,
                                                   visibleY1, appendOnly)) {
                        prototype::drawInk(cr, *state.current);
                    }
                }
            }
        }
        prototype::drawLaserPointer(cr, state, i);
        prototype::drawVerticalSpacePreview(cr, state, i);
        if (!textEditor || !textEditor->active()) {
            prototype::drawSelection(cr, state, i);
            if (state.brush.tool != Tool::Hand && selectedImageIndex && *selectedImageIndex < state.images.size() &&
                state.images[*selectedImageIndex].page == i &&
                state.layerVisible(i, state.images[*selectedImageIndex].layerId)) {
                prototype::drawCanvasImageHandles(cr, displayedImage(*selectedImageIndex), state.zoom);
            }
        }
        if (pdfTextSelection && pdfTextSelection->page == i && pdfTextSelection->region) {
            const int rectangleCount = cairo_region_num_rectangles(pdfTextSelection->region.get());
            if (rectangleCount > 0) {
                cairo_save(cr);
                cairo_set_source_rgba(cr, 0.22, 0.50, 0.86, 0.30);
                for (int rectangleIndex = 0; rectangleIndex < rectangleCount; ++rectangleIndex) {
                    cairo_rectangle_int_t rectangle{};
                    cairo_region_get_rectangle(pdfTextSelection->region.get(), rectangleIndex, &rectangle);
                    cairo_rectangle(cr, rectangle.x, rectangle.y, rectangle.width, rectangle.height);
                }
                cairo_fill(cr);
                if (pdfTextSelection->dragging && pdfTextSelection->tool == Tool::SelectPdfTextRect) {
                    const auto& bounds = pdfTextSelection->bounds;
                    const auto frame = prototype::pdf_text_selection_detail::cairoRect(bounds);
                    cairo_set_source_rgba(cr, 0.16, 0.39, 0.68, 0.95);
                    cairo_set_line_width(cr, 1.0 / state.zoom);
                    cairo_rectangle(cr, frame.x, frame.y, frame.width, frame.height);
                    cairo_stroke(cr);
                }
                cairo_restore(cr);
            }
        }
        searchController.drawMatches(cr, i);
        cairo_restore(cr);
    }
    if (!activeStrokeDrawn) {
        inkRasterCache.clearActive();
    }
    inkRasterCache.endFrame();
    if (importedConfig.settings.highlightPosition && pointerPosition && pointerPosition->x >= 0 &&
        pointerPosition->y >= 0 && pointerPosition->x < width && pointerPosition->y < height) {
        const auto fill = unpackArgbColor(importedConfig.settings.cursorHighlightColor);
        cairo_save(cr);
        cairo_arc(cr, pointerPosition->x, pointerPosition->y, importedConfig.settings.cursorHighlightRadius, 0,
                  2 * G_PI);
        cairo_set_source_rgba(cr, fill.red, fill.green, fill.blue, fill.alpha);
        cairo_fill_preserve(cr);
        if (importedConfig.settings.cursorHighlightBorderWidth > 0) {
            const auto border = unpackArgbColor(importedConfig.settings.cursorHighlightBorderColor);
            cairo_set_source_rgba(cr, border.red, border.green, border.blue, border.alpha);
            cairo_set_line_width(cr, importedConfig.settings.cursorHighlightBorderWidth);
            cairo_stroke(cr);
        } else {
            cairo_new_path(cr);
        }
        cairo_restore(cr);
    }
    ++frames;
}

}  // namespace prototype::gtk4
