#include "Prototype.h"

namespace prototype::gtk4 {

bool Prototype::getPluginActionState(std::string_view action, prototype::PluginActionValue& value,
                                     std::string& error) const {
    if (action == "select-tool") {
        value = pluginSelectedToolState();
        return true;
    }
    if (action == "grid-snapping") {
        value = state.snapGridEnabled;
        return true;
    }
    if (action == "position-highlighting") {
        value = importedConfig.settings.highlightPosition;
        return true;
    }
    if (action == "zoom") {
        value = state.zoom;
        return true;
    }
    if (action == "tool-size") {
        value = pluginActiveToolSizeState();
        return true;
    }
    if (action == "tool-pen-size") {
        value = pluginToolSizeState(prototype::behavior::penWidths, penBrush.width);
        return true;
    }
    if (action == "tool-eraser-size") {
        value = pluginToolSizeState(prototype::behavior::eraserWidths, eraserBrush.width);
        return true;
    }
    if (action == "tool-highlighter-size") {
        value = pluginToolSizeState(prototype::behavior::highlighterWidths, highlightBrush.width);
        return true;
    }
    if (action == "tool-fill") {
        value = fillToggleActive();
        return true;
    }
    if (action == "tool-eraser-type") {
        value = static_cast<std::int64_t>(state.eraserMode) + 1;
        return true;
    }
    if (action == "tool-pen-line-style") {
        value = selectedPenLineStyleState().value_or("none");
        return true;
    }
    if (action == "tool-color") {
        std::array<double, 3> color = state.brush.color;
        if (state.brush.tool == Tool::Text) {
            color = state.textColor;
        } else if (state.brush.tool == Tool::Latex) {
            color = latexBrush.color;
        } else if (state.brush.tool == Tool::Pen) {
            color = penBrush.color;
        } else if (state.brush.tool == Tool::Highlighter) {
            color = highlightBrush.color;
        } else if (state.brush.tool == Tool::LaserPointerPen) {
            color = laserPenBrush.color;
        } else if (state.brush.tool == Tool::LaserPointerHighlighter) {
            color = laserHighlighterBrush.color;
        } else if (state.brush.tool == Tool::Eraser || state.brush.tool == Tool::Hand ||
                   state.brush.tool == Tool::Link || state.brush.tool == Tool::PlayObject ||
                   state.brush.tool == Tool::VerticalSpace || state.brush.tool == Tool::FloatingToolbox) {
            // The master action state is an opaque 0xAARRGGBB Color even while disabled.
            value = std::int64_t{0xff000000U};
            return true;
        }

        const auto channel = [](double component) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(component, 0.0, 1.0) * 255.0));
        };
        const auto red = channel(color[0]);
        const auto green = channel(color[1]);
        const auto blue = channel(color[2]);
        value = static_cast<std::int64_t>(0xff000000U | (red << 16U) | (green << 8U) | blue);
        return true;
    }

    error = "Action state is unavailable in this GTK 4 host: " + std::string(action);
    return false;
}

