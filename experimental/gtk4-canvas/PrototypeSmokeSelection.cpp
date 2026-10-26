#include "Prototype.h"

namespace prototype::gtk4 {

gboolean Prototype::stepCrashHelper() {
    require(++crashHelperTicks < 240, "Crash helper timed out waiting for autosave");
    if (busy || !frames || canvasRenderer.empty() || !nativeSession) {
        return G_SOURCE_CONTINUE;
    }
    if (crashHelperStep == 0) {
        const auto x = state.offsetX + 90 * state.zoom;
        const auto y = state.offsetY + 120 * state.zoom;
        begin(Device::Stylus, x, y, 0.25);
        motion(Device::Stylus, x + 50 * state.zoom, y + 25 * state.zoom, 0.75);
        finish(Device::Stylus, 0.75);
        require(modified() && state.ink.size() == 1 && state.ink[0].pressureSensitive,
                "Crash helper did not record pressure-sensitive stroke");
        crashHelperStep = 1;
        return G_SOURCE_CONTINUE;
    }
    if (autosaving || autosavedRevision != state.revision() || activeAutosavePath.empty()) {
        return G_SOURCE_CONTINUE;
    }
    require(std::filesystem::is_regular_file(activeAutosavePath), "Crash helper autosave file missing");
    const auto temporary = crashMarkerPath + ".tmp";
    {
        std::ofstream marker(temporary, std::ios::binary);
        marker << activeAutosavePath << '\n';
        marker.close();
        require(bool(marker), "Cannot write crash helper marker");
    }
    std::filesystem::rename(temporary, crashMarkerPath);
    crashHelperSource = 0;
    return G_SOURCE_REMOVE;
}

gboolean Prototype::stepRecoveryVerify() {
    require(++recoveryVerifyTicks < 240, "Recovery verifier timed out");
    if (busy || deferredAction) {
        return G_SOURCE_CONTINUE;
    }
    if (recoveryVerifyStep == 0) {
        if (!messageDialog) {
            return G_SOURCE_CONTINUE;
        }
        if (recoveryActionSpec.starts_with("restore:")) {
            answerDialog("restore");
        } else if (recoveryActionSpec == "delete") {
            answerDialog("delete");
        } else if (recoveryActionSpec == "recover") {
            answerDialog("recover");
        } else if (recoveryActionSpec == "discard") {
            answerDialog("discard");
        } else {
            throw std::runtime_error("Unknown XOURNALPP_RECOVERY_ACTION");
        }
        recoveryVerifyStep = 1;
        return G_SOURCE_CONTINUE;
    }
    if (recoveryVerifyStep == 1) {
        if (!frames || canvasRenderer.empty() || g_get_monotonic_time() - lastDocumentCompletion < 150000) {
            return G_SOURCE_CONTINUE;
        }
        if (recoveryActionSpec == "delete" || recoveryActionSpec == "discard") {
            require(!modified() && activeAutosavePath.empty(),
                    "Expected clean state after deleting/discarding recovery file");
            closeWindow();
            recoveryVerifySource = 0;
            return G_SOURCE_REMOVE;
        }
        if (recoveryActionSpec.starts_with("restore:")) {
            require(modified() && recoveredChanges && saveTarget.empty() && !activeAutosavePath.empty() &&
                            std::filesystem::is_regular_file(activeAutosavePath),
                    "Expected unnamed dirty restored document with adopted cache autosave");
            smokeSaveChoice = recoveryActionSpec.substr(std::string_view("restore:").size());
            save([this] { closeWindow(); });
        } else {
            require(modified() && recoveredChanges && !saveTarget.empty() && !activeAutosavePath.empty() &&
                            std::filesystem::is_regular_file(activeAutosavePath),
                    "Expected associated dirty recovered document");
            save([this] { closeWindow(); });
        }
        recoveryVerifyStep = 2;
        return G_SOURCE_CONTINUE;
    }
    if (!closed) {
        return G_SOURCE_CONTINUE;
    }
    recoveryVerifySource = 0;
    return G_SOURCE_REMOVE;
}

gboolean Prototype::smokeLayers() {
    switch (layerVerifyStep++) {
        case 0: {
            require(nativeSession && pageCount == 2 && state.layersForPage(0).layers.size() == 2,
                    "Layer verification requires a native two-layer document");
            layerVerifyInitialInk = state.ink.size();
            require(state.layersForPage(0).layers[0].name == "Lower" &&
                            state.layersForPage(0).layers[1].name == "Upper",
                    "Native layer names were not loaded into the sidebar model");
            capture("layer-stack");
            auto* upper = findLayerControl(2, false);
            require(upper, "Upper layer is missing from the layer sidebar");
            g_signal_emit_by_name(upper, "clicked");
            require(state.activeLayerId(0) == 2, "Clicking a layer row did not select it");
            g_signal_emit_by_name(layerAddButton, "clicked");
            require(state.layersForPage(0).layers.size() == 3 && state.activeLayerId(0) == 3 &&
                            state.activeLayerOrigin(0) == 2,
                    "Add layer did not insert above the selected native layer");
            break;
        }
        case 1: {
            capture("layer-added");
            require(gtk_widget_get_sensitive(layerMergeButton), "Merge down was unavailable for the top layer");
            g_signal_emit_by_name(layerMergeButton, "clicked");
            require(state.layersForPage(0).layers.size() == 2 && state.activeLayerId(0) == 2 &&
                            state.layersForPage(0).layers[1].origins == std::vector<size_t>({1, 2}),
                    "Merge down did not preserve source origins in bottom-to-top order");
            break;
        }
        case 2: {
            capture("layer-merged");
            auto* upperVisibility = findLayerControl(2, true);
            require(upperVisibility, "Upper layer visibility control is missing");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(upperVisibility), false);
            require(!state.layerVisible(0, 2), "Visibility control did not hide the selected layer");
            break;
        }
        case 3: {
            capture("layer-hidden");
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.layerVisible(0, 2), "Undo did not restore layer visibility");
            const auto start = state.pageScreenPoint(0, 120, 120);
            const auto end = state.pageScreenPoint(0, 170, 120);
            g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
            g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
            g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            require(state.ink.size() == layerVerifyInitialInk + 1 && state.ink.back().layerId == 2 &&
                            state.ink.back().originLayer == 2,
                    "New ink did not follow the active merged layer origin");
            g_signal_emit_by_name(layerRenameBackgroundButton, "clicked");
            require(messageDialog && layerRenameEntry, "Rename Background did not open its dialog");
            break;
        }
        case 4: {
            gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), "Paper");
            answerDialog("rename");
            require(state.layersForPage(0).backgroundName == "Paper",
                    "Renaming the background did not update its explicit name");
            break;
        }
        case 5: {
            capture("layer-background-renamed");
            g_signal_emit_by_name(layerRenameButton, "clicked");
            require(messageDialog && layerRenameEntry, "Rename Layer did not open its dialog");
            break;
        }
        case 6: {
            gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), "Upper renamed");
            answerDialog("rename");
            require(state.layersForPage(0).layers[1].name == "Upper renamed",
                    "Renaming the selected layer did not update its name");
            break;
        }
        case 7: {
            capture("layer-renamed");
            g_signal_emit_by_name(layerDuplicateButton, "clicked");
            layerVerifyDuplicateInkStart = state.ink.size() - 2;
            require(state.layersForPage(0).layers.size() == 3 && state.activeLayerId(0) == 3 &&
                            state.layersForPage(0).layers[1].origins == std::vector<size_t>({1, 2}) &&
                            state.layersForPage(0).layers[2].origins == std::vector<size_t>({1, 2}) &&
                            state.ink.size() == layerVerifyInitialInk + 3 &&
                            state.ink[layerVerifyDuplicateInkStart].layerId == 3 &&
                            state.ink[layerVerifyDuplicateInkStart + 1].layerId == 3,
                    "Duplicating a merged layer did not copy its origins and editable ink");
            g_signal_emit_by_name(layerRenameButton, "clicked");
            require(messageDialog && layerRenameEntry, "Renaming a duplicate did not open its dialog");
            break;
        }
        case 8: {
            gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), "Duplicate");
            answerDialog("rename");
            break;
        }
        case 9: {
            capture("layer-duplicated");
            g_signal_emit_by_name(layerMoveDownButton, "clicked");
            require(state.activeLayerId(0) == 2 && state.layersForPage(0).layers[1].name == "Duplicate" &&
                            state.ink[layerVerifyDuplicateInkStart].layerId == 2 &&
                            state.ink[layerVerifyDuplicateInkStart + 1].layerId == 2,
                    "Moving the duplicate down did not update stack order and object IDs");
            break;
        }
        case 10: {
            capture("layer-moved-down");
            g_signal_emit_by_name(layerMoveUpButton, "clicked");
            require(state.activeLayerId(0) == 3 && state.layersForPage(0).layers[2].name == "Duplicate" &&
                            state.ink[layerVerifyDuplicateInkStart].layerId == 3 &&
                            state.ink[layerVerifyDuplicateInkStart + 1].layerId == 3,
                    "Moving the duplicate up did not restore stack order and object IDs");
            break;
        }
        case 11: {
            g_signal_emit_by_name(layerHideAllButton, "clicked");
            require(!state.layerVisible(0, 1) && !state.layerVisible(0, 2) && !state.layerVisible(0, 3),
                    "Hide All Layers did not hide every foreground layer");
            break;
        }
        case 12: {
            capture("layers-hidden-all");
            g_signal_emit_by_name(layerShowAllButton, "clicked");
            require(state.layerVisible(0, 1) && state.layerVisible(0, 2) && state.layerVisible(0, 3),
                    "Show All Layers did not show every foreground layer");
            break;
        }
        case 13: {
            g_signal_emit_by_name(undoButton, "clicked");
            require(!state.layerVisible(0, 1) && !state.layerVisible(0, 2) && !state.layerVisible(0, 3),
                    "Undo did not restore the prior all-hidden state");
            g_signal_emit_by_name(redoButton, "clicked");
            require(state.layerVisible(0, 1) && state.layerVisible(0, 2) && state.layerVisible(0, 3),
                    "Redo did not restore the all-visible state");
            break;
        }
        case 14: {
            capture("layers-shown-all");
            const auto saved = (std::filesystem::path(options.smokeDirectory) / "layer-saved.xopp").string();
            saveCopyTo(saved);
            require(busy, "Saving the layer verification document did not start");
            break;
        }
        default: {
            const auto saved = std::filesystem::path(options.smokeDirectory) / "layer-saved.xopp";
            require(!modified() && savedCopyPath == saved.string() && std::filesystem::is_regular_file(saved),
                    "Layer-aware save did not complete cleanly");
            capture("layer-saved");
            std::cout
                    << "PASS: GTK layer selection, add, rename, duplicate, reorder, visibility, merge, undo and save\n";
            closeWindow();
            return G_SOURCE_REMOVE;
        }
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

