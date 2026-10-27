#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::verifyExportPlugin() {
    const int pageCountBefore = pageCount;
    require(nativeSession && pageCountBefore == 2,
            "Export plugin verification expects the original two-page document fixture");
    const auto source = std::filesystem::path(options.smokeDirectory) / "export-plugin-source.xopp";
    const auto oldSaveTarget = saveTarget;
    saveTarget = source.string();

    prototype::Ink sample;
    sample.page = 0;
    sample.layerId = state.activeLayerId(0);
    sample.originLayer = state.activeLayerOrigin(0);
    sample.layerOrder = 0;
    sample.brush = {Tool::Pen, {1, 0, 0}, 8};
    sample.points = {{120, 150, 1}, {220, 150, 1}};
    state.ink.push_back(sample);

    for (size_t action = 0; action < 3; ++action) {
        require(pluginRuntime->invokeMenu("Export", action),
                "Bundled Export menu action did not dispatch through the GTK 4 Lua host");
        waitForPluginDocumentJob();
    }

    const auto stem = source.parent_path() / source.stem();
    auto pdfPath = stem;
    pdfPath += "_export.pdf";
    auto [exportedPdf, sizes] = openPreviewPdf(pdfPath.string());
    require(sizes.size() == 2 && std::filesystem::file_size(pdfPath) > 1000,
            "Export plugin did not produce the expected two-page PDF");
    g_object_unref(exportedPdf);

    for (int page = 1; page <= 2; ++page) {
        auto svgPath = stem;
        svgPath += "_export-" + std::to_string(page) + ".svg";
        require(std::filesystem::is_regular_file(svgPath) && std::filesystem::file_size(svgPath) > 500,
                "Export plugin did not produce one SVG file per page");

        auto pngPath = stem;
        pngPath += "_export-" + std::to_string(page) + ".png";
        auto* surface = cairo_image_surface_create_from_png(pngPath.c_str());
        require(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS,
                "Export plugin did not produce a readable PNG page");
        const int width = cairo_image_surface_get_width(surface);
        const int height = cairo_image_surface_get_height(surface);
        require(width > 1000 && height > 1000, "Default PNG export did not use the master 300-DPI output size");
        if (page == 1) {
            cairo_surface_flush(surface);
            const auto* pixels = cairo_image_surface_get_data(surface);
            const int stride = cairo_image_surface_get_stride(surface);
            size_t redPixels = 0;
            const int x0 = static_cast<int>(120.0 * 300.0 / 72.0);
            const int x1 = static_cast<int>(220.0 * 300.0 / 72.0);
            const int y = static_cast<int>(150.0 * 300.0 / 72.0);
            for (int x = x0; x < x1; ++x) {
                const auto* pixel = pixels + y * stride + x * 4;
                if (pixel[2] > 180 && pixel[1] < 100 && pixel[0] < 100) {
                    ++redPixels;
                }
            }
            require(redPixels > 20, "Export plugin did not include unsaved GTK 4 ink in its PNG output");
        }
        cairo_surface_destroy(surface);
    }

    state.ink.pop_back();
    saveTarget = oldSaveTarget;
    update();
}