bool Prototype::changePluginActionState(std::string_view action, const prototype::PluginActionValue& value,
                                        std::string& error) {
    if (action == "tool-pen-line-style") {
        if (!std::holds_alternative<std::string>(value)) {
            error = "tool-pen-line-style expects a string style";
            return false;
        }
        const auto lineDashes = prototype::parseLineStyle(std::get<std::string>(value));
        if (!lineDashes) {
            error = "tool-pen-line-style has an invalid style";
            return false;
        }

        const auto previousStyle = importedConfig.settings.penLineStyle;
        importedConfig.settings.penLineStyle = prototype::formatLineStyle(*lineDashes);
        std::string saveError;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, saveError)) {
            importedConfig.settings.penLineStyle = previousStyle;
            error = saveError;
            return false;
        }

        input.cancel();
        const bool hasSelectedPenStroke = selectedPenStrokeCount() > 0;
        if (hasSelectedPenStroke) {
            state.setSelectedInkLineStyle(*lineDashes);
        }
        penBrush.lineDashes = *lineDashes;
        if (state.brush.tool == Tool::Pen) {
            state.brush.lineDashes = *lineDashes;
        } else if (hasSelectedPenStroke) {
            state.brush.lineDashes = *lineDashes;
            if (state.brush.tool == Tool::Highlighter) {
                highlightBrush.lineDashes = *lineDashes;
            }
        } else {
            selectTool(Tool::Pen);
        }
        update();
        return true;
    }
    if (action == "tool-fill") {
        if (!std::holds_alternative<bool>(value)) {
            error = "tool-fill expects a boolean state";
            return false;
        }
        return setFillEnabled(std::get<bool>(value), error);
    }
    if (action == "position-highlighting") {
        if (!std::holds_alternative<bool>(value)) {
            error = "position-highlighting expects a boolean state";
            return false;
        }
        const bool previous = importedConfig.settings.highlightPosition;
        importedConfig.settings.highlightPosition = std::get<bool>(value);
        std::string saveError;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, saveError)) {
            importedConfig.settings.highlightPosition = previous;
            error = saveError;
            return false;
        }
        auto* control = preferencesUi.controls[static_cast<size_t>(PreferenceKey::PositionHighlight)];
        if (control) {
            preferencesUi.syncingControls = true;
            adw_switch_row_set_active(ADW_SWITCH_ROW(control), importedConfig.settings.highlightPosition);
            preferencesUi.syncingControls = false;
        }
        update();
        return true;
    }
    if (action == "grid-snapping") {
        if (!std::holds_alternative<bool>(value)) {
            error = "grid-snapping expects a boolean state";
            return false;
        }
        state.snapGridEnabled = std::get<bool>(value);
        importedConfig.settings.snapGrid = state.snapGridEnabled;
        std::string saveError;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, saveError)) {
            error = saveError;
            return false;
        }
        update();
        return true;
    }
    if (action != "select-tool" || !std::holds_alternative<std::int64_t>(value)) {
        error = "Only select-tool (integer), grid-snapping/position-highlighting/tool-fill (boolean) "
                "action state changes are supported";
        return false;
    }

    const auto selectionTool = [&](Tool selected, bool acrossLayers = false) {
        activateSelection(selected, acrossLayers);
    };
    switch (std::get<std::int64_t>(value)) {
        case 1:
            selectTool(Tool::Pen);
            break;
        case 2:
            selectTool(Tool::Eraser);
            break;
        case 3:
            selectTool(Tool::Highlighter);
            break;
        case 4:
            selectTool(Tool::Text);
            break;
        case 5:
            if (imageButton)
                g_signal_emit_by_name(imageButton, "clicked");
            break;
        case 6:
            selectionTool(Tool::SelectRectangle);
            break;
        case 7:
            selectionTool(Tool::SelectLasso);
            break;
        case 8:
            selectionTool(Tool::SelectRectangle, true);
            break;
        case 9:
            selectionTool(Tool::SelectLasso, true);
            break;
        case 10:
            selectionTool(Tool::SelectObject);
            break;
        case 11:
            selectTool(Tool::PlayObject);
            break;
        case 12:
            selectTool(Tool::VerticalSpace);
            break;
        case 13:
            selectTool(Tool::Hand);
            break;
        case 14:
            state.drawingMode = prototype::DrawingMode::Rectangle;
            selectTool(Tool::Pen);
            break;
        case 15:
            state.drawingMode = prototype::DrawingMode::Ellipse;
            selectTool(Tool::Pen);
            break;
        case 16:
            state.drawingMode = prototype::DrawingMode::Arrow;
            selectTool(Tool::Pen);
            break;
        case 17:
            state.drawingMode = prototype::DrawingMode::DoubleArrow;
            selectTool(Tool::Pen);
            break;
        case 18:
            state.drawingMode = prototype::DrawingMode::CoordinateSystem;
            selectTool(Tool::Pen);
            break;
        case 19:
            selectTool(Tool::FloatingToolbox);
            break;
        case 20:
            state.drawingMode = prototype::DrawingMode::Spline;
            selectTool(Tool::Pen);
            break;
        case 21:
            selectionTool(Tool::SelectPdfTextLinear);
            break;
        case 22:
            selectionTool(Tool::SelectPdfTextRect);
            break;
        case 23:
            selectTool(Tool::LaserPointerPen);
            break;
        case 24:
            selectTool(Tool::LaserPointerHighlighter);
            break;
        case 25:
            selectTool(Tool::Link);
            break;
        case 27:
            selectionTool(Tool::SmartSelect);
            break;
        default:
            error = "This master tool is not available in the GTK 4 prototype yet";
            return false;
    }
    if (drawingModeDropdown) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown), static_cast<guint>(state.drawingMode));
    }
    update();
    return true;
}