gboolean Prototype::smokeSelectionModes() {
    switch (selectionVerifyStep++) {
        case 0: {
            require(nativeSession && pageCount == 1 && state.layersForPage(0).layers.size() == 2 &&
                            state.ink.size() == 4 && state.texts.size() == 1 && state.texts[0].layoutWidth > 0 &&
                            state.texts[0].layoutHeight > 0 && state.layersForPage(0).layers[0].name == "Lower" &&
                            state.layersForPage(0).layers[1].name == "Upper",
                    "Visible-layer selection verification requires its native two-layer fixture");
            require(g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(selectionModeDropdown))) == 9,
                    "Selection mode menu does not expose visible-layer, object, PDF text and vertical-space modes");
            activateSelection(Tool::SelectRectangle);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
            require(state.brush.tool == Tool::SelectRectangle && state.selectionAcrossLayers,
                    "Visible-layer rectangle mode did not activate multilayer selection");
            state.clearSelection();
            {
                const auto start = state.pageScreenPoint(0, 45, 95);
                const auto end = state.pageScreenPoint(0, 270, 230);
                g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            }
            require(state.selected == std::vector<size_t>({2, 3}),
                    "Visible-layer rectangle did not select all matching strokes from the upper layer only");
            require(state.selectedTexts == std::vector<size_t>{0},
                    "Visible-layer rectangle did not include native text from the matching upper layer");
            break;
        }
        case 1: {
            capture("selection-visible-layer-rectangle");
            state.clearSelection();
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 4);
            require(state.brush.tool == Tool::SelectLasso && state.selectionAcrossLayers,
                    "Visible-layer lasso mode did not activate multilayer selection");
            const auto start = state.pageScreenPoint(0, 45, 95);
            const std::array<prototype::Point, 4> polygon{{{270, 95}, {270, 230}, {45, 230}, {45, 95}}};
            const auto end = state.pageScreenPoint(0, polygon.back().x, polygon.back().y);
            g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
            for (const auto& point: polygon) {
                const auto screen = state.pageScreenPoint(0, point.x, point.y);
                g_signal_emit_by_name(mouse, "drag-update", screen.x - start.x, screen.y - start.y);
            }
            g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            require(state.selected == std::vector<size_t>({2, 3}),
                    "Visible-layer lasso did not select all matching strokes from the upper layer only");
            require(state.selectedTexts == std::vector<size_t>{0},
                    "Visible-layer lasso did not include native text from the matching upper layer");
            break;
        }
        case 2: {
            capture("selection-visible-layer-lasso");
            state.clearSelection();
            auto* upperVisibility = findLayerControl(2, true);
            require(upperVisibility, "Upper-layer visibility control is missing");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(upperVisibility), false);
            require(!state.layerVisible(0, 2), "Upper layer did not become hidden before fallback selection");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
            const auto start = state.pageScreenPoint(0, 45, 95);
            const auto end = state.pageScreenPoint(0, 270, 230);
            g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
            g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
            g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            require(state.selected == std::vector<size_t>({0, 1}),
                    "Visible-layer rectangle did not fall back to the next visible matching layer");
            require(state.selectedTexts.empty(),
                    "Hidden upper-layer text leaked into the visible-layer fallback selection");
            break;
        }
        case 3: {
            capture("selection-hidden-layer-fallback");
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.layerVisible(0, 2), "Undo did not restore the upper layer visibility");
            state.clearSelection();
            {
                const auto start = state.pageScreenPoint(0, 68, 206);
                const auto end = state.pageScreenPoint(0, 270, 230);
                g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            }
            require(state.selected.empty() && state.selectedTexts == std::vector<size_t>{0},
                    "Rectangle selection did not isolate the native text object");
            selectionVerifyTextX = state.texts[0].x;
            break;
        }
        case 4: {
            capture("selection-text-object");
            {
                const auto start = state.pageScreenPoint(0, 95, 213);
                const auto end = state.pageScreenPoint(0, 113, 223);
                g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            }
            require(std::abs(state.texts[0].x - (selectionVerifyTextX + 18)) < 1e-6 &&
                            std::abs(state.texts[0].y - 218) < 1e-6 && state.selectedTexts == std::vector<size_t>{0},
                    "Dragging selected native text did not move it while preserving selection");
            break;
        }
        case 5: {
            capture("selection-text-moved");
            g_signal_emit_by_name(undoButton, "clicked");
            require(std::abs(state.texts[0].x - selectionVerifyTextX) < 1e-6 &&
                            std::abs(state.texts[0].y - 208) < 1e-6 && state.selectedTexts == std::vector<size_t>{0},
                    "Undo did not restore the native text position and selection");

            auto* upperLayer = findLayerControl(2, false);
            require(upperLayer, "Upper layer selector is missing for Text-tool verification");
            g_signal_emit_by_name(upperLayer, "clicked");

            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 2);
            require(state.brush.tool == Tool::SmartSelect && !state.selectionAcrossLayers,
                    "Smart Select was not available for native hit testing");
            state.clearSelection();
            const bool oldSnapGridEnabled = state.snapGridEnabled;
            const double oldSnapGridSize = state.snapGridSize;
            const double oldSnapGridTolerance = state.snapGridTolerance;
            state.snapGridEnabled = false;
            const auto edgeTextBefore = state.texts[0];
            const auto edgeStart = state.pageScreenPoint(0, edgeTextBefore.x + edgeTextBefore.layoutWidth + 0.25,
                                                         edgeTextBefore.y + edgeTextBefore.layoutHeight / 2);
            begin(Device::Mouse, edgeStart.x, edgeStart.y);
            require(state.movingSelection() && state.selectedTexts == std::vector<size_t>{0},
                    "Smart Select missed native text within the 0.5 point edge hit radius");
            motion(Device::Mouse, edgeStart.x + 10 * state.zoom, edgeStart.y);
            finish(Device::Mouse, std::nullopt, prototype::Point{edgeStart.x + 10 * state.zoom, edgeStart.y});
            require(std::abs(state.texts[0].x - edgeTextBefore.x - 10) < 1e-6 &&
                            state.selectedTexts == std::vector<size_t>{0},
                    "Smart Select edge hit did not move the native text object");
            waitForNextPaint();
            capture("smart-select-edge-hit");
            state.undo();
            require(std::abs(state.texts[0].x - edgeTextBefore.x) < 1e-6,
                    "Undo did not restore the Smart Select edge-hit move");

            state.snapGridEnabled = true;
            state.snapGridSize = 10;
            state.snapGridTolerance = 0.5;
            const auto selectionBounds = state.selectionBox();
            require(selectionBounds && selectionBounds->page == 0,
                    "Smart Select grid-snap verification lost the text selection");
            const auto oldGridXOffset = state.pageLayers[0].gridXOffset;
            const auto oldGridYOffset = state.pageLayers[0].gridYOffset;
            const auto oldGridXStep = state.pageLayers[0].gridXStep;
            const auto oldGridYStep = state.pageLayers[0].gridYStep;
            const auto gridStart = state.pageScreenPoint(
                    0, selectionBounds->x1 - 7, selectionBounds->y0 + (selectionBounds->y1 - selectionBounds->y0) / 4);
            const auto gridAnchorX = selectionBounds->x1;
            const auto gridAnchorY = selectionBounds->y0;
            state.pageLayers[0].gridXOffset = gridAnchorX + 10;
            state.pageLayers[0].gridYOffset = gridAnchorY + 10;
            state.pageLayers[0].gridXStep = 0;
            state.pageLayers[0].gridYStep = 0;
            begin(Device::Mouse, gridStart.x, gridStart.y);
            motion(Device::Mouse, gridStart.x + 11 * state.zoom, gridStart.y + 11 * state.zoom);
            finish(Device::Mouse, std::nullopt,
                   prototype::Point{gridStart.x + 11 * state.zoom, gridStart.y + 11 * state.zoom});
            require(std::abs(state.texts[0].x - edgeTextBefore.x - 10) < 1e-6 &&
                            std::abs(state.texts[0].y - edgeTextBefore.y - 10) < 1e-6,
                    "Smart Select move did not snap its nearest selection corner to the page grid");
            waitForNextPaint();
            capture("smart-select-grid-snapped");
            state.undo();
            state.pageLayers[0].gridXOffset = oldGridXOffset;
            state.pageLayers[0].gridYOffset = oldGridYOffset;
            state.pageLayers[0].gridXStep = oldGridXStep;
            state.pageLayers[0].gridYStep = oldGridYStep;
            state.snapGridEnabled = oldSnapGridEnabled;
            state.snapGridSize = oldSnapGridSize;
            state.snapGridTolerance = oldSnapGridTolerance;
            require(std::abs(state.texts[0].x - edgeTextBefore.x) < 1e-6 &&
                            std::abs(state.texts[0].y - edgeTextBefore.y) < 1e-6,
                    "Undo did not restore the Smart Select grid-snapped move");

            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
            require(state.brush.tool == Tool::SelectRectangle && state.selectionAcrossLayers,
                    "Restoring the remembered visible-layer selection mode failed");
            selectTool(Tool::Text);
            require(state.brush.tool == Tool::Text && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(textButton)) &&
                            std::none_of(toolButtons.begin(), toolButtons.end(),
                                         [](GtkWidget* button) {
                                             return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
                                         }) &&
                            !gtk_widget_get_visible(selectionModeDropdown),
                    "Selecting the Text tool left another tool selected in the toolbar");
            const auto originalText = state.texts[0];
            const auto editPoint = state.pageScreenPoint(0, originalText.x + originalText.layoutWidth / 2,
                                                         originalText.y + originalText.layoutHeight / 2);
            g_signal_emit_by_name(textClick, "released", 1, editPoint.x, editPoint.y);
            require(textEditor->active() && gtk_widget_get_visible(textFontEntry) &&
                            gtk_widget_get_visible(textFormatBox) && gtk_widget_get_visible(colorBox),
                    "Text editing did not expose the quick-format toolbar controls");
            const GdkRGBA textColor{208.0f / 255, 32.0f / 255, 16.0f / 255, 1};
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &textColor);
            finishTextEditing();
            require(state.texts[0].color == 0xffd02010U && state.selectedTexts == std::vector<size_t>{0},
                    "Text color editing did not persist or leave the edited text selected");
            waitForNextPaint();
            capture("text-tool-edited");

            const auto beforeMove = state.texts[0];
            const auto moveStart = state.pageScreenPoint(0, beforeMove.x + beforeMove.layoutWidth / 2,
                                                         beforeMove.y + beforeMove.layoutHeight / 2);
            g_signal_emit_by_name(textClick, "pressed", 1, moveStart.x, moveStart.y);
            g_signal_emit_by_name(mouse, "drag-begin", moveStart.x, moveStart.y);
            require(state.movingSelection() && state.selectedTexts == std::vector<size_t>{0},
                    "Text-tool drag did not begin moving the edited text");
            g_signal_emit_by_name(mouse, "drag-update", 18.0 * state.zoom, 10.0 * state.zoom);
            g_signal_emit_by_name(mouse, "drag-end", 18.0 * state.zoom, 10.0 * state.zoom);
            g_signal_emit_by_name(textClick, "released", 1, moveStart.x, moveStart.y);
            require(!textEditor->active() && std::abs(state.texts[0].x - beforeMove.x - 18) < 1e-6 &&
                            std::abs(state.texts[0].y - beforeMove.y - 10) < 1e-6 &&
                            state.selectedTexts == std::vector<size_t>{0},
                    "Text-tool drag did not move the object or suppressed click-to-edit incorrectly");
            waitForNextPaint();
            capture("text-tool-moved");

            const auto beforeResize = state.texts[0];
            const auto handle = state.selectionHandles()[4];
            const auto resizeStart = state.pageScreenPoint(0, handle.x, handle.y);
            g_signal_emit_by_name(textClick, "pressed", 1, resizeStart.x, resizeStart.y);
            g_signal_emit_by_name(mouse, "drag-begin", resizeStart.x, resizeStart.y);
            require(state.movingSelection(), "Text-tool corner drag did not start a resize");
            g_signal_emit_by_name(mouse, "drag-update", 18.0 * state.zoom, 18.0 * state.zoom);
            g_signal_emit_by_name(mouse, "drag-end", 18.0 * state.zoom, 18.0 * state.zoom);
            g_signal_emit_by_name(textClick, "released", 1, resizeStart.x, resizeStart.y);
            require(std::abs(state.texts[0].xx - beforeResize.xx) > 1e-5 &&
                            state.selectedTexts == std::vector<size_t>{0},
                    "Text-tool corner drag did not resize text while keeping the selection active");
            waitForNextPaint();
            capture("text-tool-resized");

            const auto stylusTap = state.pageScreenPoint(0, state.texts[0].x + state.texts[0].layoutWidth / 2,
                                                         state.texts[0].y + state.texts[0].layoutHeight / 2);
            begin(Device::Stylus, stylusTap.x, stylusTap.y);
            require(pendingStylusTextTap.has_value(), "A stylus tap did not wait for click-versus-drag routing");
            finish(Device::Stylus, std::nullopt, stylusTap);
            require(textEditor->active(), "A stylus tap on text did not open the inline editor");
            const auto tappedText = textEditor->finish();
            require(tappedText && tappedText->existingIndex == std::optional<size_t>{0},
                    "Stylus text editing lost the existing object identity");
            updateTextToolbarVisibility();

            const auto beforeStylusMove = state.texts[0];
            const auto stylusStart = state.pageScreenPoint(0, state.texts[0].x + state.texts[0].layoutWidth / 2,
                                                           state.texts[0].y + state.texts[0].layoutHeight / 2);
            begin(Device::Stylus, stylusStart.x, stylusStart.y);
            motion(Device::Stylus, stylusStart.x + 14.0 * state.zoom, stylusStart.y + 8.0 * state.zoom);
            require(state.movingSelection(), "A stylus drag did not begin moving text with the Text tool");
            finish(Device::Stylus, std::nullopt,
                   prototype::Point{stylusStart.x + 14.0 * state.zoom, stylusStart.y + 8.0 * state.zoom});
            require(std::abs(state.texts[0].x - beforeStylusMove.x - 14) < 1e-6 &&
                            std::abs(state.texts[0].y - beforeStylusMove.y - 8) < 1e-6 &&
                            state.selectedTexts == std::vector<size_t>{0},
                    "Stylus text drag did not commit with the text selected");
            waitForNextPaint();
            capture("text-tool-stylus-moved");

            gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), 18);
            require(state.texts[0].fontSize == 18, "The font-size toolbar control did not update selected text");
            g_signal_emit_by_name(textFormatButtons[0], "clicked");
            require(!state.texts[0].styleRuns.empty(),
                    "The style toolbar did not format selected text outside the inline editor");
            const GdkRGBA selectedColor{32.0f / 255, 80.0f / 255, 210.0f / 255, 1};
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &selectedColor);
            require(state.texts[0].color == 0xff2050d2U, "The color toolbar did not recolor a selected text object");
            state.undo();
            state.undo();
            state.undo();
            state.undo();
            state.undo();
            state.undo();
            state.undo();
            require(state.texts[0].color == originalText.color && state.texts[0].fontSize == originalText.fontSize &&
                            std::abs(state.texts[0].x - originalText.x) < 1e-6 &&
                            std::abs(state.texts[0].y - originalText.y) < 1e-6 &&
                            std::abs(state.texts[0].xx - originalText.xx) < 1e-6 &&
                            state.texts[0].styleRuns == originalText.styleRuns,
                    "Undo did not restore the edited text, move, resize and toolbar formatting sequence");

            selectTool(Tool::Pen);
            require(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(textButton)),
                    "Switching away from Text left the Text tool selected in the header");
            require(state.selectionAcrossLayers, "Changing drawing tools discarded the remembered selection mode");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
            require(state.brush.tool == Tool::SmartSelect,
                    "The main selection tool button did not reopen Smart Select");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
            require(state.brush.tool == Tool::SelectRectangle && state.selectionAcrossLayers &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 3,
                    "The selection mode menu did not restore the visible-layer rectangle mode");
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_r, 0u,
                                  GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
            require(handled && state.brush.tool == Tool::SelectRectangle && !state.selectionAcrossLayers &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 0,
                    "The rectangle keyboard shortcut did not select the regular single-layer tool");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 4);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 2);
            require(state.brush.tool == Tool::SmartSelect && !state.selectionAcrossLayers &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 2,
                    "Smart Select incorrectly retained the multilayer scope");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 5);
            require(state.brush.tool == Tool::SelectObject && !state.selectionAcrossLayers &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 5,
                    "Select Object mode did not activate as a distinct tool");
            selectTool(Tool::Pen);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
            require(state.brush.tool == Tool::SmartSelect &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 2,
                    "The primary selection button did not return to Smart Select");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 5);
            std::vector<size_t> activeStrokeIndices;
            for (size_t index = 0; index < state.ink.size(); ++index) {
                if (state.ink[index].page == 0 && state.ink[index].layerId == state.activeLayerId(0) &&
                    state.layerVisible(0, state.ink[index].layerId)) {
                    activeStrokeIndices.push_back(index);
                }
            }
            require(activeStrokeIndices.size() >= 2,
                    "Select Object click verification needs two strokes on the active layer");
            const auto firstStroke = activeStrokeIndices[0];
            const auto secondStroke = activeStrokeIndices[1];
            const auto firstBefore = state.ink[firstStroke].points;
            const auto clickStroke = [&](size_t index) {
                const auto& stroke = state.ink[index];
                const auto& anchor = stroke.points[stroke.points.size() / 2];
                const auto screen = state.pageScreenPoint(stroke.page, anchor.x, anchor.y);
                g_signal_emit_by_name(mouse, "drag-begin", screen.x, screen.y);
                g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
            };
            state.clearSelection();
            clickStroke(firstStroke);
            require(state.selected == std::vector<size_t>{firstStroke} && state.ink[firstStroke].points == firstBefore,
                    "Select Object did not select on click without moving the object");
            input.shiftHeld = true;
            clickStroke(secondStroke);
            input.shiftHeld = false;
            require(state.selected == std::vector<size_t>({firstStroke, secondStroke}),
                    "Shift-click Select Object did not add a second object");
            const auto handSelection = state.selected;
            const auto handInkCount = state.ink.size();
            const double handOffsetX = state.offsetX;
            const double handOffsetY = state.offsetY;
            selectTool(Tool::Hand);
            require(state.brush.tool == Tool::Hand && state.selected == handSelection &&
                            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toolButtons[4])),
                    "Selecting Hand should preserve the current object selection");
            const auto panStart = state.pageScreenPoint(0, 240, 240);
            g_signal_emit_by_name(mouse, "drag-begin", panStart.x, panStart.y);
            g_signal_emit_by_name(mouse, "drag-update", 24.0, -18.0);
            g_signal_emit_by_name(mouse, "drag-end", 24.0, -18.0);
            require(state.ink.size() == handInkCount && state.selected == handSelection &&
                            state.offsetX != handOffsetX && state.offsetY != handOffsetY && !input.active(),
                    "Hand drag did not pan the canvas while retaining its selected objects");
            const auto lowerFirstBefore = state.ink[0].points;
            const auto lowerSecondBefore = state.ink[1].points;
            const auto upperFirstBefore = state.ink[2].points;
            const auto upperSecondBefore = state.ink[3].points;
            const auto textBeforeVerticalSpace = state.texts[0];
            activateSelection(Tool::SelectRectangle);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 8);
            require(state.brush.tool == Tool::VerticalSpace && gtk_widget_get_visible(selectionModeDropdown) &&
                            !gtk_widget_get_visible(selectionActions),
                    "Vertical space did not activate from the selection mode menu");
            input.setModifiers(false, false, false);
            const auto verticalStart = state.pageScreenPoint(0, 40, 240);
            g_signal_emit_by_name(mouse, "drag-begin", verticalStart.x, verticalStart.y);
            require(state.verticalSpacePreview() && !state.verticalSpacePreview()->moveAbove &&
                            state.verticalSpacePreview()->inkIndices.empty(),
                    "Vertical-space drag should start in below-the-line mode with no lower objects selected");
            gboolean controlHandled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Control_L, 0u, GdkModifierType(GDK_CONTROL_MASK),
                                  &controlHandled);
            require(controlHandled && state.verticalSpacePreview()->moveAbove &&
                            state.verticalSpacePreview()->inkIndices == std::vector<size_t>({2, 3}) &&
                            state.verticalSpacePreview()->textIndices == std::vector<size_t>{0},
                    "Holding Control during the drag did not switch the preview above the boundary");
            g_signal_emit_by_name(mouse, "drag-update", 0.0, 20.0 * state.zoom);
            const double verticalShift = state.verticalSpacePreview()->endY - state.verticalSpacePreview()->startY;
            require(verticalShift > 0, "Vertical-space boundary did not move down");
            waitForNextPaint();
            capture("vertical-space-preview");
            g_signal_emit_by_name(keys, "key-released", GDK_KEY_Control_L, 0u, GdkModifierType(0));
            require(!state.verticalSpacePreview()->moveAbove && state.verticalSpacePreview()->inkIndices.empty(),
                    "Releasing Control during the drag did not switch the preview below the boundary");
            controlHandled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Control_L, 0u, GdkModifierType(GDK_CONTROL_MASK),
                                  &controlHandled);
            require(controlHandled && state.verticalSpacePreview()->inkIndices == std::vector<size_t>({2, 3}),
                    "Pressing Control again did not restore the above-the-line preview");
            g_signal_emit_by_name(mouse, "drag-end", 0.0, 20.0 * state.zoom);
            g_signal_emit_by_name(keys, "key-released", GDK_KEY_Control_L, 0u, GdkModifierType(0));
            require(!state.verticalSpacePreview() && state.ink[0].points == lowerFirstBefore &&
                            state.ink[1].points == lowerSecondBefore &&
                            std::abs(state.ink[2].points.front().y - upperFirstBefore.front().y - verticalShift) <
                                    1e-6 &&
                            std::abs(state.ink[3].points.front().y - upperSecondBefore.front().y - verticalShift) <
                                    1e-6 &&
                            std::abs(state.texts[0].y - textBeforeVerticalSpace.y - verticalShift) < 1e-6,
                    "Vertical space did not move only the upper-layer objects selected by its preview");
            const auto verticalRevision = state.revision();
            state.undo();
            require(state.revision() != verticalRevision &&
                            state.ink[2].points.front().y == upperFirstBefore.front().y &&
                            state.texts[0].y == textBeforeVerticalSpace.y,
                    "Undo did not restore the vertical-space edit as one step");
            state.redo();
            require(state.revision() == verticalRevision &&
                            std::abs(state.ink[2].points.front().y - upperFirstBefore.front().y - verticalShift) < 1e-6,
                    "Redo did not reapply the vertical-space edit");
            waitForNextPaint();
            capture("vertical-space-applied");
            activateSelection(Tool::SmartSelect);
            state.clearSelection();
            state.snapGridEnabled = false;
            prototype::Ink rotationStroke{0, {{120, 300}, {220, 340}}, {Tool::Pen, {0.8, 0.1, 0.1}, 3.0}};
            rotationStroke.layerId = state.activeLayerId(0);
            rotationStroke.originLayer = state.activeLayerOrigin(0);
            rotationStroke.layerOrder = state.ink.size();
            state.ink.push_back(rotationStroke);
            const auto rotationStrokeIndex = state.ink.size() - 1;
            state.selected = {rotationStrokeIndex};
            const auto originalFrame = *state.selectionBox();
            const auto rotationHandle = state.selectionHandles()[8];
            const prototype::Point rotationCenter{(originalFrame.x0 + originalFrame.x1) / 2,
                                                  (originalFrame.y0 + originalFrame.y1) / 2};
            const auto radius = std::hypot(rotationHandle.x - rotationCenter.x, rotationHandle.y - rotationCenter.y);
            constexpr double rotationAngle = 0.7853981633974483;
            const auto rotationCosine = std::cos(rotationAngle);
            const auto rotationSine = std::sin(rotationAngle);
            const auto rotateAroundCenter = [&](prototype::Point point) {
                const auto x = point.x - rotationCenter.x;
                const auto y = point.y - rotationCenter.y;
                return prototype::Point{rotationCenter.x + rotationCosine * x - rotationSine * y,
                                        rotationCenter.y + rotationSine * x + rotationCosine * y};
            };
            const std::array<prototype::Point, 4> rotatedCorners{
                    rotateAroundCenter({120, 300}), rotateAroundCenter({220, 300}), rotateAroundCenter({220, 340}),
                    rotateAroundCenter({120, 340})};
            const auto rotatedBounds = prototype::CanvasState::boundsForCorners(0, rotatedCorners);
            const auto rotationStart = state.pageScreenPoint(0, rotationHandle.x, rotationHandle.y);
            const auto rotationEnd = state.pageScreenPoint(0, rotationCenter.x + radius * rotationCosine,
                                                           rotationCenter.y + radius * rotationSine);
            g_signal_emit_by_name(mouse, "drag-begin", rotationStart.x, rotationStart.y);
            g_signal_emit_by_name(mouse, "drag-update", rotationEnd.x - rotationStart.x,
                                  rotationEnd.y - rotationStart.y);
            g_signal_emit_by_name(mouse, "drag-end", rotationEnd.x - rotationStart.x, rotationEnd.y - rotationStart.y);
            require(state.selectionBox() && std::abs(state.selectionBox()->x0 - rotatedBounds.x0) < 1e-6 &&
                            std::abs(state.selectionBox()->y0 - rotatedBounds.y0) < 1e-6 &&
                            std::abs(state.selectionBox()->x1 - rotatedBounds.x1) < 1e-6 &&
                            std::abs(state.selectionBox()->y1 - rotatedBounds.y1) < 1e-6 &&
                            std::abs(state.selectionHandles()[0].y - state.selectionHandles()[2].y) > 1,
                    "Smart Select rotation did not rotate the visible handle frame");
            waitForNextPaint();
            capture("smart-select-rotated-frame");
            const auto rightHandle = state.selectionHandles()[3];
            const auto rotationResizeStart = state.pageScreenPoint(0, rightHandle.x, rightHandle.y);
            const auto rotationResizeEnd =
                    state.pageScreenPoint(0, rightHandle.x + 40 * rotationCosine, rightHandle.y + 40 * rotationSine);
            g_signal_emit_by_name(mouse, "drag-begin", rotationResizeStart.x, rotationResizeStart.y);
            g_signal_emit_by_name(mouse, "drag-update", rotationResizeEnd.x - rotationResizeStart.x,
                                  rotationResizeEnd.y - rotationResizeStart.y);
            g_signal_emit_by_name(mouse, "drag-end", rotationResizeEnd.x - rotationResizeStart.x,
                                  rotationResizeEnd.y - rotationResizeStart.y);
            const std::array<prototype::Point, 4> resizedCorners{
                    rotateAroundCenter({120, 300}), rotateAroundCenter({260, 300}), rotateAroundCenter({260, 340}),
                    rotateAroundCenter({120, 340})};
            const auto resizedBounds = prototype::CanvasState::boundsForCorners(0, resizedCorners);
            const auto resizedStart = rotateAroundCenter({120, 300});
            const auto resizedEnd = rotateAroundCenter({260, 340});
            require(state.selectionBox() && std::abs(state.selectionBox()->x0 - resizedBounds.x0) < 1e-6 &&
                            std::abs(state.selectionBox()->x1 - resizedBounds.x1) < 1e-6 &&
                            std::abs(state.selectionBox()->y0 - resizedBounds.y0) < 1e-6 &&
                            std::abs(state.selectionBox()->y1 - resizedBounds.y1) < 1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[0].x - resizedStart.x) < 1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[0].y - resizedStart.y) < 1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[1].x - resizedEnd.x) < 1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[1].y - resizedEnd.y) < 1e-6,
                    "Smart Select resize did not apply scaling in the accumulated rotated coordinates");
            waitForNextPaint();
            capture("smart-select-rotated-resize");
            state.undo();
            require(state.selectionBox() && std::abs(state.selectionBox()->x0 - rotatedBounds.x0) < 1e-6 &&
                            std::abs(state.selectionBox()->x1 - rotatedBounds.x1) < 1e-6 &&
                            std::abs(state.selectionBox()->y0 - rotatedBounds.y0) < 1e-6 &&
                            std::abs(state.selectionBox()->y1 - rotatedBounds.y1) < 1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[1].x - rotateAroundCenter({220, 340}).x) <
                                    1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[1].y - rotateAroundCenter({220, 340}).y) <
                                    1e-6,
                    "Undo did not restore the rotated handle frame and stroke geometry");
            state.undo();
            require(std::abs(state.ink[rotationStrokeIndex].points[0].x - 120) < 1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[0].y - 300) < 1e-6 &&
                            std::abs(state.selectionBox()->x1 - originalFrame.x1) < 1e-6,
                    "Undo rotation did not restore the original Smart Select state");
            state.redo();
            state.redo();
            require(std::abs(state.ink[rotationStrokeIndex].points[1].x - resizedEnd.x) < 1e-6 &&
                            std::abs(state.ink[rotationStrokeIndex].points[1].y - resizedEnd.y) < 1e-6 &&
                            std::abs(state.selectionBox()->x0 - resizedBounds.x0) < 1e-6 &&
                            std::abs(state.selectionBox()->x1 - resizedBounds.x1) < 1e-6 &&
                            std::abs(state.selectionBox()->y0 - resizedBounds.y0) < 1e-6 &&
                            std::abs(state.selectionBox()->y1 - resizedBounds.y1) < 1e-6,
                    "Redo did not restore the rotated Smart Select resize");
            state.undo();
            state.undo();
            state.clearSelection();
            state.ink.pop_back();

            const auto viewportWidth = gtk_widget_get_width(canvas);
            const auto viewportHeight = gtk_widget_get_height(canvas);
            require(viewportWidth > 0 && viewportHeight > 0, "Edge-pan verification has no allocated canvas");
            state.clearSelection();
            prototype::TextItem edgePanText;
            edgePanText.page = 0;
            edgePanText.layerId = 1;
            edgePanText.originLayer = 0;
            edgePanText.text = "edge pan";
            edgePanText.layoutWidth = 40;
            edgePanText.layoutHeight = 40;
            edgePanText.x = 80;
            edgePanText.y = 100;
            state.texts.push_back(edgePanText);
            const auto edgePanTextIndex = state.texts.size() - 1;
            state.selectedTexts = {edgePanTextIndex};
            state.brush.tool = Tool::SmartSelect;
            state.snapGridEnabled = false;
            state.edgePanSpeed = 20;
            state.edgePanMaxMultiplier = 5;
            state.zoom = std::max(2.0, (viewportWidth + 50.0) / 500.0);
            state.offsetX = 0;
            state.offsetY = 0;
            const auto edgePanStart = state.pageScreenPoint(0, 100, 120);
            const auto edgePanPointer = prototype::Point{viewportWidth - 1.0, edgePanStart.y};
            require(state.beginSelection(edgePanStart.x, edgePanStart.y), "Smart Select edge-pan move did not begin");
            state.moveSelectionPointer(edgePanPointer.x, edgePanPointer.y);
            require(state.selectionCanEdgePan(), "Smart Select move did not enter edge-pan mode");
            const auto edgePanBounds = state.selectionBox();
            require(edgePanBounds &&
                            edgePanBounds->x1 + state.pageLeft(0) > (viewportWidth - state.offsetX) / state.zoom,
                    "Edge-pan fixture does not extend past the canvas viewport");
            selectionVerifyEdgePanOffsetX = state.offsetX;
            selectionVerifyEdgePanScreenX = state.offsetX + (state.pageLeft(0) + edgePanBounds->x0) * state.zoom;
            scheduleSelectionEdgePan(edgePanPointer.x, edgePanPointer.y);
            update();
            return G_SOURCE_CONTINUE;
        }
        case 6: {
            if (state.offsetX >= selectionVerifyEdgePanOffsetX) {
                --selectionVerifyStep;
                return G_SOURCE_CONTINUE;
            }
            const auto edgePanBounds = state.selectionBox();
            require(edgePanBounds && state.offsetX < selectionVerifyEdgePanOffsetX &&
                            std::abs(state.offsetX + (state.pageLeft(0) + edgePanBounds->x0) * state.zoom -
                                     selectionVerifyEdgePanScreenX) < 1e-6,
                    "The edge-pan timer did not scroll while keeping the selection under the stationary pointer");
            stopSelectionEdgePan();
            state.finishSelection();
            update();
            waitForNextPaint();
            capture("smart-select-edge-pan");
            std::cout << "PASS: GTK visible-layer rectangle/lasso selection, Smart Select edge hit, grid snap, "
                         "resize snap, rotation-aware resize and edge-pan, Text-tool mouse/stylus move/resize and "
                         "formatting, "
                         "vertical-space direction/preview/undo, Select Object and Hand modes, "
                         "selection-preserving pan, hidden-layer fallback, remembered tool state, and shortcuts\n";
            closeWindow();
            return G_SOURCE_REMOVE;
        }
        default:
            return G_SOURCE_REMOVE;
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