void Prototype::verifyFitToContentPlugin() {
    const int originalPage = state.page;
    const auto originalInk = state.ink;
    const auto originalSelection = state.selected;
    const auto originalTextSelection = state.selectedTexts;
    const auto originalImageSelection = state.selectedImages;
    const auto originalRevision = state.revision();
    require(nativeSession && !nativeSession->pdfPageMap.empty(),
            "FitToContent verification requires a native multi-page document");
    const auto blankPage = std::find(nativeSession->pdfPageMap.begin(), nativeSession->pdfPageMap.end(), -1);
    require(blankPage != nativeSession->pdfPageMap.end(),
            "FitToContent verification requires an inserted blank note page");
    const int page = static_cast<int>(std::distance(nativeSession->pdfPageMap.begin(), blankPage));
    navigateToPage(page);
    require(state.page == page, "FitToContent verification could not select the inserted note page");
    const auto originalSize = state.pageSize(page);
    const auto originalLayer = state.activeLayerId(page);
    require(originalLayer > 0, "FitToContent verification requires a foreground layer");
    const auto originalLayerName = state.layersForPage(page).layers.at(originalLayer - 1).name;
    require(state.layerVisible(page, originalLayer), "FitToContent verification requires a visible foreground layer");

    state.ink.clear();
    state.selected.clear();
    state.selectedTexts.clear();
    state.selectedImages.clear();
    prototype::Ink content;
    content.page = page;
    content.layerId = originalLayer;
    content.originLayer = state.activeLayerOrigin(page);
    content.layerOrder = 0;
    content.brush = {Tool::Pen, {0.15, 0.35, 0.75}, 4};
    content.points = {{100, 120, 1}, {220, 180, 1}};
    state.ink.push_back(content);

    require(pluginRuntime->invokeMenu("FitToContent", 0),
            "FitToContent layer action did not execute through the plugin menu");
    const auto fitted = state.pageSize(page);
    require(std::abs(fitted.width - 124) < 1e-8 && std::abs(fitted.height - 64) < 1e-8,
            "FitToContent did not account for stroke width when resizing the page");
    capture("fit-to-content-page");
    const auto& layers = state.layersForPage(page);
    require(layers.layers.size() >= originalLayer + 1 && layers.layers[originalLayer - 1].name == originalLayerName &&
                    !layers.layers[originalLayer - 1].visible && state.activeLayerId(page) == originalLayer + 1 &&
                    layers.layers[originalLayer].name == "moved for fitting" && layers.layers[originalLayer].visible,
            "FitToContent did not hide the source layer and name/select its moved-content layer");
    require(state.ink.size() == 2 && state.ink[0].points.front().x == 100 &&
                    std::abs(state.ink[1].points.front().x - 2) < 1e-8 &&
                    std::abs(state.ink[1].points.front().y - 2) < 1e-8 &&
                    std::abs(state.ink[1].points.back().x - 122) < 1e-8 &&
                    std::abs(state.ink[1].points.back().y - 62) < 1e-8 && state.ink[1].layerId == originalLayer + 1,
            "FitToContent did not move a copy of the layer strokes into fitted coordinates");

    const auto roundTripPath =
            (std::filesystem::path(options.smokeDirectory) / "fit-to-content-round-trip.xopp").string();
    const auto packets = prototype::bridge::transferInk(state.committedInk());
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto pageLayers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    nativeSession->saveCopy(options.worker, roundTripPath, packets, texts, images, pageLayers, pageSizes);
    const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
    require(reopened.initialPageSizes.size() == state.pages.size() &&
                    std::abs(reopened.initialPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                    std::abs(reopened.initialPageSizes[static_cast<size_t>(page)].height - fitted.height) < 1e-8 &&
                    reopened.initialInk.size() == 2,
            "Saving and reopening FitToContent did not preserve fitted page dimensions and strokes");

    const auto nativeSavePath =
            (std::filesystem::path(options.smokeDirectory) / "fit-to-content-native-save.xopp").string();
    nativeSession->save(options.worker, nativeSavePath, packets, texts, images, pageLayers, pageSizes,
                        prototype::bridge::documentVersion(nativeSavePath).token);
    const prototype::bridge::DocumentSession reopenedNativeSave(options.worker, nativeSavePath);
    require(std::abs(reopenedNativeSave.initialPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                    std::abs(reopenedNativeSave.initialPageSizes[static_cast<size_t>(page)].height - fitted.height) <
                            1e-8,
            "Normal Save did not persist the fitted page dimensions");

    const auto autosavePath = (std::filesystem::path(options.smokeDirectory) / "fit-to-content.autosave.xopp").string();
    nativeSession->autosave(options.worker, autosavePath, packets, texts, images, pageLayers, pageSizes);
    const prototype::bridge::DocumentSession reopenedAutosave(options.worker, autosavePath);
    require(std::abs(reopenedAutosave.initialPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                    std::abs(reopenedAutosave.initialPageSizes[static_cast<size_t>(page)].height - fitted.height) <
                            1e-8,
            "Autosave did not persist the fitted page dimensions");

    const auto exportPath = (std::filesystem::path(options.smokeDirectory) / "fit-to-content.pdf").string();
    nativeSession->exportPdf(options.worker, exportPath, packets, texts, images, pageLayers, pageSizes);
    auto [exportedPdf, exportedSizes] = openPreviewPdf(exportPath);
    require(exportedSizes.size() == state.pages.size() &&
                    std::abs(exportedSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                    std::abs(exportedSizes[static_cast<size_t>(page)].height - fitted.height) < 1e-8,
            "PDF export did not use the fitted page dimensions");
    g_object_unref(exportedPdf);

    const auto insertedRevision = nativeSession->stageInsertPageAfter(options.worker, page, pageSizes);
    const auto insertedPageSizes = prototype::bridge::readPageSizeTransfer(insertedRevision.pageSizesPath());
    prototype::bridge::DocumentSession::discardRevision(insertedRevision);
    require(insertedPageSizes.size() == state.pages.size() + 1 &&
                    std::abs(insertedPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                    std::abs(insertedPageSizes[static_cast<size_t>(page)].height - fitted.height) < 1e-8 &&
                    std::abs(insertedPageSizes[static_cast<size_t>(page + 1)].width - fitted.width) < 1e-8 &&
                    std::abs(insertedPageSizes[static_cast<size_t>(page + 1)].height - fitted.height) < 1e-8,
            "Page insertion did not preserve the fitted page and inherit its dimensions");

    for (int edit = 0; edit < 5; ++edit) {
        require(state.canUndo(), "FitToContent lost one of its five grouped undo steps");
        state.undo();
    }
    require(state.revision() == originalRevision && state.pageSize(page).width == originalSize.width &&
                    state.pageSize(page).height == originalSize.height && state.ink.size() == 1 &&
                    state.layersForPage(page).layers[originalLayer - 1].visible,
            "Undo did not restore the document state from before FitToContent");
    for (int edit = 0; edit < 5; ++edit) {
        require(state.canRedo(), "FitToContent undo history could not be replayed");
        state.redo();
    }
    require(std::abs(state.pageSize(page).width - fitted.width) < 1e-8 && state.ink.size() == 2,
            "Redo did not restore FitToContent's page size and moved stroke");
    for (int edit = 0; edit < 5; ++edit) {
        state.undo();
    }

    state.selected = {0};
    require(pluginRuntime->invokeMenu("FitToContent", 1),
            "FitToContent selection action did not execute through the plugin menu");
    require(std::abs(state.pageSize(page).width - fitted.width) < 1e-8 &&
                    std::abs(state.pageSize(page).height - fitted.height) < 1e-8 && state.ink.size() == 2,
            "FitToContent selection action did not fit the selected stroke");
    for (int edit = 0; edit < 5; ++edit) {
        require(state.canUndo(), "FitToContent selection action lost one of its undo steps");
        state.undo();
    }
    require(state.revision() == originalRevision && state.pageSize(page).width == originalSize.width &&
                    state.pageSize(page).height == originalSize.height && state.ink.size() == 1,
            "Undo did not restore the document state from before the selection fit");

    state.ink = originalInk;
    state.selected = originalSelection;
    state.selectedTexts = originalTextSelection;
    state.selectedImages = originalImageSelection;
    navigateToPage(originalPage);
    update();
}

void Prototype::verifyBeamerIntegration() {
    require(pluginRuntime != nullptr, "Lua plugin runtime was not created");
    const auto beamer = pluginRuntime->findPlugin("BeamerPresentation");
    require(beamer && beamer->loaded && beamer->menuItems == 3,
            "Enabled BeamerPresentation did not register its three menu actions");
    require(nativeSession && pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
            "Beamer integration expects a two-page animation PDF");
    require(nativeSession->pdfPageLabels.size() >= 2 &&
                    nativeSession->pdfPageLabels[0] == nativeSession->pdfPageLabels[1],
            "Beamer fixture must use repeated PDF page labels");

    capture("beamer-animation-start");
    require(pluginRuntime->invokeMenu("BeamerPresentation", 0), "Beamer +1 did not dispatch its next-animation action");
    waitForPluginDocumentJob();
    require(pageCount == 2 && state.page == 0 && nativeSession->pdfPageMap == std::vector<int>({1, 1}),
            "Beamer +1 did not advance only the current PDF background during an animation");
    capture("beamer-animation-forward");

    require(pluginRuntime->invokeMenu("BeamerPresentation", 1),
            "Beamer -1 did not dispatch its previous-animation action");
    waitForPluginDocumentJob();
    require(pageCount == 2 && state.page == 0 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
            "Beamer -1 did not restore the previous PDF animation background");

    require(pluginRuntime->invokeMenu("BeamerPresentation", 2),
            "Beamer clean did not dispatch its animation cleanup action");
    waitForPluginDocumentJob();
    require(pageCount == 1 && nativeSession->pdfPageMap == std::vector<int>({0}),
            "Beamer clean did not remove the duplicate-label animation page");
    capture("beamer-animation-cleaned");

    const auto roundTripPath = std::filesystem::path(options.smokeDirectory) / "beamer-cleaned.xopp";
    nativeSession->saveCopy(options.worker, roundTripPath.string(),
                            prototype::bridge::transferInk(state.committedInk()), state.texts,
                            prototype::bridge::transferImages(state.images), state.pageLayers, currentPageSizes(state));
    const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath.string());
    require(reopened.pdfPageMap == std::vector<int>({0}) && reopened.pdfPageLabels.size() >= 2 &&
                    reopened.pdfPageLabels[0] == reopened.pdfPageLabels[1],
            "Saving and reopening the cleaned Beamer document lost its page or attached-PDF labels");
    std::cout << "PASS: private GTK4 Beamer animation advance, reverse, cleanup, and native reopen\n";
}

void Prototype::verifyLayerActionsIntegration() {
    require(pluginRuntime != nullptr, "Lua plugin runtime was not created");
    const auto layerActions = pluginRuntime->findPlugin("LayerActions");
    require(layerActions && layerActions->loaded && layerActions->menuItems == 3,
            "Enabled LayerActions plugin did not register all three menu actions");
    require(nativeSession && pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
            "LayerActions integration expects a two-page PDF document");

    for (int page = 0; page < pageCount; ++page) {
        state.selectActivePage(page);
        require(state.addLayerAboveCurrent() && state.addLayerAboveCurrent(),
                "LayerActions fixture could not create three foreground layers per page");
    }
    state.setLayerVisible(0, 0, false);
    state.setLayerVisible(0, 2, false);
    state.selectActivePage(0);
    navigateToPage(0);

    Ink sourceStroke;
    sourceStroke.page = 0;
    sourceStroke.layerId = 3;
    sourceStroke.originLayer = 2;
    sourceStroke.layerOrder = 0;
    sourceStroke.brush = {Tool::Pen, {1, 0, 0}, 3};
    sourceStroke.points = {{35, 45, 1}, {90, 100, 1}};
    Ink nextPageAnnotation = sourceStroke;
    nextPageAnnotation.page = 1;
    nextPageAnnotation.layerId = 2;
    nextPageAnnotation.originLayer = 1;
    nextPageAnnotation.brush.color = {0, 0, 1};
    nextPageAnnotation.points = {{160, 45, 1}, {210, 100, 1}};
    state.ink = {sourceStroke, nextPageAnnotation};

    require(pluginRuntime->invokeMenu("LayerActions", 0), "LayerActions clone did not start its target-page check");
    waitForPluginDocumentJob();
    require(dialogHasResponseLabel("plugin-response-1", "Cancel") &&
                    dialogHasResponseLabel("plugin-response-2", "Proceed"),
            "LayerActions did not ask before replacing an annotated next page");
    answerDialog("plugin-response-1");
    require(messageDialog == nullptr && pageCount == 2 && state.ink.size() == 2 &&
                    nativeSession->pdfPageMap == std::vector<int>({0, 1}),
            "Canceling LayerActions clone modified the existing next page");

    require(pluginRuntime->invokeMenu("LayerActions", 0), "LayerActions clone could not be retried after cancellation");
    waitForPluginDocumentJob();
    require(messageDialog != nullptr, "LayerActions omitted the second annotated-page confirmation");
    answerDialog("plugin-response-2");
    waitForPluginDocumentJob();
    require(pageCount == 2 && state.page == 1 && nativeSession->pdfPageMap == std::vector<int>({0, 1}) &&
                    state.ink.size() == 2 && state.ink[0].page == 0 && state.ink[1].page == 1 &&
                    state.ink[0].layerId == 3 && state.ink[1].layerId == 3 && !state.pageLayers[0].backgroundVisible &&
                    state.pageLayers[1].backgroundVisible && !state.pageLayers[0].layers[1].visible &&
                    state.pageLayers[1].layers[1].visible,
            "Proceed did not replace the annotated page with a clone of the source and its next PDF background");
    capture("layer-actions-cloned");

    const auto cloneRoundTripPath =
            (std::filesystem::path(options.smokeDirectory) / "layer-actions-clone-round-trip.xopp").string();
    nativeSession->saveCopy(options.worker, cloneRoundTripPath, prototype::bridge::transferInk(state.committedInk()),
                            state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                            currentPageSizes(state));
    const prototype::bridge::DocumentSession cloneReopened(options.worker, cloneRoundTripPath);
    require(cloneReopened.pdfPageMap == std::vector<int>({0, 1}) && cloneReopened.initialInk.size() == 2 &&
                    cloneReopened.initialInk[0].page == 0 && cloneReopened.initialInk[1].page == 1 &&
                    cloneReopened.initialLayers[0].layers.size() == 3 &&
                    cloneReopened.initialLayers[1].layers.size() == 3 &&
                    cloneReopened.initialLayers[0].backgroundVisible &&
                    cloneReopened.initialLayers[1].backgroundVisible,
            "LayerActions clone did not preserve its page layers and editable content through save/reopen");

    performHistoryStep(false);
    waitForPluginDocumentJob();
    require(pageCount == 3 && nativeSession->pdfPageMap == std::vector<int>({0, 1, 1}) && state.ink.size() == 3 &&
                    state.ink[1].page == 2 && state.ink[1].points.front().x == 160 && state.ink[2].page == 1 &&
                    state.ink[2].points.front().x == 35,
            "Undo did not restore the deleted target after the inserted clone");

    performHistoryStep(false);
    waitForPluginDocumentJob();
    require(pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}) && state.ink.size() == 2 &&
                    state.ink[1].page == 1 && state.ink[1].points.front().x == 160,
            "The second undo did not remove the clone and restore the original annotated target");

    performHistoryStep(true);
    waitForPluginDocumentJob();
    require(pageCount == 3 && nativeSession->pdfPageMap == std::vector<int>({0, 1, 1}) && state.ink.size() == 3 &&
                    state.ink[1].page == 2 && state.ink[1].points.front().x == 160 && state.ink[2].page == 1 &&
                    state.ink[2].points.front().x == 35,
            "Redo did not restore the clone before the annotated target");

    performHistoryStep(true);
    waitForPluginDocumentJob();
    require(pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}) && state.ink.size() == 2 &&
                    state.ink[1].page == 1 && state.ink[1].points.front().x == 35,
            "The second redo did not replace the annotated target with the source clone");

    require(pluginRuntime->invokeMenu("LayerActions", 0), "LayerActions clone did not start its final-page check");
    waitForPluginDocumentJob();
    require(dialogHasResponseLabel("plugin-response-1", "OK"),
            "LayerActions did not report that the final page has no successor");
    answerDialog("plugin-response-1");

    require(pluginRuntime->invokeMenu("LayerActions", 1), "LayerActions hide did not execute");
    require(state.page == 1 && state.pageLayers.size() == 2 &&
                    std::all_of(state.pageLayers.begin(), state.pageLayers.end(),
                                [](const prototype::PageLayers& page) {
                                    return page.selectedLayer == 1 && page.layers[0].visible &&
                                           !page.layers[1].visible && !page.layers[2].visible;
                                }) &&
                    !state.pageLayers[0].backgroundVisible && state.pageLayers[1].backgroundVisible,
            "LayerActions hide did not select layer one, hide higher layers, and preserve background visibility");

    require(pluginRuntime->invokeMenu("LayerActions", 2), "LayerActions add did not execute");
    require(state.page == 1 && state.pageLayers[0].layers.size() == 4 && state.pageLayers[1].layers.size() == 4 &&
                    state.pageLayers[0].selectedLayer == 4 && state.pageLayers[1].selectedLayer == 4,
            "LayerActions add did not create and select a top layer on each page");
    state.undo();
    require(state.pageLayers[0].layers.size() == 4 && state.pageLayers[1].layers.size() == 3,
            "Undo did not revert the last per-page LayerActions layer addition");
    state.undo();
    require(state.pageLayers[0].layers.size() == 3 && state.pageLayers[1].layers.size() == 3,
            "Undo did not revert the first per-page LayerActions layer addition");
    state.redo();
    state.redo();
    require(state.pageLayers[0].layers.size() == 4 && state.pageLayers[1].layers.size() == 4,
            "Redo did not restore both per-page LayerActions layer additions");
    capture("layer-actions-complete");

    const auto roundTripPath =
            (std::filesystem::path(options.smokeDirectory) / "layer-actions-round-trip.xopp").string();
    nativeSession->saveCopy(options.worker, roundTripPath, prototype::bridge::transferInk(state.committedInk()),
                            state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                            currentPageSizes(state));
    const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
    require(reopened.initialLayers.size() == 2 && reopened.initialLayers[0].layers.size() == 4 &&
                    reopened.initialLayers[1].layers.size() == 4 && reopened.initialInk.size() == 2,
            "LayerActions added top layers and cloned editable content did not persist through native save/reopen");
    std::cout << "PASS: private GTK4 LayerActions annotated-page confirmation, clone, hide, add, undo/redo, and "
                 "native reopen\n";
}