bool Prototype::stepBeamerPresentation(int delta, std::string& error) {
    if (!nativeSession) {
        error = "Beamer navigation requires an open document";
        return false;
    }
    const auto action =
            prototype::beamer::step(state.page, delta, nativeSession->pdfPageMap, nativeSession->pdfPageLabels);
    if (!action) {
        return true;
    }
    if (action->kind == prototype::beamer::Step::Kind::ChangePdfBackground) {
        return changePdfBackgroundPageFromPlugin(action->target, error);
    }
    navigateToPage(action->target);
    return true;
}

bool Prototype::cleanBeamerAnimations(std::string& error) {
    if (!nativeSession || options.worker.empty()) {
        error = "Cleaning Beamer animation pages requires a native document and document worker";
        return false;
    }
    if (busy || choosingFile || messageDialog || closed) {
        error = "A document operation is already in progress";
        return false;
    }
    if (autosaving) {
        waitForAutosave([this] {
            std::string deferredError;
            if (!cleanBeamerAnimations(deferredError)) {
                showError(deferredError);
            }
        });
        return true;
    }

    const auto pageIndices =
            prototype::beamer::animationPagesToRemove(nativeSession->pdfPageMap, nativeSession->pdfPageLabels);
    if (pageIndices.empty()) {
        return true;
    }
    if (pageIndices.size() >= static_cast<size_t>(pageCount)) {
        error = "Beamer cleanup cannot remove every page in a document";
        return false;
    }

    finishTextEditing();
    resetInput(true);
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto pageSizes = currentPageSizes(state);
    const auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    runDocumentJob(
            "Removing Beamer animation pages…",
            [session, worker, pageIndices, pageSizes, staged] {
                *staged = session->stageDeletePagesAt(worker, pageIndices, pageSizes);
            },
            [this, session, pageIndices, staged] {
                PopplerDocument* candidate = nullptr;
                std::vector<PopplerDocument*> candidateLayers;
                std::vector<PopplerDocument*> candidateAudioLayers;
                std::vector<prototype::PageSize> sizes;
                try {
                    std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                    candidateLayers = openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                    candidateAudioLayers =
                            openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(), static_cast<int>(sizes.size()));
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
                for (const int pageIndex: pageIndices) {
                    state.deletePageAt(pageIndex);
                }
                pageCount = static_cast<int>(sizes.size());
                state.setPages(std::move(sizes));
                state.selectActivePage(std::min(pageIndices.back(), pageCount - 1));
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
                navigateToPage(state.page);
                updateTitle();
            });
    return true;
}

void Prototype::showLayerActionsNotice(const char* message) {
    std::string error;
    if (!showPluginDialog("LayerActions", message, {{1, "OK"}}, true, {}, error)) {
        showError(error);
    }
}