void Prototype::smokeDragPdfPage(Tool tool) {
    activateSelection(tool);
    auto* page = pdfPageForPage(0);
    require(page != nullptr, "PDF-backed XOPP fixture lost its first PDF page");
    PopplerRectangle* glyphs = nullptr;
    guint glyphCount = 0;
    require(poppler_page_get_text_layout(page, &glyphs, &glyphCount) && glyphCount > 0,
            "PDF-backed fixture has no selectable text layout");
    const auto firstGlyph = glyphs[0];
    double left = std::min(firstGlyph.x1, firstGlyph.x2);
    double right = std::max(firstGlyph.x1, firstGlyph.x2);
    double top = std::min(firstGlyph.y1, firstGlyph.y2);
    double bottom = std::max(firstGlyph.y1, firstGlyph.y2);
    for (guint index = 1; index < glyphCount; ++index) {
        const auto& glyph = glyphs[index];
        if (std::abs(glyph.y1 - firstGlyph.y1) > 1e-5 || std::abs(glyph.y2 - firstGlyph.y2) > 1e-5) {
            continue;
        }
        left = std::min(left, std::min(glyph.x1, glyph.x2));
        right = std::max(right, std::max(glyph.x1, glyph.x2));
        top = std::min(top, std::min(glyph.y1, glyph.y2));
        bottom = std::max(bottom, std::max(glyph.y1, glyph.y2));
    }
    g_free(glyphs);
    g_object_unref(page);
    const auto start = state.pageScreenPoint(0, left - 1, top - 1);
    const auto end = state.pageScreenPoint(0, right + 1, bottom + 1);
    g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
    g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
    g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
}