void Prototype::verifyLatexIntegration() {
    require(nativeSession != nullptr, "LaTeX verification requires a native XOPP document");
    require(!latexEditor, "LaTeX editor should not already be open");

    constexpr int page = 0;
    state.selectActivePage(page);
    const auto pageSize = state.pageSize(page);
    prototype::Point insertionPoint{};
    bool foundEmptyPoint = false;
    for (double y = 48; y < pageSize.height - 48 && !foundEmptyPoint; y += 48) {
        for (double x = 48; x < pageSize.width - 48; x += 48) {
            if (!latexTargetAt(page, {x, y}).imageIndex && !latexTargetAt(page, {x, y}).textIndex) {
                insertionPoint = {x, y};
                foundEmptyPoint = true;
                break;
            }
        }
    }
    require(foundEmptyPoint, "LaTeX verification could not find an unused insertion point");

    const auto originalImageCount = state.images.size();
    const auto expectedSource = importedConfig.settings.latexUseExternalEditor ?
                                        std::string("\\frac{1}{2}") :
                                        importedConfig.settings.latexDefaultText;
    selectTool(Tool::Latex);
    const auto screenPoint = state.pageScreenPoint(page, insertionPoint.x, insertionPoint.y);
    openLatexEditorAt(screenPoint.x, screenPoint.y);
    require(latexEditor != nullptr, "The LaTeX tool did not open its editor");

    const gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    while (latexEditor && !latexEditor->readyForInsertion() && g_get_monotonic_time() < deadline) {
        while (g_main_context_iteration(nullptr, false)) {}
        g_usleep(1000);
    }
    require(latexEditor && latexEditor->readyForInsertion(), "LaTeX did not compile successfully within 30 seconds");
    latexEditor->activateInsert();
    require(!latexEditor && state.images.size() == originalImageCount + 1,
            "Inserting a compiled formula did not create one editable image");

    const size_t insertedIndex = state.images.size() - 1;
    const auto insertedFormula = state.images[insertedIndex];
    require(insertedFormula.kind == prototype::CanvasImageKind::Latex && insertedFormula.latexSource == expectedSource,
            "Inserted LaTeX image lost its editable source");
    require(prototype::isCanvasTexPdf(insertedFormula.encodedData), "Inserted LaTeX image did not retain a vector PDF");
    require(insertedFormula.nativeWidth > 0 && insertedFormula.nativeHeight > 0,
            "Inserted LaTeX image has invalid vector dimensions");
    require(std::abs(insertedFormula.transform.x - insertionPoint.x) < 1e-6 &&
                    std::abs(insertedFormula.transform.y - insertionPoint.y) < 1e-6,
            "Inserted LaTeX image moved away from the click position");
    waitForNextWindowFrame();
    capture("latex-formula-inserted");

    selectTool(Tool::Latex);
    const auto center = prototype::canvasImageCenter(insertedFormula);
    const auto editPoint = state.pageScreenPoint(page, center.x, center.y);
    openLatexEditorAt(editPoint.x, editPoint.y);
    const gint64 editDeadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    while (latexEditor && !latexEditor->readyForInsertion() && g_get_monotonic_time() < editDeadline) {
        while (g_main_context_iteration(nullptr, false)) {}
        g_usleep(1000);
    }
    require(latexEditor && latexEditor->readyForInsertion(),
            "The LaTeX tool did not reopen the inserted formula for editing");
    latexEditor->activateInsert();
    require(!latexEditor && state.images.size() == originalImageCount + 1 &&
                    state.images[insertedIndex].latexSource == insertedFormula.latexSource &&
                    state.images[insertedIndex].transform.x == insertedFormula.transform.x &&
                    state.images[insertedIndex].transform.y == insertedFormula.transform.y,
            "Editing an existing formula changed its placement or editable source");

    state.undo();
    require(state.images.size() == originalImageCount + 1 &&
                    state.images[insertedIndex].encodedData == insertedFormula.encodedData && state.canUndo(),
            "Undo did not restore the formula before its in-place edit");
    state.undo();
    require(state.images.size() == originalImageCount && state.canRedo(),
            "A second undo did not remove the inserted LaTeX formula");
    state.redo();
    state.redo();
    require(state.images.size() == originalImageCount + 1 && state.images[insertedIndex].latexSource == expectedSource,
            "Redo did not restore the inserted LaTeX formula");

    const auto roundTripPath = (std::filesystem::path(options.smokeDirectory) / "latex-round-trip.xopp").string();
    nativeSession->saveCopy(options.worker, roundTripPath, prototype::bridge::transferInk(state.committedInk()),
                            state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                            currentPageSizes(state));
    const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
    const auto reopenedFormula = std::find_if(
            reopened.initialImages.begin(), reopened.initialImages.end(),
            [&expectedSource, &insertedFormula, page](const prototype::bridge::ImagePacket& image) {
                return image.kind == prototype::CanvasImageKind::Latex && image.latexSource == expectedSource &&
                       image.page == page && std::abs(image.transform.x - insertedFormula.transform.x) < 1e-3 &&
                       std::abs(image.transform.y - insertedFormula.transform.y) < 1e-3;
            });
    require(reopenedFormula != reopened.initialImages.end() &&
                    prototype::isCanvasTexPdf(reopenedFormula->encodedData) &&
                    std::abs(reopenedFormula->nativeWidth - insertedFormula.nativeWidth) < 1e-3 &&
                    std::abs(reopenedFormula->nativeHeight - insertedFormula.nativeHeight) < 1e-3,
            "Native save/reopen did not preserve the LaTeX source, PDF or geometry");
    std::cout << "PASS: private GTK4 LaTeX compile, insert, edit, undo/redo and native round trip\n";
}