void Prototype::startLayerActionsClone(int currentPage, int pdfPage, bool allowAnnotated,
                                       const std::shared_ptr<prototype::bridge::DocumentSession>& session,
                                       const std::string& worker,
                                       const std::vector<prototype::bridge::StrokePacket>& packets,
                                       const std::vector<prototype::TextItem>& texts,
                                       const std::vector<prototype::bridge::ImagePacket>& images,
                                       const prototype::bridge::LayerDocument& layers,
                                       const std::vector<prototype::bridge::PageSizePacket>& pageSizes) {
    if (nativeSession != session || currentPage < 0 || currentPage + 1 >= pageCount) {
        showError("The LayerActions source page changed before cloning could start");
        return;
    }
    auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    runDocumentJob(
            "Cloning layers to the next PDF page…",
            [session, worker, currentPage, pdfPage, allowAnnotated, packets, texts, images, layers, pageSizes, staged] {
                *staged = session->stageLayerActionsCloneNext(worker, currentPage, pdfPage, allowAnnotated, packets,
                                                              texts, images, layers, pageSizes);
            },
            [this, session, currentPage, staged] {
                PopplerDocument* candidate = nullptr;
                std::vector<PopplerDocument*> candidateLayers;
                std::vector<PopplerDocument*> candidateAudioLayers;
                std::vector<prototype::PageSize> sizes;
                prototype::bridge::LayerDocument layerState;
                std::vector<Ink> hydratedInk;
                std::vector<prototype::TextItem> hydratedTexts;
                std::vector<prototype::CanvasImage> hydratedImages;
                try {
                    std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                    candidateLayers = openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                    candidateAudioLayers =
                            openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(), static_cast<int>(sizes.size()));
                    layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                    hydratedInk =
                            prototype::bridge::sessionInk(prototype::bridge::readTransfer(staged->editableInkPath()));
                    hydratedTexts = prototype::bridge::readTextTransfer(staged->editableTextPath());
                    const auto imagePackets = prototype::bridge::readImageTransfer(staged->editableImagesPath());
                    hydratedImages.reserve(imagePackets.size());
                    for (const auto& packet: imagePackets) {
                        hydratedImages.push_back(prototype::hydrateCanvasImage(prototype::bridge::canvasImage(packet)));
                    }
                    if (sizes.size() != static_cast<size_t>(pageCount) || layerState.size() != sizes.size() ||
                        currentPage + 1 >= static_cast<int>(sizes.size())) {
                        throw std::runtime_error("LayerActions clone output does not match the current document");
                    }
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
                if (!state.replaceNextPageWithClone(currentPage, sizes.at(static_cast<size_t>(currentPage + 1)),
                                                    staged->insertedPageHistorySnapshotPath.string(),
                                                    staged->pageHistorySnapshotPath.string())) {
                    throw std::runtime_error("Cannot replace the next page with the cloned page");
                }
                pageCount = static_cast<int>(sizes.size());
                state.setPages(std::move(sizes));
                state.setLayerDocument(layerState);
                state.ink = std::move(hydratedInk);
                state.texts = std::move(hydratedTexts);
                state.images = std::move(hydratedImages);
                state.selectActivePage(currentPage + 1);
                selectedImageIndex.reset();
                g_clear_object(&pdfTextDocument);
                pdfTextDocumentPath.clear();
                canvasRenderer.invalidate();
                thumbnailRenderer.invalidate();
                thumbnailWidgets.clear();
                pageList = nullptr;
                adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                navigateToPage(currentPage + 1);
                updateTitle();
            });
}

bool Prototype::cloneNextPageFromLayerActions(std::string& error) {
    if (!nativeSession || options.worker.empty()) {
        error = "Cloning layers requires an open document and document worker";
        return false;
    }
    if (busy || choosingFile || messageDialog || closed) {
        error = "A document operation is already in progress";
        return false;
    }
    if (autosaving) {
        waitForAutosave([this] {
            std::string deferredError;
            if (!cloneNextPageFromLayerActions(deferredError)) {
                showError(deferredError);
            }
        });
        return true;
    }

    finishTextEditing();
    resetInput(true);
    const int currentPage = state.page;
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto packets = prototype::bridge::transferInk(state.committedInk());
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    auto target = std::make_shared<prototype::bridge::LayerActionTargetInfo>();
    runDocumentJob(
            "Checking the next page…",
            [session, worker, currentPage, packets, texts, images, layers, target] {
                *target = session->inspectLayerActionTarget(worker, currentPage, packets, texts, images, layers);
            },
            [this, currentPage, session, worker, packets, texts, images, layers, pageSizes, target] {
                if (!target->hasNextPage) {
                    showLayerActionsNotice("No next page. ");
                    return;
                }
                if (target->nextPdfPage < 0) {
                    showLayerActionsNotice("Next page has no pdf background. ");
                    return;
                }
                if (!target->nextPageAnnotated) {
                    startLayerActionsClone(currentPage, target->nextPdfPage, false, session, worker, packets, texts,
                                           images, layers, pageSizes);
                    return;
                }

                std::string dialogError;
                const bool shown = showPluginDialog(
                        "LayerActions", "Next page contains annotations that will be lost, when proceeding. ",
                        {{1, "Cancel"}, {2, "Proceed"}}, false,
                        [this, currentPage, pdfPage = target->nextPdfPage, session, worker, packets, texts, images,
                         layers, pageSizes](int response) {
                            if (response == 2) {
                                startLayerActionsClone(currentPage, pdfPage, true, session, worker, packets, texts,
                                                       images, layers, pageSizes);
                            }
                        },
                        dialogError);
                if (!shown) {
                    showError(dialogError);
                }
            });
    return true;
}