gboolean Prototype::smokePdfTextToolbar() {
    switch (pdfTextSelectionVerifyStep++) {
        case 0: {
            require(document && pageCount > 0, "PDF toolbar verification requires a directly opened PDF document");
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            require(pdfTextSelection && pdfTextSelection->finalized && !pdfTextSelection->text.empty() &&
                            !pdfTextSelection->textRects.empty() &&
                            gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)) &&
                            !gtk_popover_get_autohide(pdfActionsPopover),
                    "PDF text selection did not show its persistent action toolbar");
            capture("pdf-text-toolbar-placement");

            const auto& selectedRect = pdfTextSelection->textRects.front();
            const double left = std::min(selectedRect.x1, selectedRect.x2);
            const double top = std::min(selectedRect.y1, selectedRect.y2);
            const double right = std::max(selectedRect.x1, selectedRect.x2);
            const double bottom = std::max(selectedRect.y1, selectedRect.y2);
            const auto start = state.pageScreenPoint(pdfTextSelection->page, left - 4.25, top - 4.25);
            const auto end = state.pageScreenPoint(pdfTextSelection->page, right + 4.25, bottom + 4.25);
            activateSelection(Tool::SelectPdfTextRect);
            hidePdfActionsPreservingSelection();
            g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
            g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
            require(pdfTextSelection && pdfTextSelection->dragging &&
                            state.brush.tool == Tool::SelectPdfTextRect &&
                            pdfTextSelection->tool == Tool::SelectPdfTextRect &&
                            pdfTextSelection->style == prototype::PdfTextSelectionStyle::Rectangle &&
                            pdfTextSelection->region,
                    "Rectangular PDF selection did not expose its active preview region");
            cairo_rectangle_int_t previewBounds{};
            cairo_region_get_extents(pdfTextSelection->region.get(), &previewBounds);
            const auto frameBounds = prototype::pdf_text_selection_detail::cairoRect(pdfTextSelection->bounds);
            require(previewBounds.x == frameBounds.x && previewBounds.y == frameBounds.y &&
                            previewBounds.width == frameBounds.width && previewBounds.height == frameBounds.height,
                    "Rectangular PDF selection frame diverged from its shaded preview");
            capture("pdf-text-rectangle-preview");
            g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            require(pdfTextSelection && pdfTextSelection->finalized && !pdfTextSelection->text.empty(),
                    "Rectangular PDF selection preview did not finalize selectable text");
            break;
        }
        case 1: {
            activateSelection(Tool::Hand);
            const auto blankPoint = state.pageScreenPoint(0, 1, 1);
            begin(Device::Mouse, blankPoint.x, blankPoint.y);
            finish(Device::Mouse, std::nullopt, blankPoint);
            require(pdfTextSelection && pdfTextSelection->finalized &&
                            !gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)),
                    "A Hand-tool press did not hide the PDF action toolbar while preserving its selection");
            activateSelection(Tool::SelectPdfTextLinear);

            const auto& textRect = pdfTextSelection->textRects.front();
            const auto textCenter = state.pageScreenPoint(pdfTextSelection->page, (textRect.x1 + textRect.x2) / 2,
                                                          (textRect.y1 + textRect.y2) / 2);
            zoom(state.zoom * 1.1, textCenter.x, textCenter.y);
            require(pdfTextSelection && pdfTextSelection->finalized &&
                            !gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)),
                    "Zooming did not hide the PDF action toolbar while preserving its selection");
            break;
        }
        case 2: {
            const auto& textRect = pdfTextSelection->textRects.front();
            const auto textCenter = state.pageScreenPoint(pdfTextSelection->page, (textRect.x1 + textRect.x2) / 2,
                                                          (textRect.y1 + textRect.y2) / 2);
            begin(Device::Mouse, textCenter.x, textCenter.y);
            GdkRectangle toolbarPoint{};
            require(pdfTextSelection && pdfTextSelection->finalized && !pdfTextSelection->dragging &&
                            gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)) &&
                            gtk_popover_get_pointing_to(pdfActionsPopover, &toolbarPoint) && toolbarPoint.width == 1 &&
                            toolbarPoint.height == 1 && toolbarPoint.x >= 0 && toolbarPoint.y >= 0 &&
                            toolbarPoint.x < gtk_widget_get_width(canvasOverlay) &&
                            toolbarPoint.y < gtk_widget_get_height(canvasOverlay),
                    "Clicking selected PDF text did not reopen the compact toolbar at an in-viewport point");
            capture("pdf-text-toolbar-reopened");
            std::cout << "PASS: GTK PDF text toolbar placement, input/zoom dismissal, and selection reopen\n";
            closeWindow();
            return G_SOURCE_REMOVE;
        }
        default:
            return G_SOURCE_REMOVE;
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