void Prototype::verifyImageActionsPlugin() {
    require(state.images.empty(), "ImageActions verification expects a clean editable image layer");
    const auto fixture = std::filesystem::path(options.smokeDirectory) / "image-actions-input.png";
    auto source = prototype::readCanvasImageFile(fixture);
    source.page = 0;
    source.transform = {1, 0, 0, 1, 35, 45};
    require(state.insertImage(source), "ImageActions integration could not insert its synthetic image");
    const auto sourceIndex = state.images.size() - 1;
    const auto original = state.images[sourceIndex];

    if (!prototype::canvasImageActionsAvailable()) {
        state.selectedImages = {sourceIndex};
        require(!pluginRuntime->invokeMenu("ImageActions", 0) && messageDialog,
                "ImageActions without libvips should report its missing dependency");
        answerDialog("close");
        state.undo();
        require(state.images.empty(), "Unavailable ImageActions should leave the selected image unchanged");
        return;
    }

    for (std::size_t mode = 0; mode < 10; ++mode) {
        state.selectedImages = {sourceIndex};
        require(pluginRuntime->invokeMenu("ImageActions", mode),
                "ImageActions menu operation failed to invoke its GTK host callback");
        const auto selectedCount = (mode == 8 || mode == 9) ? 2U : 1U;
        require(state.selectedImages.size() == selectedCount,
                "ImageActions did not select all outputs from the grouped operation");
        if (mode == 8) {
            capture("image-actions-split");
            const auto roundTripPath =
                    (std::filesystem::path(options.smokeDirectory) / "image-actions-round-trip.xopp").string();
            nativeSession->saveCopy(options.worker, roundTripPath, prototype::bridge::transferInk(state.committedInk()),
                                    state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                                    currentPageSizes(state));
            const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
            require(reopened.initialImages.size() == 2,
                    "Saving and reopening a split image group did not preserve both image parts");
        }

        const auto changedImages = state.images;
        require(state.canUndo(), "ImageActions transformation should be undoable");
        state.undo();
        require(state.images.size() == sourceIndex + 1 &&
                        state.images[sourceIndex].encodedData == original.encodedData &&
                        state.selectedImages == std::vector<size_t>{sourceIndex},
                "One ImageActions undo should restore its original image and selection");
        require(state.canRedo(), "ImageActions transformation should be redoable");
        state.redo();
        require(state.images.size() == changedImages.size() && state.selectedImages.size() == selectedCount,
                "One ImageActions redo should restore the complete transformed image group");
        for (size_t index = 0; index < changedImages.size(); ++index) {
            require(state.images[index].encodedData == changedImages[index].encodedData &&
                            state.images[index].pixelWidth == changedImages[index].pixelWidth &&
                            state.images[index].pixelHeight == changedImages[index].pixelHeight,
                    "ImageActions redo changed a transformed output");
        }
        state.undo();
    }
    state.undo();
    require(state.images.empty(), "ImageActions verification should remove its fixture with one undo");
}

void Prototype::waitForPluginDocumentJob() {
    const gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
    while (busy && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(!busy, "The native document operation did not finish within 20 seconds");
    waitForNextWindowFrame();
}

}  // namespace prototype::gtk4