bool Prototype::hideLayersFromLayerActions(std::string& error) {
    if (busy || choosingFile || messageDialog || closed) {
        error = "A document operation is already in progress";
        return false;
    }
    if (autosaving) {
        waitForAutosave([this] {
            std::string deferredError;
            if (!hideLayersFromLayerActions(deferredError)) {
                showError(deferredError);
            }
        });
        return true;
    }

    finishTextEditing();
    resetInput(true);
    const int currentPage = state.page;
    for (int page = 0; page < state.pageCount(); ++page) {
        const auto count = state.layersForPage(page).layers.size();
        if (!state.selectLayer(page, 1)) {
            error = "LayerActions could not select the first layer on every page";
            return false;
        }
        for (size_t layer = 1; layer <= count; ++layer) {
            state.setLayerVisible(page, layer, layer == 1);
        }
    }
    state.selectActivePage(currentPage);
    navigateToPage(currentPage);
    refreshAfterLayerMutation();
    return true;
}

bool Prototype::addTopLayerToEveryPageFromLayerActions(std::string& error) {
    if (busy || choosingFile || messageDialog || closed) {
        error = "A document operation is already in progress";
        return false;
    }
    if (autosaving) {
        waitForAutosave([this] {
            std::string deferredError;
            if (!addTopLayerToEveryPageFromLayerActions(deferredError)) {
                showError(deferredError);
            }
        });
        return true;
    }
    for (int page = 0; page < state.pageCount(); ++page) {
        if (state.layersForPage(page).layers.size() >= prototype::bridge::maxLayersPerPage) {
            error = "A page already has the maximum supported number of layers";
            return false;
        }
    }

    finishTextEditing();
    resetInput(true);
    const int currentPage = state.page;
    for (int page = 0; page < state.pageCount(); ++page) {
        const auto topLayer = state.layersForPage(page).layers.size();
        state.selectActivePage(page);
        if (!state.selectLayer(page, topLayer) || !state.addLayerAboveCurrent()) {
            error = "LayerActions could not add a top layer to every page";
            return false;
        }
    }
    state.selectActivePage(currentPage);
    navigateToPage(currentPage);
    refreshAfterLayerMutation();
    return true;
}

bool Prototype::activatePluginAction(std::string_view action, std::string& error) {
    if (action == "search") {
        if (!searchController.widget() || busy || choosingFile || messageDialog || closed) {
            error = "Search cannot open while the document is busy";
            return false;
        }
        openSearchBar();
    } else if (action == "fill-opacity") {
        return openFillOpacityDialog(error);
    } else if (action == "goto-next") {
        navigateToPage(state.page + 1);
    } else if (action == "goto-previous") {
        navigateToPage(state.page - 1);
    } else if (action == "goto-page") {
        openGotoPageDialog();
    } else if (action == "undo") {
        performHistoryStep(false);
    } else if (action == "redo") {
        performHistoryStep(true);
    } else if (action == "select-default-tool") {
        selectTool(Tool::Pen);
    } else if (action == "beamer-next") {
        return stepBeamerPresentation(1, error);
    } else if (action == "beamer-previous") {
        return stepBeamerPresentation(-1, error);
    } else if (action == "beamer-clean") {
        return cleanBeamerAnimations(error);
    } else if (action == "layer-actions-clone-next") {
        return cloneNextPageFromLayerActions(error);
    } else if (action == "layer-actions-hide") {
        return hideLayersFromLayerActions(error);
    } else if (action == "layer-actions-add-top") {
        return addTopLayerToEveryPageFromLayerActions(error);
    } else if (action == "layer-new-above-current") {
        if (busy || choosingFile || messageDialog || closed || autosaving) {
            error = "Wait for the current document operation to finish before adding a layer";
            return false;
        }
        if (!state.addLayerAboveCurrent()) {
            error = "A layer could not be added above the current layer";
            return false;
        }
        refreshAfterLayerMutation();
    } else {
        error = "This master action is not available in the GTK 4 prototype yet: " + std::string(action);
        return false;
    }
    return true;
}

}  // namespace prototype::gtk4