gboolean Prototype::smokePdfTextSelection() {
    if (pdfTextSelectionVerifyMode == "toolbar-only") {
        return smokePdfTextToolbar();
    }
    switch (pdfTextSelectionVerifyStep++) {
        case 0: {
            require(nativeSession && document && pageCount >= 1 && state.texts.size() >= 3,
                    "PDF text selection verification requires a native PDF-backed XOPP fixture");
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            require(pdfTextSelection && pdfTextSelection->finalized &&
                            pdfTextSelection->text.find("Test PDF for Xournal++") != std::string::npos &&
                            pdfTextSelection->text.find("テスト") == std::string::npos &&
                            !pdfTextSelection->textRects.empty(),
                    "Linear selection did not select text from the PDF background only");
            capture("pdf-text-linear-selection");
            require(gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)) &&
                            !gtk_popover_get_autohide(pdfActionsPopover),
                    "Finalizing a PDF text selection did not show its persistent action toolbar");
            const auto& firstTextRect = pdfTextSelection->textRects.front();
            const auto firstTextCenter =
                    state.pageScreenPoint(pdfTextSelection->page, (firstTextRect.x1 + firstTextRect.x2) / 2,
                                          (firstTextRect.y1 + firstTextRect.y2) / 2);
            zoom(state.zoom * 1.1, firstTextCenter.x, firstTextCenter.y);
            require(pdfTextSelection && pdfTextSelection->finalized &&
                            !gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)),
                    "Zooming did not hide the PDF action toolbar while keeping its text selection");
            const auto reopenedTextCenter =
                    state.pageScreenPoint(pdfTextSelection->page, (firstTextRect.x1 + firstTextRect.x2) / 2,
                                          (firstTextRect.y1 + firstTextRect.y2) / 2);
            begin(Device::Mouse, reopenedTextCenter.x, reopenedTextCenter.y);
            GdkRectangle toolbarPoint{};
            require(pdfTextSelection && pdfTextSelection->finalized && !pdfTextSelection->dragging &&
                            gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)) &&
                            gtk_popover_get_pointing_to(pdfActionsPopover, &toolbarPoint) && toolbarPoint.width == 1 &&
                            toolbarPoint.height == 1,
                    "Clicking the selected text did not reopen the compact toolbar at the pointer");
            capture("pdf-text-toolbar-reopened");

            auto* page = pdfPageForPage(0);
            require(page != nullptr, "PDF-backed XOPP fixture lost its first PDF page");
            PopplerRectangle* glyphs = nullptr;
            guint glyphCount = 0;
            require(poppler_page_get_text_layout(page, &glyphs, &glyphCount) && glyphCount > 0,
                    "PDF-backed fixture has no text layout for Smart Select");
            PopplerRectangle smartGlyph{};
            bool foundPdfOnlyGlyph = false;
            for (guint index = 0; index < glyphCount; ++index) {
                const auto& glyph = glyphs[index];
                const prototype::Point point{(glyph.x1 + glyph.x2) / 2, (glyph.y1 + glyph.y2) / 2};
                if (!smartSelectHasEditableObjectAt(0, point)) {
                    smartGlyph = glyph;
                    foundPdfOnlyGlyph = true;
                    break;
                }
            }
            g_free(glyphs);
            g_object_unref(page);
            require(foundPdfOnlyGlyph, "PDF-backed fixture has no glyph outside editable native objects");
            const auto smartStart =
                    state.pageScreenPoint(0, (smartGlyph.x1 + smartGlyph.x2) / 2, (smartGlyph.y1 + smartGlyph.y2) / 2);
            const auto smartEnd = state.pageScreenPoint(0, std::max(smartGlyph.x1, smartGlyph.x2) + 80,
                                                        (smartGlyph.y1 + smartGlyph.y2) / 2);
            input.setModifiers(false, false, false);
            activateSelection(Tool::SmartSelect);
            begin(Device::Mouse, smartStart.x, smartStart.y);
            require(smartSelectPdfGesture && !input.active(),
                    "Smart Select did not defer a PDF-text press until the drag threshold");
            motion(Device::Mouse, smartEnd.x, smartEnd.y);
            require(!smartSelectPdfGesture && pdfTextSelection && pdfTextSelection->dragging &&
                            pdfTextSelection->tool == Tool::SmartSelect &&
                            pdfTextSelection->style == prototype::PdfTextSelectionStyle::Linear,
                    "Smart Select did not delegate a PDF-text drag to linear text selection");
            finish(Device::Mouse, std::nullopt, smartEnd);
            require(pdfTextSelection && pdfTextSelection->finalized && !pdfTextSelection->text.empty(),
                    "Smart Select PDF-text drag did not finalize a native PDF selection");
            capture("smart-select-pdf-text");
            g_signal_emit_by_name(pdfSelectionStyleButton, "clicked");
            require(state.brush.tool == Tool::SmartSelect && pdfTextSelection &&
                            pdfTextSelection->style == prototype::PdfTextSelectionStyle::Rectangle,
                    "PDF selection actions changed the active Smart Select tool or failed to switch style");
            g_signal_emit_by_name(pdfSelectionStyleButton, "clicked");
            require(state.brush.tool == Tool::SmartSelect && pdfTextSelection &&
                            pdfTextSelection->style == prototype::PdfTextSelectionStyle::Linear,
                    "Smart Select PDF selection did not return to linear style");

            activateSelection(Tool::SmartSelect);
            begin(Device::Mouse, smartStart.x, smartStart.y);
            require(smartSelectPdfGesture.has_value(), "Smart Select PDF click was not deferred");
            finish(Device::Mouse, std::nullopt, smartStart);
            require(!smartSelectPdfGesture && !pdfTextSelection,
                    "A PDF-text click below the drag threshold unexpectedly selected PDF text");

            const auto blankStart = state.pageScreenPoint(0, 1, 1);
            const auto blankEnd = state.pageScreenPoint(0, 60, 60);
            require(!pdfTextAtCanvasPoint(0, {1, 1}), "Smart Select Alt test point is not in a blank PDF margin");
            input.setModifiers(false, false, true);
            begin(Device::Mouse, blankStart.x, blankStart.y);
            require(smartSelectPdfGesture && smartSelectPdfGesture->altPressed,
                    "Alt did not force Smart Select into deferred PDF-text routing");
            motion(Device::Mouse, blankEnd.x, blankEnd.y);
            require(pdfTextSelection && pdfTextSelection->dragging && pdfTextSelection->tool == Tool::SmartSelect,
                    "Alt-drag with Smart Select did not start PDF text selection on a blank page area");
            finish(Device::Mouse, std::nullopt, blankEnd);
            input.setModifiers(false, false, false);
            clearPdfTextSelection(false);
            require(!pdfTextSelection, "Alt-drag cleanup left a PDF selection active");

            activateSelection(Tool::SmartSelect);
            input.setModifiers(true, false, false);
            begin(Device::Mouse, smartStart.x, smartStart.y);
            require(!smartSelectPdfGesture && input.active(),
                    "Shift did not route Smart Select directly to rectangle selection");
            motion(Device::Mouse, smartEnd.x, smartEnd.y);
            finish(Device::Mouse, std::nullopt, smartEnd);
            input.setModifiers(false, false, false);
            require(!pdfTextSelection, "Shift-rectangle gesture unexpectedly entered PDF text selection");
            capture("smart-select-shift-rectangle");

            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            break;
        }
        case 1: {
            g_signal_emit_by_name(pdfSelectionStyleButton, "clicked");
            require(state.brush.tool == Tool::SelectPdfTextRect && pdfTextSelection &&
                            pdfTextSelection->style == prototype::PdfTextSelectionStyle::Rectangle &&
                            pdfTextSelection->text.find("Test PDF for Xournal++") != std::string::npos,
                    "Switching to rectangular PDF selection did not recompute the current selection");
            capture("pdf-text-rectangle-selection");
            break;
        }
        case 2: {
            auto* page = pdfPageForPage(0);
            require(page != nullptr, "PDF-backed XOPP fixture lost its first PDF page");
            PopplerRectangle* glyphs = nullptr;
            guint glyphCount = 0;
            require(poppler_page_get_text_layout(page, &glyphs, &glyphCount) && glyphCount > 0,
                    "PDF-backed fixture has no selectable text layout");
            const auto firstGlyph = glyphs[0];
            g_free(glyphs);
            g_object_unref(page);
            const auto click =
                    state.pageScreenPoint(0, (firstGlyph.x1 + firstGlyph.x2) / 2, (firstGlyph.y1 + firstGlyph.y2) / 2);
            g_signal_emit_by_name(textClick, "released", 2, click.x, click.y);
            require(pdfTextSelection && pdfTextSelection->finalized &&
                            pdfTextSelection->style == prototype::PdfTextSelectionStyle::Word &&
                            !pdfTextSelection->text.empty(),
                    "Double click did not select a PDF word");
            capture("pdf-text-word-selection");
            g_signal_emit_by_name(textClick, "released", 3, click.x, click.y);
            require(pdfTextSelection && pdfTextSelection->finalized &&
                            pdfTextSelection->style == prototype::PdfTextSelectionStyle::Line &&
                            pdfTextSelection->text.find("Test PDF for Xournal++") != std::string::npos,
                    "Triple click did not select the full PDF text line");
            capture("pdf-text-line-selection");
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            pdfClipboardExpected = pdfTextSelection->text;
            g_signal_emit_by_name(pdfCopyButton, "clicked");
            require(!pdfTextSelection && !gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)),
                    "Copy did not dismiss the PDF selection actions");
            auto* clipboard = gdk_display_get_clipboard(gtk_widget_get_display(canvas));
            gdk_clipboard_read_text_async(
                    clipboard, nullptr,
                    +[](GObject* source, GAsyncResult* result, gpointer data) {
                        auto* self = static_cast<Prototype*>(data);
                        GError* error = nullptr;
                        char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
                        self->pdfClipboardReadError = error ? error->message : "";
                        g_clear_error(&error);
                        self->pdfClipboardReadText = text ? text : "";
                        g_free(text);
                        self->pdfClipboardReadComplete = true;
                    },
                    this);
            break;
        }
        case 3: {
            if (!pdfClipboardReadComplete) {
                --pdfTextSelectionVerifyStep;
                return G_SOURCE_CONTINUE;
            }
            require(pdfClipboardReadError.empty() && pdfClipboardReadText == pdfClipboardExpected,
                    "PDF Copy did not publish the selected text to the clipboard");
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            const auto oldInkCount = state.ink.size();
            g_signal_emit_by_name(pdfHighlightButton, "clicked");
            require(state.ink.size() > oldInkCount && !pdfTextSelection,
                    "Highlight did not add PDF text annotation strokes");
            for (size_t index = oldInkCount; index < state.ink.size(); ++index) {
                require(state.ink[index].brush.tool == Tool::Highlighter && state.ink[index].fill == 60 &&
                                state.ink[index].capStyle == 1,
                        "PDF highlight did not preserve marker opacity and butt caps");
            }
            capture("pdf-text-highlight");
            break;
        }
        case 4: {
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            const auto oldInkCount = state.ink.size();
            g_signal_emit_by_name(pdfUnderlineButton, "clicked");
            require(state.ink.size() > oldInkCount && !pdfTextSelection,
                    "Underline did not add PDF text annotation strokes");
            for (size_t index = oldInkCount; index < state.ink.size(); ++index) {
                require(state.ink[index].fill == 230 && state.ink[index].capStyle == 1,
                        "PDF underline did not preserve opacity and butt caps");
            }
            capture("pdf-text-underline");
            break;
        }
        case 5: {
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            const auto oldInkCount = state.ink.size();
            g_signal_emit_by_name(pdfStrikethroughButton, "clicked");
            require(state.ink.size() > oldInkCount && !pdfTextSelection,
                    "Strikethrough did not add PDF text annotation strokes");
            for (size_t index = oldInkCount; index < state.ink.size(); ++index) {
                require(state.ink[index].fill == 230 && state.ink[index].capStyle == 1,
                        "PDF strikethrough did not preserve opacity and butt caps");
            }
            capture("pdf-text-strikethrough");
            break;
        }
        case 6: {
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            gtk_menu_button_popup(GTK_MENU_BUTTON(pdfHighlightPaletteButton));
            capture("pdf-text-highlight-presets");
            g_signal_emit_by_name(pdfHighlightPresetButtons[1], "clicked");
            require(std::abs(highlightBrush.color[0] - 140.0 / 255) < 1e-6 &&
                            std::abs(highlightBrush.color[1] - 233.0 / 255) < 1e-6 &&
                            std::abs(highlightBrush.color[2] - 154.0 / 255) < 1e-6,
                    "Green PDF highlight preset did not select the production color");
            const auto oldInkCount = state.ink.size();
            const auto oldRevision = state.revision();
            g_signal_emit_by_name(pdfHighlightButton, "clicked");
            require(state.ink.size() == oldInkCount && state.revision() == oldRevision + 1,
                    "Reapplying a highlight should recolor existing strokes in one edit");
            for (size_t index = 0; index < state.ink.size(); ++index) {
                if (state.ink[index].fill == 60) {
                    require(std::abs(state.ink[index].brush.color[1] - 233.0 / 255) < 1e-6,
                            "Reapplying a PDF highlight did not recolor existing markers");
                }
            }
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.revision() == oldRevision && state.ink.size() == oldInkCount &&
                            std::abs(state.ink.front().brush.color[0] - 1.0) < 1e-6,
                    "Undo did not restore the prior PDF highlight color");
            g_signal_emit_by_name(redoButton, "clicked");
            require(state.revision() == oldRevision + 1 &&
                            std::abs(state.ink.front().brush.color[1] - 233.0 / 255) < 1e-6,
                    "Redo did not restore the PDF highlight recolor");
            capture("pdf-text-highlight-recolored");
            gtk_menu_button_popup(GTK_MENU_BUTTON(pdfMarkerOpacityButton));
            capture("pdf-text-marker-opacity-control");
            gtk_range_set_value(GTK_RANGE(pdfMarkerOpacityScale), 128);
            require(pdfTextMarkerOpacity == 128, "PDF marker opacity control did not change its value");
            gtk_menu_button_popdown(GTK_MENU_BUTTON(pdfMarkerOpacityButton));
            smokeDragPdfPage(Tool::SelectPdfTextLinear);
            const auto beforeOpacityEdit = state.ink.size();
            g_signal_emit_by_name(pdfHighlightButton, "clicked");
            require(state.ink.size() > beforeOpacityEdit,
                    "Applying a different PDF marker opacity did not create updated annotations");
            for (size_t index = beforeOpacityEdit; index < state.ink.size(); ++index) {
                require(state.ink[index].fill == 128,
                        "PDF highlight opacity control did not set the annotation fill value");
            }
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.ink.size() == beforeOpacityEdit,
                    "Undo did not remove highlights created with the changed opacity");
            gtk_range_set_value(GTK_RANGE(pdfMarkerOpacityScale), 60);
            require(pdfTextMarkerOpacity == 60, "PDF marker opacity did not restore its default value");
            break;
        }
        case 7: {
            pdfTextSelectionOutput =
                    (std::filesystem::path(options.smokeDirectory) / "pdf-text-annotations.xopp").string();
            saveCopyTo(pdfTextSelectionOutput, [this] { pdfTextSelectionSaveComplete = true; });
            require(busy, "Saving PDF text annotations did not start");
            break;
        }
        default: {
            require(pdfTextSelectionSaveComplete && !modified() &&
                            std::filesystem::is_regular_file(pdfTextSelectionOutput),
                    "Native save did not preserve PDF text annotations");
            std::cout << "PASS: GTK Smart Select PDF delegation, threshold and modifier routing, "
                         "linear/rectangle/word/line selection, toolbar placement and dismissal/reopen, "
                         "clipboard, marker presets and opacity, annotations, undo/redo and native save\n";
            closeWindow();
            return G_SOURCE_REMOVE;
        }
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

}  // namespace prototype::gtk4
