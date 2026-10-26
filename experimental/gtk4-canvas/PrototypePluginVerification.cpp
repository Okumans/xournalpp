#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::verifyPluginIntegration() {
    require(pluginRuntime != nullptr, "Lua plugin runtime was not created");
    const int displayDpi = pluginDisplayDpi();
    require(displayDpi >= 36 && displayDpi <= 720,
            "app.getDisplayDpi did not return a bounded monitor DPI value or fallback");
    prototype::PluginActionValue actionState;
    std::string actionStateError;
    require(getPluginActionState("zoom", actionState, actionStateError) &&
                    std::holds_alternative<double>(actionState) &&
                    std::abs(std::get<double>(actionState) - state.zoom) < 1e-9,
            actionStateError.empty() ? "app.getActionState did not report the current zoom" : actionStateError.c_str());
    require(getPluginActionState("tool-size", actionState, actionStateError) &&
                    std::holds_alternative<std::int64_t>(actionState) &&
                    std::get<std::int64_t>(actionState) == pluginActiveToolSizeState(),
            actionStateError.empty() ? "app.getActionState did not report the active tool size" :
                                       actionStateError.c_str());
    require(getPluginActionState("tool-pen-size", actionState, actionStateError) &&
                    std::holds_alternative<std::int64_t>(actionState) &&
                    std::get<std::int64_t>(actionState) ==
                            pluginToolSizeState(prototype::behavior::penWidths, penBrush.width),
            actionStateError.empty() ? "app.getActionState did not report the pen size preset" :
                                       actionStateError.c_str());
    require(getPluginActionState("tool-eraser-size", actionState, actionStateError) &&
                    std::holds_alternative<std::int64_t>(actionState) &&
                    std::get<std::int64_t>(actionState) ==
                            pluginToolSizeState(prototype::behavior::eraserWidths, eraserBrush.width),
            actionStateError.empty() ? "app.getActionState did not report the eraser size preset" :
                                       actionStateError.c_str());
    require(getPluginActionState("tool-highlighter-size", actionState, actionStateError) &&
                    std::holds_alternative<std::int64_t>(actionState) &&
                    std::get<std::int64_t>(actionState) ==
                            pluginToolSizeState(prototype::behavior::highlighterWidths, highlightBrush.width),
            actionStateError.empty() ? "app.getActionState did not report the highlighter size preset" :
                                       actionStateError.c_str());
    require(getPluginActionState("tool-eraser-type", actionState, actionStateError) &&
                    std::holds_alternative<std::int64_t>(actionState) &&
                    std::get<std::int64_t>(actionState) == static_cast<std::int64_t>(state.eraserMode) + 1,
            actionStateError.empty() ? "app.getActionState did not report the eraser mode" : actionStateError.c_str());
    require(pluginToolSizeState(prototype::behavior::penWidths, 1.41) == 2 &&
                    pluginToolSizeState(prototype::behavior::penWidths, 1.42) == 5,
            "app.getActionState did not distinguish preset sizes from custom widths");
    prototype::PluginToolInfo toolInfo;
    std::string toolInfoError;
    const auto activeToolForLineStyle = state.brush.tool;
    state.brush.tool = Tool::Pen;
    const auto expectedPenLineStyle = prototype::formatLineStyle(penBrush.lineDashes);
    const bool reportsCurrentPenLineStyle =
            getPluginActionState("tool-pen-line-style", actionState, actionStateError) &&
            std::holds_alternative<std::string>(actionState) &&
            std::get<std::string>(actionState) == expectedPenLineStyle;
    const auto previousPenLineDashes = penBrush.lineDashes;
    const auto previousActiveLineDashes = state.brush.lineDashes;
    const auto previousPenLineStyle = importedConfig.settings.penLineStyle;
    std::string lineStyleError;
    if (state.selected.empty()) {
        require(changePluginActionState("tool-pen-line-style", std::string{"dash"}, lineStyleError) &&
                        penBrush.lineDashes == prototype::LineDashPattern{6, 3} &&
                        getPluginActionState("tool-pen-line-style", actionState, actionStateError) &&
                        std::get<std::string>(actionState) == "dash",
                lineStyleError.empty() ? "Plugin line-style action did not apply a named dash pattern" :
                                         lineStyleError.c_str());
        require(changePluginActionState("tool-pen-line-style", std::string{"cust: 1.25 2.50"}, lineStyleError) &&
                        penBrush.lineDashes == prototype::LineDashPattern{1.25, 2.5} &&
                        getPluginActionState("tool-pen-line-style", actionState, actionStateError) &&
                        std::get<std::string>(actionState) == "cust: 1.25 2.50" &&
                        getPluginToolInfo("pen", toolInfo, toolInfoError) && toolInfo.lineStyle &&
                        *toolInfo.lineStyle == "cust: 1.25 2.50",
                lineStyleError.empty() ? "Plugin line-style action did not preserve a custom dash pattern" :
                                         lineStyleError.c_str());
        require(changePluginActionState("tool-pen-line-style", previousPenLineStyle, lineStyleError) &&
                        penBrush.lineDashes == previousPenLineDashes,
                lineStyleError.empty() ? "Plugin line-style action could not restore the Pen setting" :
                                         lineStyleError.c_str());
    }
    state.brush.tool = activeToolForLineStyle;
    state.brush.lineDashes = previousActiveLineDashes;
    require(reportsCurrentPenLineStyle, "app.getActionState did not report the current Pen line style");
    require(getPluginActionState("grid-snapping", actionState, actionStateError) &&
                    std::holds_alternative<bool>(actionState) && std::get<bool>(actionState) == state.snapGridEnabled,
            actionStateError.empty() ? "app.getActionState did not report grid snapping" : actionStateError.c_str());
    require(getPluginActionState("position-highlighting", actionState, actionStateError) &&
                    std::holds_alternative<bool>(actionState) &&
                    std::get<bool>(actionState) == importedConfig.settings.highlightPosition,
            actionStateError.empty() ? "app.getActionState did not report position highlighting" :
                                       actionStateError.c_str());
    require(getPluginActionState("select-tool", actionState, actionStateError) &&
                    std::holds_alternative<std::int64_t>(actionState) &&
                    std::get<std::int64_t>(actionState) == pluginSelectedToolState(),
            actionStateError.empty() ? "app.getActionState did not report the current tool" : actionStateError.c_str());
    if (state.brush.tool == Tool::Pen) {
        const auto drawingMode = state.drawingMode;
        state.drawingMode = prototype::DrawingMode::Rectangle;
        const bool reportsPenDuringShapeDrawing = getPluginActionState("select-tool", actionState, actionStateError) &&
                                                  std::holds_alternative<std::int64_t>(actionState) &&
                                                  std::get<std::int64_t>(actionState) == 1;
        state.drawingMode = drawingMode;
        require(reportsPenDuringShapeDrawing,
                "app.getActionState select-tool confused the Pen tool with a drawing-mode action");
    }
    require(getPluginActionState("tool-color", actionState, actionStateError) &&
                    std::holds_alternative<std::int64_t>(actionState) && std::get<std::int64_t>(actionState) >= 0 &&
                    std::get<std::int64_t>(actionState) <= 0xffffffff,
            actionStateError.empty() ? "app.getActionState did not report the active tool color" :
                                       actionStateError.c_str());
    require(!getPluginActionState("missing-action-state", actionState, actionStateError) &&
                    actionStateError.find("unavailable") != std::string::npos,
            "app.getActionState did not reject an unsupported action state");
    require(getPluginToolInfo("active", toolInfo, toolInfoError) && toolInfo.type &&
                    *toolInfo.type == pluginToolName(state.brush.tool) && toolInfo.sizeValue &&
                    std::abs(*toolInfo.sizeValue - state.brush.width) < 1e-9 && toolInfo.color &&
                    *toolInfo.color == pluginRgbColor(state.brush.color),
            toolInfoError.empty() ? "app.getToolInfo did not report the active GTK4 tool" : toolInfoError.c_str());
    require(getPluginToolInfo("pen", toolInfo, toolInfoError) && toolInfo.sizeValue &&
                    std::abs(*toolInfo.sizeValue - penBrush.width) < 1e-9 && toolInfo.color &&
                    *toolInfo.color == pluginRgbColor(penBrush.color) && toolInfo.filled == false,
            toolInfoError.empty() ? "app.getToolInfo did not report Pen settings" : toolInfoError.c_str());
    require(getPluginToolInfo("highlighter", toolInfo, toolInfoError) && toolInfo.sizeValue &&
                    std::abs(*toolInfo.sizeValue - highlightBrush.width) < 1e-9 && toolInfo.color &&
                    *toolInfo.color == pluginRgbColor(highlightBrush.color),
            toolInfoError.empty() ? "app.getToolInfo did not report Highlighter settings" : toolInfoError.c_str());
    require(getPluginToolInfo("eraser", toolInfo, toolInfoError) && toolInfo.eraserType && toolInfo.sizeValue &&
                    std::abs(*toolInfo.sizeValue - eraserBrush.width) < 1e-9,
            toolInfoError.empty() ? "app.getToolInfo did not report Eraser settings" : toolInfoError.c_str());
    require(getPluginToolInfo("text", toolInfo, toolInfoError) && toolInfo.fontName &&
                    *toolInfo.fontName == state.textFontName && toolInfo.fontSize &&
                    std::abs(*toolInfo.fontSize - state.textFontSize) < 1e-9 && toolInfo.color &&
                    *toolInfo.color == pluginRgbColor(state.textColor),
            toolInfoError.empty() ? "app.getToolInfo did not report Text settings" : toolInfoError.c_str());
    const auto backgroundPage = static_cast<std::size_t>(state.page);
    const auto originalBackgroundName = state.pageLayers.at(backgroundPage).backgroundName;
    const auto backgroundNameRevision = state.revision();
    const auto backgroundNameSelection = state.selected;
    const auto backgroundNameTextSelection = state.selectedTexts;
    const auto backgroundNameImageSelection = state.selectedImages;
    std::string backgroundNameError;
    require(setPluginBackgroundName("Plugin API test", backgroundNameError) &&
                    state.layersForPage(state.page).backgroundName == "Plugin API test" &&
                    state.revision() == backgroundNameRevision && state.selected == backgroundNameSelection &&
                    state.selectedTexts == backgroundNameTextSelection &&
                    state.selectedImages == backgroundNameImageSelection,
            backgroundNameError.empty() ? "app.setBackgroundName changed undo or selection state" :
                                          backgroundNameError.c_str());
    state.pageLayers.at(backgroundPage).backgroundName = originalBackgroundName;
    rebuildLayerStack();
    update();
    require(nativeSession && !nativeSession->pdfPageLabels.empty(),
            "Plugin integration fixture has no PDF page labels");
    const auto refreshPageRevision = state.revision();
    const bool refreshPageCanUndo = state.canUndo();
    const bool refreshPageCanRedo = state.canRedo();
    require(waitForCanvasRaster(state.page, canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))),
            "Plugin page-refresh verification could not render the current page");
    require(!canvasRenderer.empty(), "Plugin page-refresh verification could not build the current page cache");
    std::string refreshPageError;
    require(refreshPluginPage(refreshPageError) && canvasRenderer.empty() && thumbnailRenderer.empty() &&
                    state.revision() == refreshPageRevision && state.canUndo() == refreshPageCanUndo &&
                    state.canRedo() == refreshPageCanRedo,
            refreshPageError.empty() ? "app.refreshPage did not invalidate page surfaces without changing history" :
                                       refreshPageError.c_str());
    std::string firstPdfPageLabel;
    std::string pageLabelError;
    require(getPluginPageLabel(0, firstPdfPageLabel, pageLabelError) &&
                    firstPdfPageLabel == nativeSession->pdfPageLabels.front(),
            pageLabelError.empty() ? "app.getPageLabel did not return the first PDF page label" :
                                     pageLabelError.c_str());
    require(!getPluginPageLabel(nativeSession->pdfPageLabels.size(), firstPdfPageLabel, pageLabelError) &&
                    pageLabelError ==
                            "page nr " + std::to_string(nativeSession->pdfPageLabels.size()) + " is out of range",
            "app.getPageLabel did not return the master-compatible out-of-range result");
    const auto pluginPalette = getPluginColorPalette();
    require(pluginPalette.size() == importedConfig.palette.swatches.size(),
            "app.getColorPalette did not return every imported color");
    for (std::size_t index = 0; index < pluginPalette.size(); ++index) {
        const auto& swatch = importedConfig.palette.swatches[index];
        const auto expectedColor = static_cast<std::uint32_t>(std::stoul(swatch.cssHex.substr(1), nullptr, 16));
        require(pluginPalette[index].name == swatch.name && pluginPalette[index].color == expectedColor,
                "app.getColorPalette lost a swatch name or 24-bit color value");
    }
    require(state.selectedTexts.empty() && (!textEditor || !textEditor->active()),
            "Plugin font verification unexpectedly started with selected or editing text");
    const prototype::PluginFont originalPluginFont{state.textFontName, state.textFontSize};
    const auto originalFontRevision = state.revision();
    const bool originalFontCanUndo = state.canUndo();
    const bool originalFontCanRedo = state.canRedo();
    const auto historyRemainsUnchanged = [&] {
        return state.revision() == originalFontRevision && state.canUndo() == originalFontCanUndo &&
               state.canRedo() == originalFontCanRedo;
    };
    std::string fontError;
    const prototype::PluginFont changedPluginFont{originalPluginFont.name, originalPluginFont.size + 1};
    require(setPluginFont(changedPluginFont, fontError) && state.textFontName == changedPluginFont.name &&
                    state.textFontSize == changedPluginFont.size &&
                    std::abs(gtk_spin_button_get_value(GTK_SPIN_BUTTON(textFontSize)) - changedPluginFont.size) <
                            1e-9 &&
                    historyRemainsUnchanged(),
            fontError.empty() ? "app.setFont did not update GTK4 text defaults without document history" :
                                fontError.c_str());
    require(setPluginFont(originalPluginFont, fontError) && state.textFontName == originalPluginFont.name &&
                    state.textFontSize == originalPluginFont.size && historyRemainsUnchanged(),
            fontError.empty() ? "app.setFont did not restore the original text defaults" : fontError.c_str());
    const auto savedZoom = state.zoom;
    const auto savedOffsetX = state.offsetX;
    const auto savedOffsetY = state.offsetY;
    const bool savedFitMode = fitMode;
    const bool savedFitted = fitted;
    const double zoomCenterX = gtk_widget_get_width(canvas) / 2.0;
    const double zoomCenterY = gtk_widget_get_height(canvas) / 2.0;
    const auto documentPointBeforeZoom = state.documentPoint(zoomCenterX, zoomCenterY);
    const auto requestedZoom = savedZoom > 4 ? savedZoom * 0.75 : savedZoom * 1.25;
    std::string zoomError;
    require(setPluginZoom(requestedZoom, zoomError) && std::abs(state.zoom - requestedZoom) < 1e-9 && !fitMode,
            zoomError.empty() ? "The GTK4 plugin host did not apply app.setZoom or leave fit mode" : zoomError.c_str());
    const auto documentPointAfterZoom = state.documentPoint(zoomCenterX, zoomCenterY);
    require(std::abs(documentPointBeforeZoom.x - documentPointAfterZoom.x) < 1e-7 &&
                    std::abs(documentPointBeforeZoom.y - documentPointAfterZoom.y) < 1e-7,
            "app.setZoom did not keep the viewport center anchored to the same document point");
    state.zoom = savedZoom;
    state.offsetX = savedOffsetX;
    state.offsetY = savedOffsetY;
    fitMode = savedFitMode;
    fitted = savedFitted;
    update();

    const int originalPage = state.page;
    const auto originalImageSelection = selectedImageIndex;
    const int availablePages = state.pageCount();
    std::string pageError;
    require(setPluginCurrentPage(0, pageError) && state.page == 0 && state.offsetX == savedOffsetX &&
                    state.offsetY == savedOffsetY && state.zoom == savedZoom && fitMode == savedFitMode &&
                    fitted == savedFitted,
            pageError.empty() ? "app.setCurrentPage did not clamp to the first page without scrolling" :
                                pageError.c_str());
    require(setPluginCurrentPage(static_cast<std::size_t>(availablePages) + 10, pageError) &&
                    state.page == availablePages - 1 && state.offsetX == savedOffsetX && state.offsetY == savedOffsetY,
            pageError.empty() ? "app.setCurrentPage did not clamp to the final page without scrolling" :
                                pageError.c_str());
    state.selectActivePage(originalPage);
    selectedImageIndex = originalImageSelection;
    update();

    const auto originalScrollPosition = getPluginScrollPosition();
    const auto originalScrollRevision = state.revision();
    const bool originalCanUndoScrollState = state.canUndo();
    const bool originalCanRedoScrollState = state.canRedo();
    const auto scrollHistoryRemainsUnchanged = [&] {
        return state.revision() == originalScrollRevision && state.canUndo() == originalCanUndoScrollState &&
               state.canRedo() == originalCanRedoScrollState;
    };
    const double scrollOriginX = std::max(0.0, (originalScrollPosition.width - state.layoutWidth() * state.zoom) / 2.0);
    const double scrollOriginY =
            std::max(0.0, (originalScrollPosition.height - state.layoutHeight() * state.zoom) / 2.0);
    require(originalScrollPosition.width > 0 && originalScrollPosition.height > 0 &&
                    std::abs(originalScrollPosition.x - std::max(0.0, scrollOriginX - savedOffsetX)) < 1e-9 &&
                    std::abs(originalScrollPosition.y - std::max(0.0, scrollOriginY - savedOffsetY)) < 1e-9,
            "app.getScrollPos did not report the current canvas position and viewport size");

    std::string scrollError;
    require(scrollPluginToPosition(20.5, 10.25, true, scrollError) &&
                    std::abs(state.offsetX - savedOffsetX + 20.5) < 1e-9 &&
                    std::abs(state.offsetY - savedOffsetY + 10.25) < 1e-9 && scrollHistoryRemainsUnchanged(),
            scrollError.empty() ? "app.scrollToPos did not apply relative pixel scrolling" : scrollError.c_str());
    require(scrollPluginToPosition(200.25, 50.5, false, scrollError) &&
                    std::abs(getPluginScrollPosition().x - 200.25) < 1e-9 &&
                    std::abs(getPluginScrollPosition().y - 50.5) < 1e-9 && scrollHistoryRemainsUnchanged(),
            scrollError.empty() ? "app.scrollToPos did not set absolute canvas coordinates" : scrollError.c_str());
    require(scrollPluginToPosition(-20, -10, false, scrollError) && getPluginScrollPosition().x == 0 &&
                    getPluginScrollPosition().y == 0 && scrollHistoryRemainsUnchanged(),
            scrollError.empty() ? "app.scrollToPos did not clamp absolute positions to the layout origin" :
                                  scrollError.c_str());

    state.selectActivePage(originalPage);
    state.zoom = 0.1;
    const auto visiblePageSize = state.pageSize(originalPage);
    state.offsetX = (originalScrollPosition.width - state.layoutWidth() * state.zoom) / 2.0;
    state.offsetY = (originalScrollPosition.height - visiblePageSize.height * state.zoom) / 2.0 -
                    state.pageTop(originalPage) * state.zoom;
    const double visiblePageOffsetX = state.offsetX;
    const double visiblePageOffsetY = state.offsetY;
    require(scrollPluginToPage(originalPage + 1, false, scrollError) && state.page == originalPage &&
                    std::abs(state.offsetX - visiblePageOffsetX) < 1e-9 &&
                    std::abs(state.offsetY - visiblePageOffsetY) < 1e-9 && scrollHistoryRemainsUnchanged(),
            scrollError.empty() ? "app.scrollToPage moved a page that was already visible" : scrollError.c_str());
    state.zoom = savedZoom;
    state.offsetX = savedOffsetX;
    state.offsetY = savedOffsetY;
    fitMode = savedFitMode;
    fitted = savedFitted;
    state.selectActivePage(originalPage);

    require(scrollPluginToPage(availablePages + 10, false, scrollError) && state.page == availablePages - 1 &&
                    scrollHistoryRemainsUnchanged(),
            scrollError.empty() ? "app.scrollToPage did not clamp to the final page" : scrollError.c_str());
    require(scrollPluginToPage(1, true, scrollError) && state.page == availablePages - 1 &&
                    scrollHistoryRemainsUnchanged(),
            scrollError.empty() ? "app.scrollToPage did not clamp a relative page offset" : scrollError.c_str());
    require(scrollPluginToPage(1, false, scrollError) && state.page == 0 && scrollPluginToPage(1, true, scrollError) &&
                    state.page == std::min(1, availablePages - 1) && scrollHistoryRemainsUnchanged(),
            scrollError.empty() ? "app.scrollToPage did not navigate absolute and relative page numbers" :
                                  scrollError.c_str());

    const auto positionBeforeInvalidScroll = getPluginScrollPosition();
    require(!scrollPluginToPosition(std::numeric_limits<double>::infinity(), 0, true, scrollError) &&
                    scrollError == "Scroll positions must be finite numbers" &&
                    getPluginScrollPosition().x == positionBeforeInvalidScroll.x &&
                    getPluginScrollPosition().y == positionBeforeInvalidScroll.y && scrollHistoryRemainsUnchanged(),
            "app.scrollToPos accepted a nonfinite coordinate or changed state before reporting the error");

    state.selectActivePage(originalPage);
    state.offsetX = savedOffsetX;
    state.offsetY = savedOffsetY;
    fitMode = savedFitMode;
    fitted = savedFitted;
    selectedImageIndex = originalImageSelection;
    update();

    const auto originalPageLayers = state.pageLayers;
    const auto originalLayerRevision = state.revision();
    const bool originalCanUndoLayerState = state.canUndo();
    const bool originalCanRedoLayerState = state.canRedo();
    const int layerTestPage = state.page;
    auto& layerTestState = state.pageLayers.at(static_cast<std::size_t>(layerTestPage));
    while (layerTestState.layers.size() < 3) {
        layerTestState.layers.push_back({"Plugin API test layer", true, {prototype::newLayerOrigin}});
    }
    const auto foregroundLayerVisibility = [&] {
        std::vector<bool> visibility;
        const auto& layers = state.pageLayers[static_cast<std::size_t>(layerTestPage)].layers;
        visibility.reserve(layers.size());
        for (const auto& layer: layers) {
            visibility.push_back(layer.visible);
        }
        return visibility;
    };
    const auto initialLayerVisibility = foregroundLayerVisibility();
    const bool initialBackgroundVisibility = layerTestState.backgroundVisible;
    const auto revisionRemainsUnchanged = [&] {
        return state.revision() == originalLayerRevision && state.canUndo() == originalCanUndoLayerState &&
               state.canRedo() == originalCanRedoLayerState;
    };
    std::string layerError;
    require(setPluginCurrentLayer(2, false, layerError) &&
                    state.pageLayers[static_cast<std::size_t>(layerTestPage)].selectedLayer == 2 &&
                    foregroundLayerVisibility() == initialLayerVisibility &&
                    state.pageLayers[static_cast<std::size_t>(layerTestPage)].backgroundVisible ==
                            initialBackgroundVisibility &&
                    state.selected.empty() && state.selectedTexts.empty() && state.selectedImages.empty() &&
                    revisionRemainsUnchanged(),
            layerError.empty() ? "app.setCurrentLayer did not select layer two without changing visibility or history" :
                                 layerError.c_str());

    auto expectedLayerVisibility = initialLayerVisibility;
    std::fill(expectedLayerVisibility.begin(), expectedLayerVisibility.end(), false);
    expectedLayerVisibility[0] = true;
    expectedLayerVisibility[1] = true;
    require(setPluginCurrentLayer(2, true, layerError) &&
                    state.pageLayers[static_cast<std::size_t>(layerTestPage)].selectedLayer == 2 &&
                    foregroundLayerVisibility() == expectedLayerVisibility &&
                    state.pageLayers[static_cast<std::size_t>(layerTestPage)].backgroundVisible ==
                            initialBackgroundVisibility &&
                    revisionRemainsUnchanged(),
            layerError.empty() ? "app.setCurrentLayer did not reveal layers through layer two without history" :
                                 layerError.c_str());

    require(setPluginCurrentLayer(0, true, layerError) &&
                    state.pageLayers[static_cast<std::size_t>(layerTestPage)].selectedLayer == 1 &&
                    std::none_of(state.pageLayers[static_cast<std::size_t>(layerTestPage)].layers.begin(),
                                 state.pageLayers[static_cast<std::size_t>(layerTestPage)].layers.end(),
                                 [](const auto& layer) { return layer.visible; }) &&
                    state.pageLayers[static_cast<std::size_t>(layerTestPage)].backgroundVisible ==
                            initialBackgroundVisibility &&
                    revisionRemainsUnchanged(),
            layerError.empty() ? "app.setCurrentLayer did not retain the layer-zero visibility behavior" :
                                 layerError.c_str());

    const auto beforeInvalidLayer = state.pageLayers[static_cast<std::size_t>(layerTestPage)];
    const auto missingLayerId = beforeInvalidLayer.layers.size() + 1;
    require(!setPluginCurrentLayer(missingLayerId, true, layerError) &&
                    layerError == "No layer with layer ID " + std::to_string(missingLayerId) &&
                    state.pageLayers[static_cast<std::size_t>(layerTestPage)] == beforeInvalidLayer &&
                    revisionRemainsUnchanged(),
            "app.setCurrentLayer accepted an out-of-range layer or changed state before reporting the error");

    state.pageLayers = originalPageLayers;
    state.clearSelection();
    selectedImageIndex = originalImageSelection;
    rebuildLayerStack();
    update();

    const auto documentStructure = pluginDocumentStructure();
    require(documentStructure.pages.size() == state.pages.size() &&
                    documentStructure.currentPage == static_cast<std::size_t>(state.page + 1) &&
                    documentStructure.pdfBackgroundFilename == sourcePath && documentStructure.xoppFilename.empty(),
            "app.getDocumentStructure did not expose the active PDF and page count");
    for (std::size_t pageIndex = 0; pageIndex < documentStructure.pages.size(); ++pageIndex) {
        const auto& page = documentStructure.pages[pageIndex];
        const auto& pageSize = state.pages[pageIndex];
        const auto pdfPage = nativeSession->pdfPageMap.at(pageIndex);
        require(std::abs(page.width - pageSize.width) < 1e-9 && std::abs(page.height - pageSize.height) < 1e-9 &&
                        page.pdfBackgroundPageNo == (pdfPage < 0 ? 0 : pdfPage + 1) &&
                        page.currentLayer == state.activeLayerId(static_cast<int>(pageIndex)) &&
                        page.layers.size() == state.layersForPage(static_cast<int>(pageIndex)).layers.size() + 1,
                "app.getDocumentStructure lost page dimensions, PDF mapping, current layer, or layer stack");
    }
    const auto savedInkSize = state.ink.size();
    const auto savedTextSize = state.texts.size();
    const auto savedImageSize = state.images.size();
    const auto savedRegularTextCount = static_cast<std::size_t>(
            std::count_if(state.texts.begin(), state.texts.end(), [](const auto& item) { return !item.isLink; }));
    const auto savedLinkCount = state.texts.size() - savedRegularTextCount;
    const auto savedSelection = state.selected;
    const auto savedTextSelection = state.selectedTexts;
    const auto savedImageSelection = state.selectedImages;
    prototype::Ink sample;
    sample.page = state.page;
    sample.points = {{10, 20, 0.25}, {12.5, 24.5, 0.75}};
    sample.brush = {Tool::Pen, {0x12 / 255.0, 0x34 / 255.0, 0x56 / 255.0}, 2.25};
    sample.brush.lineDashes = {1.25, 2.5};
    sample.pressureSensitive = true;
    sample.layerId = state.activeLayerId(state.page);
    sample.layerOrder = 999999;
    state.ink.push_back(sample);

    prototype::TextItem pluginText;
    pluginText.page = state.page;
    pluginText.layerId = state.activeLayerId(state.page);
    pluginText.layerOrder = 999998;
    pluginText.text = "Lua API note";
    pluginText.fontName = "Sans";
    pluginText.fontSize = 14;
    pluginText.color = 0xff123456U;
    pluginText.x = 21;
    pluginText.y = 22;
    pluginText.wrapWidth = 90;
    pluginText.layoutWidth = 45;
    pluginText.layoutHeight = 18;
    state.texts.push_back(pluginText);

    auto pluginLink = pluginText;
    pluginLink.layerOrder = 1000000;
    pluginLink.text = "Lua API link";
    pluginLink.isLink = true;
    pluginLink.linkUrl = "https://example.test";
    pluginLink.alignment = 1;
    pluginLink.fontName = "Serif";
    pluginLink.fontSize = 12;
    pluginLink.color = 0xffabcdefU;
    pluginLink.layoutWidth = 60;
    pluginLink.layoutHeight = 16;
    state.texts.push_back(pluginLink);

    GError* imageEncodeError = nullptr;
    auto* pluginPixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1, 1);
    require(pluginPixbuf != nullptr, "Could not allocate the Lua image-query fixture");
    gdk_pixbuf_fill(pluginPixbuf, 0x3877c4ffU);
    gchar* pluginImageBuffer = nullptr;
    gsize pluginImageLength = 0;
    const bool imageEncoded = gdk_pixbuf_save_to_buffer(pluginPixbuf, &pluginImageBuffer, &pluginImageLength, "png",
                                                        &imageEncodeError, nullptr);
    g_object_unref(pluginPixbuf);
    require(imageEncoded && pluginImageBuffer != nullptr,
            imageEncodeError ? imageEncodeError->message : "Could not encode the Lua image-query fixture");
    std::string pluginImageBytes(pluginImageBuffer, pluginImageLength);
    g_free(pluginImageBuffer);
    g_clear_error(&imageEncodeError);
    prototype::CanvasImage pluginImage;
    pluginImage.page = state.page;
    pluginImage.layerId = state.activeLayerId(state.page);
    pluginImage.layerOrder = 999997;
    pluginImage.pixelWidth = 80;
    pluginImage.pixelHeight = 60;
    pluginImage.nativeWidth = 80;
    pluginImage.nativeHeight = 60;
    pluginImage.transform.xx = 0.5;
    pluginImage.transform.yy = 0.5;
    pluginImage.transform.x = 11;
    pluginImage.transform.y = 12;
    pluginImage.encodedData = pluginImageBytes;
    state.images.push_back(std::move(pluginImage));

    const auto annotatedStructure = pluginDocumentStructure();
    require(annotatedStructure.pages.at(static_cast<std::size_t>(state.page)).isAnnotated &&
                    annotatedStructure.pages.at(static_cast<std::size_t>(state.page))
                                    .layers.at(sample.layerId)
                                    .isAnnotated == std::optional<bool>{true},
            "app.getDocumentStructure did not reflect an editable stroke in page/layer annotation state");

    std::vector<prototype::PluginStroke> allStrokes;
    std::string strokeError;
    require(getPluginStrokes("all", allStrokes, strokeError) && allStrokes.size() == savedInkSize + 1,
            "app.getStrokes('all') did not enumerate every editable stroke");
    const auto sampleOutput = std::find_if(allStrokes.begin(), allStrokes.end(), [&](const auto& stroke) {
        return stroke.reference == state.ink.back().pluginReference;
    });
    require(sampleOutput != allStrokes.end() && sampleOutput->x == std::vector<double>({10, 12.5}) &&
                    sampleOutput->y == std::vector<double>({20, 24.5}) &&
                    sampleOutput->pressure == std::vector<double>({0.25, 0.75}) && sampleOutput->tool == "pen" &&
                    std::abs(sampleOutput->width - 2.25) < 1e-9 && sampleOutput->color == 0x123456 &&
                    sampleOutput->lineStyle == "cust: 1.25 2.50" &&
                    sampleOutput->page == static_cast<size_t>(state.page + 1) &&
                    sampleOutput->layer == state.activeLayerId(state.page),
            "app.getStrokes('all') did not preserve native point, pressure, style, or index values");
    for (const auto& stroke: allStrokes) {
        require(!stroke.x.empty() && stroke.x.size() == stroke.y.size() && stroke.reference != 0 && stroke.page &&
                        stroke.layer && *stroke.page >= 1 && *stroke.page <= state.pages.size() && *stroke.layer >= 1 &&
                        *stroke.layer <= state.layersForPage(static_cast<int>(*stroke.page - 1)).layers.size(),
                "app.getStrokes('all') lost stroke geometry, references, or 1-based page/layer indices");
    }
    const auto sampleStrokeReference = state.ink.back().pluginReference;
    std::vector<prototype::PluginText> allTexts;
    std::vector<prototype::PluginLink> allLinks;
    std::vector<prototype::PluginImage> allImages;
    require(getPluginTexts("all", allTexts, strokeError) && allTexts.size() == savedRegularTextCount + 1 &&
                    getPluginLinks("all", allLinks, strokeError) && allLinks.size() == savedLinkCount + 1 &&
                    getPluginImages("all", allImages, strokeError) && allImages.size() == savedImageSize + 1,
            "Lua object queries did not enumerate editable text, links, and images");
    const auto textOutput = std::find_if(allTexts.begin(), allTexts.end(),
                                         [](const auto& text) { return text.text == "Lua API note"; });
    const auto linkOutput = std::find_if(allLinks.begin(), allLinks.end(),
                                         [](const auto& link) { return link.text == "Lua API link"; });
    const auto imageOutput = std::find_if(allImages.begin(), allImages.end(), [&](const auto& image) {
        return image.reference == state.images.back().pluginReference;
    });
    require(textOutput != allTexts.end() && textOutput->fontName == "Sans" && textOutput->fontSize == 14 &&
                    textOutput->color == 0x123456 && textOutput->x == 21 && textOutput->y == 22 &&
                    textOutput->wrap == std::optional<double>{90} && textOutput->width == 45 &&
                    textOutput->height == 18 && textOutput->page == static_cast<size_t>(state.page + 1) &&
                    textOutput->layer == state.activeLayerId(state.page) && textOutput->reference != 0,
            "app.getTexts lost font, geometry, reference, or page/layer data");
    require(linkOutput != allLinks.end() && linkOutput->url == "https://example.test" && linkOutput->alignment == 1 &&
                    linkOutput->fontName == "Serif" && linkOutput->fontSize == 12 && linkOutput->color == 0xabcdef &&
                    linkOutput->x == 21 && linkOutput->y == 22 && linkOutput->width == 60 && linkOutput->height == 16 &&
                    linkOutput->layer == state.activeLayerId(state.page) && linkOutput->reference != 0,
            "app.getLinks lost URL, font, geometry, reference, or layer data");
    require(imageOutput != allImages.end() && imageOutput->data.size() > 8 && imageOutput->x == 11 &&
                    imageOutput->y == 12 && imageOutput->width == 40 && imageOutput->height == 30 &&
                    imageOutput->imageWidth == 80 && imageOutput->imageHeight == 60 &&
                    imageOutput->page == static_cast<size_t>(state.page + 1) &&
                    imageOutput->layer == state.activeLayerId(state.page) && imageOutput->reference != 0,
            "app.getImages lost image data, dimensions, reference, or page/layer metadata");

    state.selected = {state.ink.size() - 1};
    state.selectedTexts = {savedTextSize, savedTextSize + 1};
    state.selectedImages = {savedImageSize};
    std::vector<prototype::PluginStroke> selectedStrokes;
    require(getPluginStrokes("selection", selectedStrokes, strokeError) && selectedStrokes.size() == 1 &&
                    !selectedStrokes.front().page && !selectedStrokes.front().layer,
            "app.getStrokes('selection') did not return the selected stroke without page metadata");
    std::vector<prototype::PluginText> selectedTexts;
    std::vector<prototype::PluginLink> selectedLinks;
    std::vector<prototype::PluginImage> selectedImages;
    require(getPluginTexts("selection", selectedTexts, strokeError) && selectedTexts.size() == 1 &&
                    selectedTexts.front().reference == textOutput->reference &&
                    getPluginLinks("selection", selectedLinks, strokeError) && selectedLinks.size() == 1 &&
                    selectedLinks.front().reference == linkOutput->reference &&
                    getPluginImages("selection", selectedImages, strokeError) && selectedImages.size() == 1 &&
                    selectedImages.front().reference == imageOutput->reference,
            "Lua object queries did not filter selected editable objects by type");
    state.clearSelection();
    require(addPluginReferencesToSelection(
                    {sampleStrokeReference, textOutput->reference, linkOutput->reference, imageOutput->reference},
                    strokeError) &&
                    state.selected == std::vector<size_t>{state.ink.size() - 1} &&
                    state.selectedTexts == std::vector<size_t>({savedTextSize, savedTextSize + 1}) &&
                    state.selectedImages == std::vector<size_t>{savedImageSize},
            "app.addToSelection did not resolve current-layer references for all editable object types");
    std::vector<prototype::PluginStroke> pageStrokes;
    require(getPluginStrokes("page", pageStrokes, strokeError) && !state.hasSelection(),
            "app.getStrokes('page') did not clear the current selection");
    require(std::all_of(pageStrokes.begin(), pageStrokes.end(),
                        [](const auto& stroke) { return !stroke.page && stroke.layer.has_value(); }),
            "app.getStrokes('page') returned the wrong page/layer metadata");
    state.selected = {state.ink.size() - 1};
    state.selectedTexts = {savedTextSize};
    state.selectedImages = {savedImageSize};
    const auto creationSelection = state.selected;
    const auto creationTextSelection = state.selectedTexts;
    const auto creationImageSelection = state.selectedImages;
    const auto creationTextCount = state.texts.size();
    std::vector<std::uintptr_t> createdTextReferences;
    std::string creationError;
    prototype::PluginTextInput explicitText;
    explicitText.text = "Lua created note";
    explicitText.fontName = "Noto Sans";
    explicitText.fontSize = 9.5;
    explicitText.color = 0x1259b9;
    explicitText.x = 70;
    explicitText.y = 80;
    explicitText.wrap = 100;
    prototype::PluginTextInput defaultText;
    defaultText.text = "Lua default note";
    defaultText.x = 80;
    defaultText.y = 100;
    require(addPluginTexts({explicitText, defaultText}, prototype::PluginStrokeUndoPolicy::Grouped,
                           createdTextReferences, creationError) &&
                    createdTextReferences.size() == 2 && state.texts.size() == creationTextCount + 2 &&
                    state.selected == creationSelection && state.selectedTexts == creationTextSelection &&
                    state.selectedImages == creationImageSelection,
            creationError.empty() ? "app.addTexts did not add its batch with references while preserving selection" :
                                    creationError.c_str());
    const auto explicitTextIndex = creationTextCount;
    const auto& createdText = state.texts[explicitTextIndex];
    const auto& createdDefaultText = state.texts[explicitTextIndex + 1];
    require(createdText.text == "Lua created note" && createdText.fontName == "Noto Sans" &&
                    createdText.fontSize == 9.5 && createdText.color == 0xff1259b9U && createdText.wrapWidth == 100 &&
                    createdText.pluginReference == createdTextReferences[0] &&
                    createdText.layerId == state.activeLayerId(state.page) &&
                    createdDefaultText.fontName == state.textFontName &&
                    createdDefaultText.fontSize == state.textFontSize &&
                    createdDefaultText.color == packTextColor(state.textColor),
            "app.addTexts did not apply explicit and configured text defaults");
    state.undo();
    require(state.texts.size() == creationTextCount && state.selected == creationSelection,
            "Grouped app.addTexts batch did not undo in one step while preserving selection");
    state.redo();
    require(state.texts.size() == creationTextCount + 2 &&
                    state.texts[explicitTextIndex].pluginReference == createdTextReferences[0] &&
                    state.texts[explicitTextIndex + 1].pluginReference == createdTextReferences[1],
            "app.addTexts redo did not preserve returned references");
    state.undo();

    const auto creationLinkCount = state.texts.size();
    prototype::PluginLinkInput linkInput;
    linkInput.text = "Lua created link";
    linkInput.url = "https://example.test";
    linkInput.alignment = 1;
    linkInput.fontName = "Serif";
    linkInput.fontSize = 12;
    linkInput.color = 0xabcdef;
    linkInput.x = 90;
    linkInput.y = 110;
    std::vector<std::uintptr_t> createdLinkReferences;
    require(addPluginLinks({linkInput}, prototype::PluginStrokeUndoPolicy::Grouped, createdLinkReferences,
                           creationError) &&
                    createdLinkReferences.size() == 1 && state.texts.size() == creationLinkCount + 1,
            creationError.empty() ? "app.addLinks did not add the link and return its reference" :
                                    creationError.c_str());
    const auto& createdLink = state.texts.back();
    require(createdLink.isLink && createdLink.linkUrl == linkInput.url && createdLink.alignment == 1 &&
                    createdLink.fontName == "Serif" && createdLink.fontSize == 12 && createdLink.color == 0xffabcdefU &&
                    createdLink.pluginReference == createdLinkReferences[0],
            "app.addLinks did not preserve URL, alignment, style, or its opaque reference");
    state.undo();
    require(state.texts.size() == creationLinkCount, "app.addLinks should create one undoable edit");

    const auto creationImageCount = state.images.size();
    const auto creationSelectionBeforeImages = state.selected;
    const auto creationTextSelectionBeforeImages = state.selectedTexts;
    const auto pageSize = state.pageSize(state.page);
    const auto imageFixturePath = std::filesystem::path(options.smokeDirectory) / "plugin-image.png";
    {
        std::ofstream imageFixture(imageFixturePath, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(imageFixture), "Could not create the Lua image path fixture");
        imageFixture.write(pluginImageBytes.data(), static_cast<std::streamsize>(pluginImageBytes.size()));
        require(static_cast<bool>(imageFixture), "Could not write the Lua image path fixture");
    }

    prototype::PluginImageInput imageDataInput;
    imageDataInput.data = pluginImageBytes;
    imageDataInput.x = pageSize.width - 30;
    imageDataInput.y = pageSize.height - 20;
    imageDataInput.maxWidth = 40;
    imageDataInput.maxHeight = 20;
    imageDataInput.scale = 2;
    prototype::PluginImageInput missingImageInput;
    missingImageInput.path = (std::filesystem::path(options.smokeDirectory) / "missing-plugin-image.png").string();
    prototype::PluginImageInput imagePathInput;
    imagePathInput.path = imageFixturePath.string();
    imagePathInput.x = 10;
    imagePathInput.y = 15;
    imagePathInput.maxWidth = 18;
    imagePathInput.maxHeight = 10;
    imagePathInput.aspectRatio = false;
    imagePathInput.scale = 1.5;
    std::vector<prototype::PluginImageResult> createdImageResults;
    require(addPluginImages({imageDataInput, missingImageInput, imagePathInput},
                            prototype::PluginStrokeUndoPolicy::Grouped, createdImageResults, creationError) &&
                    createdImageResults.size() == 3 && createdImageResults[0].reference &&
                    !createdImageResults[1].reference && createdImageResults[1].error.find("Error:") == 0 &&
                    createdImageResults[2].reference && state.images.size() == creationImageCount + 2,
            creationError.empty() ? "app.addImages did not return per-input success and soft-error results" :
                                    creationError.c_str());
    const auto& createdDataImage = state.images[creationImageCount];
    const auto& createdPathImage = state.images[creationImageCount + 1];
    const auto dataImageBounds = prototype::canvasImageBounds(createdDataImage);
    const auto pathImageBounds = prototype::canvasImageBounds(createdPathImage);
    require(createdDataImage.encodedData == pluginImageBytes && createdDataImage.surface &&
                    createdDataImage.page == state.page &&
                    createdDataImage.layerId == state.activeLayerId(state.page) &&
                    createdDataImage.pluginReference == *createdImageResults[0].reference &&
                    std::abs(dataImageBounds.x0 - imageDataInput.x) < 1e-9 &&
                    std::abs(dataImageBounds.y0 - imageDataInput.y) < 1e-9 &&
                    std::abs((dataImageBounds.x1 - dataImageBounds.x0) - 20) < 1e-9 &&
                    std::abs((dataImageBounds.y1 - dataImageBounds.y0) - 20) < 1e-9 &&
                    createdPathImage.encodedData == pluginImageBytes && createdPathImage.surface &&
                    createdPathImage.pluginReference == *createdImageResults[2].reference &&
                    std::abs(pathImageBounds.x0 - imagePathInput.x) < 1e-9 &&
                    std::abs(pathImageBounds.y0 - imagePathInput.y) < 1e-9 &&
                    std::abs((pathImageBounds.x1 - pathImageBounds.x0) - 27) < 1e-9 &&
                    std::abs((pathImageBounds.y1 - pathImageBounds.y0) - 15) < 1e-9 && state.selected.empty() &&
                    state.selectedTexts.empty() &&
                    state.selectedImages == std::vector<size_t>({creationImageCount, creationImageCount + 1}),
            "app.addImages lost decoded bytes, active-layer metadata, scaling, page-fit sizing, or selection");
    const auto createdImageReferences =
            std::vector<std::uintptr_t>{*createdImageResults[0].reference, *createdImageResults[2].reference};
    state.undo();
    require(state.images.size() == creationImageCount && state.selected == creationSelectionBeforeImages &&
                    state.selectedTexts == creationTextSelectionBeforeImages &&
                    state.selectedImages == creationImageSelection,
            "Grouped app.addImages did not undo in one step and restore the prior selection");
    state.redo();
    require(state.images.size() == creationImageCount + 2 &&
                    state.images[creationImageCount].pluginReference == createdImageReferences[0] &&
                    state.images[creationImageCount + 1].pluginReference == createdImageReferences[1],
            "app.addImages redo did not preserve its opaque references");
    state.undo();

    const auto creationSplineStrokeCount = state.ink.size();
    prototype::PluginStrokeInput splineStroke;
    splineStroke.x = {210, 214, 220, 228};
    splineStroke.y = {40, 44, 47, 50};
    splineStroke.tool = "highlighter";
    splineStroke.width = 3.5;
    splineStroke.color = 0x456789;
    splineStroke.fill = 80;
    splineStroke.lineStyle = "plain";
    std::vector<std::uintptr_t> createdSplineReferences;
    require(addPluginStrokes({splineStroke}, prototype::PluginStrokeUndoPolicy::Grouped, createdSplineReferences,
                             creationError) &&
                    createdSplineReferences.size() == 1 && state.ink.size() == creationSplineStrokeCount + 1 &&
                    state.ink.back().points.size() == splineStroke.x.size() &&
                    state.ink.back().points.front().x == 210 && state.ink.back().points.back().y == 50 &&
                    state.ink.back().brush.tool == Tool::Highlighter && state.ink.back().fill == 80 &&
                    state.ink.back().pluginReference == createdSplineReferences.front() &&
                    state.selected == creationSelectionBeforeImages &&
                    state.selectedTexts == creationTextSelectionBeforeImages &&
                    state.selectedImages == creationImageSelection,
            creationError.empty() ? "The GTK host did not insert the flattened spline stroke on the active layer" :
                                    creationError.c_str());
    state.undo();
    require(state.ink.size() == creationSplineStrokeCount && state.selected == creationSelectionBeforeImages &&
                    state.selectedTexts == creationTextSelectionBeforeImages &&
                    state.selectedImages == creationImageSelection,
            "Grouped spline stroke insertion did not undo and restore the previous selection");
    state.redo();
    require(state.ink.size() == creationSplineStrokeCount + 1 &&
                    state.ink.back().pluginReference == createdSplineReferences.front(),
            "Spline stroke redo did not preserve its opaque reference");
    state.undo();

    state.ink.resize(savedInkSize);
    state.texts.resize(savedTextSize);
    state.images.resize(savedImageSize);
    state.selected = savedSelection;
    state.selectedTexts = savedTextSelection;
    state.selectedImages = savedImageSelection;
    update();
    const auto plugins = pluginRuntime->plugins();
    require(plugins.size() >= 12, "Bundled plugin discovery did not find the full master plugin set");
    const auto colorCycle = pluginRuntime->findPlugin("ColorCycle");
    require(colorCycle && colorCycle->valid && colorCycle->loaded && colorCycle->menuItems == 1,
            "Enabled bundled ColorCycle did not load and register its menu action");
    const auto toolShortcuts = pluginRuntime->findPlugin("ToolShortcuts");
    require(toolShortcuts && toolShortcuts->loaded && toolShortcuts->menuItems == 14,
            "Enabled bundled ToolShortcuts plugin did not register its complete menu set");
    verifySearchIntegration();
    const auto example = pluginRuntime->findPlugin("Example");
    require(example && example->loaded && example->menuItems == 1,
            "Enabled bundled Example plugin did not register its menu callback");
    const auto toggleGrid = pluginRuntime->findPlugin("ToggleGrid");
    require(toggleGrid && toggleGrid->loaded && toggleGrid->menuItems == 1 && nativeSession,
            "Enabled bundled ToggleGrid plugin did not load with a native document session");
    const auto highlightPosition = pluginRuntime->findPlugin("HighlightPosition");
    require(highlightPosition && highlightPosition->loaded && highlightPosition->menuItems == 1,
            "Enabled bundled HighlightPosition plugin did not register its menu action");
    const auto spaceForNotes = pluginRuntime->findPlugin("SpaceForNotes");
    require(spaceForNotes && spaceForNotes->loaded && spaceForNotes->menuItems == 2,
            "Enabled bundled SpaceForNotes plugin did not register both page insertion modes");
    const auto imageActions = pluginRuntime->findPlugin("ImageActions");
    require(imageActions && imageActions->loaded && imageActions->menuItems == 10,
            "Enabled bundled ImageActions plugin did not register all ten image operations");
    const auto fitToContent = pluginRuntime->findPlugin("FitToContent");
    require(fitToContent && fitToContent->loaded && fitToContent->menuItems == 2,
            "Enabled bundled FitToContent plugin did not register both page-fitting actions");
    const auto exportPlugin = pluginRuntime->findPlugin("Export");
    require(exportPlugin && exportPlugin->loaded && exportPlugin->menuItems == 3,
            "Enabled bundled Export plugin did not register PDF, SVG, and PNG actions");
    const auto quickScreenshotPlugin = pluginRuntime->findPlugin("QuickScreenshot");
    require(quickScreenshotPlugin && quickScreenshotPlugin->loaded && quickScreenshotPlugin->menuItems == 1,
            "Enabled bundled QuickScreenshot plugin did not register its capture action");
    if (const auto toolbarItem = pluginRuntime->findToolbarItem("Plugin::GTK4_TEST_ACTION")) {
        require(toolbarItem->pluginName == "PrivateToolbar" && toolbarItem->iconName == "document-open",
                "Plugin toolbar metadata did not preserve its ID, plugin, and icon");
        auto* button =
                findWidgetByData(floatingToolboxRowsContainer, "gtk4-plugin-toolbar-id", toolbarItem->id.c_str());
        require(button, "Configured plugin toolbar action did not render a button");
        require(gtk_widget_is_sensitive(button), "Configured plugin toolbar action button is disabled");
        const auto placeholder = pluginRuntime->findToolbarPlaceholder("Plugin::GTK4_TEST_STATUS");
        require(placeholder && placeholder->description == "Private plugin status" && placeholder->value == "READY",
                "Configured plugin toolbar placeholder did not retain its registration or initial value");
        auto* placeholderLabel =
                findWidgetByData(floatingToolboxRowsContainer, "gtk4-plugin-placeholder-id", placeholder->id.c_str());
        require(placeholderLabel && GTK_IS_LABEL(placeholderLabel) &&
                        std::string(gtk_label_get_text(GTK_LABEL(placeholderLabel))) == "READY",
                "Configured plugin toolbar placeholder did not render its initial value");
        const bool previousGridSnapping = state.snapGridEnabled;
        g_signal_emit_by_name(button, "clicked");
        require(state.snapGridEnabled != previousGridSnapping,
                "Rendered plugin toolbar button did not dispatch its Lua callback");
        require(std::string(gtk_label_get_text(GTK_LABEL(placeholderLabel))) == "CLICKED",
                "Plugin callback did not update the rendered toolbar placeholder");
        g_signal_emit_by_name(button, "clicked");
        require(state.snapGridEnabled == previousGridSnapping,
                "Plugin toolbar callback did not restore the previous grid-snapping state");
        require(std::string(gtk_label_get_text(GTK_LABEL(placeholderLabel))) == "GTK4_TEST_STATUS",
                "Empty plugin toolbar placeholder value did not fall back to its ID");
        std::cout << "PASS: GTK4 plugin toolbar action and placeholder registration, rendering, and updates\n";
    }
    verifyFloatingToolboxTextFormats();
    verifyFloatingToolboxTextAlignment();
    auto* menuModel = gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(pluginsMenuButton));
    require(menuModel && g_menu_model_get_n_items(menuModel) == 2,
            "Plugins menu is missing the enabled plugin group or manager action");
    auto* pluginSection = g_menu_model_get_item_link(menuModel, 0, G_MENU_LINK_SECTION);
    require(pluginSection && g_menu_model_get_n_items(pluginSection) == 10,
            "Plugins menu did not include each enabled plugin group");
    g_object_unref(pluginSection);
    require(g_action_group_has_action(G_ACTION_GROUP(window), "gtk4-plugin-0"),
            "Registered Lua menu action is missing from the application window");
    gtk_menu_button_popup(GTK_MENU_BUTTON(pluginsMenuButton));
    capture("plugins-menu");
    gtk_menu_button_popdown(GTK_MENU_BUTTON(pluginsMenuButton));
    verifyExportPlugin();

    require(pluginRuntime->handleKeyPress(GDK_KEY_v, GdkModifierType(0), false) &&
                    state.brush.tool == Tool::SmartSelect,
            "Bundled ToolShortcuts did not select Smart Select with its canvas shortcut");
    require(!pluginRuntime->handleKeyPress(GDK_KEY_p, GdkModifierType(0), true) &&
                    state.brush.tool == Tool::SmartSelect,
            "Bundled ToolShortcuts stole a key from text editing");
    require(pluginRuntime->handleKeyPress(GDK_KEY_p, GdkModifierType(0), false) && state.brush.tool == Tool::Pen,
            "Bundled ToolShortcuts did not restore the Pen tool");
    auto* application = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
    std::optional<std::string> highlightAction;
    for (guint index = 0; index < 64 && !highlightAction; ++index) {
        const auto actionName = "gtk4-plugin-" + std::to_string(index);
        const auto detailedActionName = "win." + actionName;
        gchar** accelerators = gtk_application_get_accels_for_action(application, detailedActionName.c_str());
        for (guint acceleratorIndex = 0; accelerators && accelerators[acceleratorIndex]; ++acceleratorIndex) {
            if (g_str_equal(accelerators[acceleratorIndex], "<Alt>x")) {
                highlightAction = actionName;
                break;
            }
        }
        g_strfreev(accelerators);
    }
    require(highlightAction.has_value(), "HighlightPosition did not register its Alt+X accelerator");
    g_action_group_activate_action(G_ACTION_GROUP(window), highlightAction->c_str(), nullptr);
    require(importedConfig.settings.highlightPosition,
            "HighlightPosition's Alt+X action did not enable the pointer halo");
    pointerPosition = prototype::Point{gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0};
    update();
    capture("position-highlight-on");
    prototype::ImportedSettings reloadedHighlightSettings;
    const auto highlightPreferences = prototype::loadGtk4Preferences(preferencesConfigDir, reloadedHighlightSettings);
    require(highlightPreferences.found && reloadedHighlightSettings.highlightPosition &&
                    reloadedHighlightSettings.cursorHighlightColor == importedConfig.settings.cursorHighlightColor &&
                    std::abs(reloadedHighlightSettings.cursorHighlightRadius -
                             importedConfig.settings.cursorHighlightRadius) < 1e-8,
            "Pointer highlight action or appearance settings did not persist");
    g_action_group_activate_action(G_ACTION_GROUP(window), highlightAction->c_str(), nullptr);
    require(!importedConfig.settings.highlightPosition,
            "HighlightPosition did not disable the pointer halo on its second activation");

    require(nativeSession && pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
            "SpaceForNotes integration needs the two-page PDF fixture");
    require(pluginRuntime->invokeMenu("SpaceForNotes", 0),
            "SpaceForNotes normal mode did not start from its plugin menu");
    waitForPluginDocumentJob();
    require(pageCount == 4 && state.page == 1 && nativeSession->pdfPageMap == std::vector<int>({0, -1, 1, -1}),
            "SpaceForNotes normal mode did not insert lined pages after consecutive PDF pages");
    require(state.layersForPage(1).backgroundFormat == "lined" && state.layersForPage(3).backgroundFormat == "lined",
            "SpaceForNotes normal mode did not preserve inserted page background metadata");
    performHistoryStep(false);
    waitForPluginDocumentJob();
    require(pageCount == 3 && nativeSession->pdfPageMap == std::vector<int>({0, 1, -1}),
            "Undo did not remove the latest SpaceForNotes page insertion");
    performHistoryStep(false);
    waitForPluginDocumentJob();
    require(pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
            "Undo did not remove the earlier SpaceForNotes page insertion");
    performHistoryStep(true);
    waitForPluginDocumentJob();
    require(pageCount == 3 && nativeSession->pdfPageMap == std::vector<int>({0, 1, -1}),
            "Redo did not restore the first SpaceForNotes page insertion");
    performHistoryStep(true);
    waitForPluginDocumentJob();
    require(pageCount == 4 && nativeSession->pdfPageMap == std::vector<int>({0, -1, 1, -1}),
            "Redo did not restore the second SpaceForNotes page insertion");

    navigateToPage(1);
    deleteCurrentPage();
    waitForPluginDocumentJob();
    require(pageCount == 3 && nativeSession->pdfPageMap == std::vector<int>({0, 1, -1}),
            "Deleting an inserted page did not update the native PDF page map");
    performHistoryStep(false);
    waitForPluginDocumentJob();
    require(pageCount == 4 && nativeSession->pdfPageMap == std::vector<int>({0, -1, 1, -1}) &&
                    state.layersForPage(1).backgroundFormat == "lined",
            "Undo did not restore the deleted note page and its background");
    performHistoryStep(true);
    waitForPluginDocumentJob();
    require(pageCount == 3 && nativeSession->pdfPageMap == std::vector<int>({0, 1, -1}),
            "Redo did not remove the restored note page");
    performHistoryStep(false);
    waitForPluginDocumentJob();
    require(pageCount == 4 && nativeSession->pdfPageMap == std::vector<int>({0, -1, 1, -1}),
            "A deleted page snapshot did not remain available for a second undo");
    capture("space-for-notes-pdf-gaps");
    require(pluginRuntime->invokeMenu("SpaceForNotes", 1),
            "SpaceForNotes force mode did not start from its plugin menu");
    waitForPluginDocumentJob();
    require(pageCount == 8 && state.page == 1 &&
                    nativeSession->pdfPageMap == std::vector<int>({0, -1, -1, -1, 1, -1, -1, -1}),
            "SpaceForNotes force mode did not add a page after every page in reverse-safe order");
    for (const auto page: {1, 3, 5, 7}) {
        require(state.layersForPage(page).backgroundFormat == "lined",
                "SpaceForNotes force mode did not preserve inserted page background metadata");
    }
    require(pluginRuntime->invokeMenu("SpaceForNotes", 0),
            "SpaceForNotes normal mode could not be invoked after note pages existed");
    require(pageCount == 8 && state.page == 0 &&
                    nativeSession->pdfPageMap == std::vector<int>({0, -1, -1, -1, 1, -1, -1, -1}),
            "SpaceForNotes normal mode duplicated note pages or failed to restore the first page");
    const auto notesRoundTripPath =
            (std::filesystem::path(options.smokeDirectory) / "space-for-notes-round-trip.xopp").string();
    nativeSession->saveCopy(options.worker, notesRoundTripPath, prototype::bridge::transferInk(state.committedInk()),
                            state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                            currentPageSizes(state));
    const prototype::bridge::DocumentSession reopenedNotes(options.worker, notesRoundTripPath);
    require(reopenedNotes.pdfPageMap == std::vector<int>({0, -1, -1, -1, 1, -1, -1, -1}),
            "Saving and reopening a SpaceForNotes document changed PDF and note page placement");
    require(reopenedNotes.initialLayers.size() == 8 && reopenedNotes.initialLayers[1].backgroundFormat == "lined" &&
                    reopenedNotes.initialLayers[3].backgroundFormat == "lined" &&
                    reopenedNotes.initialLayers[5].backgroundFormat == "lined" &&
                    reopenedNotes.initialLayers[7].backgroundFormat == "lined",
            "Saving and reopening a SpaceForNotes document changed note page backgrounds");

    constexpr std::array<std::uint32_t, 11> expectedColors{
            0x000000, 0x008000, 0x00c0ff, 0x00ff00, 0x3333cc, 0x808080,
            0xff0000, 0xff00ff, 0xff8000, 0xffff00, 0xffffff,
    };
    for (const auto expected: expectedColors) {
        g_action_group_activate_action(G_ACTION_GROUP(window), "gtk4-plugin-0", nullptr);
        const std::array<double, 3> expectedRgb{
                static_cast<double>((expected >> 16U) & 0xffU) / 255.0,
                static_cast<double>((expected >> 8U) & 0xffU) / 255.0,
                static_cast<double>(expected & 0xffU) / 255.0,
        };
        require(penBrush.color == expectedRgb, "Bundled ColorCycle did not match its full legacy color sequence");
    }
    Ink selectedStroke;
    selectedStroke.page = 0;
    selectedStroke.layerId = 1;
    selectedStroke.originLayer = 0;
    selectedStroke.brush = {Tool::Pen, {1, 0, 0}, 2};
    selectedStroke.points = {{40, 40, 1}, {50, 50, 1}};
    state.ink.push_back(selectedStroke);
    state.selected = {state.ink.size() - 1};
    require(pluginRuntime->invokeMenu("ColorCycle", 0) &&
                    state.ink.back().brush.color == std::array<double, 3>{0, 0, 0},
            "ColorCycle did not recolor a selected stroke through the legacy selection API");
    state.undo();
    require(state.ink.back().brush.color == selectedStroke.brush.color,
            "ColorCycle selection recoloring was not undoable");
    state.clearSelection();
    state.ink.pop_back();

    navigateToPage(0);
    require(pluginRuntime->invokeMenu("ToggleGrid", 0), "ToggleGrid menu action did not execute");
    waitForPluginDocumentJob();
    require(state.layersForPage(0).backgroundFormat == "plain" && !state.snapGridEnabled,
            "ToggleGrid did not set the native page to plain paper and disable grid snapping");
    capture("toggle-grid-plain");
    require(pluginRuntime->invokeMenu("ToggleGrid", 0), "ToggleGrid second menu action did not execute");
    waitForPluginDocumentJob();
    require(state.layersForPage(0).backgroundFormat == "graph" && state.snapGridEnabled,
            "ToggleGrid did not set the native page to graph paper and enable grid snapping");
    capture("toggle-grid-graph");
    state.undo();
    waitForPluginDocumentJob();
    require(state.layersForPage(0).backgroundFormat == "plain",
            "Undo did not restore the prior page background through the native backend");
    state.redo();
    waitForPluginDocumentJob();
    require(state.layersForPage(0).backgroundFormat == "graph",
            "Redo did not restore the graph page background through the native backend");
    const auto roundTripPath = (std::filesystem::path(options.smokeDirectory) / "toggle-grid-round-trip.xopp").string();
    nativeSession->saveCopy(options.worker, roundTripPath, prototype::bridge::transferInk(state.committedInk()),
                            state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                            currentPageSizes(state));
    const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
    require(!reopened.initialLayers.empty() && reopened.initialLayers[0].backgroundFormat == "graph",
            "Saving and reopening a ToggleGrid document did not preserve its graph background");

    verifyImageActionsPlugin();

    g_action_group_activate_action(G_ACTION_GROUP(window), "gtk4-plugin-1", nullptr);
    require(messageDialog &&
                    g_strcmp0(adw_alert_dialog_get_response_label(messageDialog, "plugin-response-2"), "No") == 0,
            "Example plugin did not open its GTK dialog with the legacy button labels");
    answerDialog("plugin-response-2");
    require(messageDialog == nullptr, "Example plugin dialog did not close after choosing a response");

    openPluginManager();
    require(pluginManagerWindow && gtk_widget_get_visible(GTK_WIDGET(pluginManagerWindow)),
            "Manage Plugins action did not present the GTK manager window");
    std::size_t rowCount = 0;
    for (auto* row = gtk_widget_get_first_child(pluginManagerList); row; row = gtk_widget_get_next_sibling(row)) {
        ++rowCount;
    }
    require(rowCount == plugins.size(), "Plugin manager did not show every discovered plugin");
    const auto enabledAgain = pluginRuntime->findPlugin("ColorCycle");
    require(enabledAgain && enabledAgain->pendingEnabled, "Plugin manager did not preserve the current enabled state");
    captureTopLevel("plugins-manager", GTK_WIDGET(pluginManagerWindow));

    const auto exampleSwitch = pluginManagerSwitches.find("Example");
    require(exampleSwitch != pluginManagerSwitches.end() && example,
            "Plugin manager is missing the bundled Example plugin control");
    gtk_switch_set_active(GTK_SWITCH(exampleSwitch->second), false);
    g_signal_emit_by_name(pluginManagerApplyButton, "clicked");
    require(messageDialog && importedConfig.settings.pluginsEnabled.find(example->path.string()) == std::string::npos &&
                    !pluginRuntime->findPlugin("Example")->pendingEnabled,
            "Applying plugin changes did not persist the disabled plugin state");
    answerDialog("close");
    verifyFitToContentPlugin();
    verifyQuickScreenshotPlugin();
    std::cout << "PASS: private GTK4 ColorCycle, Example dialog, ToolShortcuts, Smart Select shortcut, "
                 "HighlightPosition, QuickScreenshot region capture/clipboard, Export PDF/SVG/PNG, ImageActions, "
                 "FitToContent page resizing, "
                 "SpaceForNotes page insertion, "
                 "ToggleGrid paper changes, and background undo/redo\n";
}

}  // namespace prototype::gtk4
