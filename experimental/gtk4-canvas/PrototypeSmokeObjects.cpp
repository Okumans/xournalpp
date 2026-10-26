#include "Prototype.h"

namespace prototype::gtk4 {

gboolean Prototype::smokeImages() {
    require(imageVerifyMode == "insert-resize-save-reopen" && !imageVerifySource.empty(),
            "Image verification requires a source fixture and supported mode");
    if ((imageVerifyStep == 5 || imageVerifyStep >= 8) && (busy || autosaving)) {
        return G_SOURCE_CONTINUE;
    }
    switch (imageVerifyStep++) {
        case 0:
            if (!nativeSession || !gtk_widget_get_sensitive(imageButton)) {
                --imageVerifyStep;
                return G_SOURCE_CONTINUE;
            }
            require(state.images.empty(), "Image verification document should start without editable images");
            g_signal_emit_by_name(imageButton, "clicked");
            require(choosingFile, "Insert Image action did not open its native image chooser");
            break;
        case 1:
            if (!cancelPrivateChooser()) {
                require(++chooserWaits < 60, "Insert Image chooser did not appear");
                --imageVerifyStep;
                return G_SOURCE_CONTINUE;
            }
            break;
        case 2:
            require(!choosingFile, "Cancelling Insert Image did not close the image chooser");
            insertImageFromPath(imageVerifySource);
            require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0} && modified(),
                    "Insert Image did not add a selected, undoable image");
            break;
        case 3: {
            capture("image-inserted");
            break;
        }
        case 4: {
            state.clearSelection();
            activateSelection(Tool::SelectRectangle);
            const auto selectionBounds = prototype::canvasImageBounds(state.images.at(0));
            const auto selectionStart =
                    state.pageScreenPoint(state.page, selectionBounds.x0 - 2, selectionBounds.y0 - 2);
            const auto selectionEnd = state.pageScreenPoint(state.page, selectionBounds.x1 + 2, selectionBounds.y1 + 2);
            begin(Device::Mouse, selectionStart.x, selectionStart.y);
            motion(Device::Mouse, selectionEnd.x, selectionEnd.y);
            finish(Device::Mouse);
            require(state.selectedImages == std::vector<size_t>{0},
                    "Rectangle selection did not select the inserted image");
            const auto originalScale = state.images.at(0).transform.xx;
            const auto corners = prototype::canvasImageCorners(state.images.at(0));
            const auto start = state.pageScreenPoint(state.images[0].page, corners[2].x, corners[2].y);
            const auto releasePoint =
                    state.pageScreenPoint(state.images[0].page, corners[2].x + 10, corners[2].y + 7.5);
            begin(Device::Mouse, start.x, start.y);
            motion(Device::Mouse, releasePoint.x, releasePoint.y);
            finish(Device::Mouse);
            require(state.images[0].transform.xx > originalScale && state.canUndo(),
                    "Dragging an image corner did not resize the image");
            capture("image-resized");
            break;
        }
        case 5: {
            const auto& image = state.images.at(0);
            const auto center = prototype::canvasImagePoint(image, image.pixelWidth / 2.0, image.pixelHeight / 2.0);
            const auto start = state.pageScreenPoint(image.page, center.x, center.y);
            const auto releasePoint = state.pageScreenPoint(image.page, center.x + 24, center.y + 19);
            const double originalX = image.transform.x;
            const double originalY = image.transform.y;
            begin(Device::Mouse, start.x, start.y);
            motion(Device::Mouse, releasePoint.x, releasePoint.y);
            finish(Device::Mouse);
            require(std::abs(state.images[0].transform.x - originalX - 24) < 1e-6 &&
                            std::abs(state.images[0].transform.y - originalY - 19) < 1e-6,
                    "Dragging an image body did not move the image");
            const auto movedTransform = state.images[0].transform;
            const auto centerBeforeRotation = prototype::canvasImageCenter(state.images[0]);
            const auto rotationHandle = state.selectionHandles()[8];
            const double handleRadius =
                    std::hypot(rotationHandle.x - centerBeforeRotation.x, rotationHandle.y - centerBeforeRotation.y);
            const double startAngle =
                    std::atan2(rotationHandle.y - centerBeforeRotation.y, rotationHandle.x - centerBeforeRotation.x);
            const double targetAngle = startAngle + G_PI / 6.0 + 0.01;
            const prototype::Point rotatedHandle{centerBeforeRotation.x + handleRadius * std::cos(targetAngle),
                                                 centerBeforeRotation.y + handleRadius * std::sin(targetAngle)};
            const auto rotationStart = state.pageScreenPoint(state.images[0].page, rotationHandle.x, rotationHandle.y);
            const auto rotationEnd = state.pageScreenPoint(state.images[0].page, rotatedHandle.x, rotatedHandle.y);
            begin(Device::Mouse, rotationStart.x, rotationStart.y);
            motion(Device::Mouse, rotationEnd.x, rotationEnd.y);
            finish(Device::Mouse);
            require(std::abs(state.images[0].transform.xy) > 1e-6 && std::abs(state.images[0].transform.yx) > 1e-6 &&
                            std::abs(std::atan2(state.images[0].transform.yx, state.images[0].transform.xx) -
                                     G_PI / 6.0) < 1e-6 &&
                            std::hypot(prototype::canvasImageCenter(state.images[0]).x - centerBeforeRotation.x,
                                       prototype::canvasImageCenter(state.images[0]).y - centerBeforeRotation.y) < 1e-6,
                    "Image rotation should snap to 30 degrees around its center");
            const auto snappedTransform = state.images[0].transform;
            const auto snappedHandle = state.selectionHandles()[8];
            const double snappedHandleAngle =
                    std::atan2(snappedHandle.y - centerBeforeRotation.y, snappedHandle.x - centerBeforeRotation.x);
            const prototype::Point unsnappedHandle{
                    centerBeforeRotation.x + handleRadius * std::cos(snappedHandleAngle + 2 * G_PI / 180.0),
                    centerBeforeRotation.y + handleRadius * std::sin(snappedHandleAngle + 2 * G_PI / 180.0)};
            const auto unsnappedStart = state.pageScreenPoint(state.images[0].page, snappedHandle.x, snappedHandle.y);
            const auto unsnappedEnd = state.pageScreenPoint(state.images[0].page, unsnappedHandle.x, unsnappedHandle.y);
            input.setModifiers(false, false, true);
            begin(Device::Mouse, unsnappedStart.x, unsnappedStart.y);
            motion(Device::Mouse, unsnappedEnd.x, unsnappedEnd.y);
            finish(Device::Mouse);
            input.setModifiers(false, false, false);
            const double altRotation = std::atan2(state.images[0].transform.yx, state.images[0].transform.xx);
            require(std::abs(altRotation - (G_PI / 6.0 + 2 * G_PI / 180.0)) < 1e-6,
                    "Holding Alt should temporarily disable rotation snapping");
            const auto rotatedTransform = state.images[0].transform;
            state.undo();
            require(std::abs(state.images[0].transform.xx - snappedTransform.xx) < 1e-9 &&
                            std::abs(state.images[0].transform.xy - snappedTransform.xy) < 1e-9 &&
                            std::abs(state.images[0].transform.yx - snappedTransform.yx) < 1e-9 &&
                            std::abs(state.images[0].transform.yy - snappedTransform.yy) < 1e-9,
                    "Undo should restore the snapped image rotation after an Alt rotation");
            state.undo();
            require(std::abs(state.images[0].transform.xx - movedTransform.xx) < 1e-9 &&
                            std::abs(state.images[0].transform.xy - movedTransform.xy) < 1e-9 &&
                            std::abs(state.images[0].transform.yx - movedTransform.yx) < 1e-9 &&
                            std::abs(state.images[0].transform.yy - movedTransform.yy) < 1e-9,
                    "Undo should restore the image transform from before rotation");
            state.redo();
            require(std::abs(state.images[0].transform.xx - snappedTransform.xx) < 1e-9 &&
                            std::abs(state.images[0].transform.xy - snappedTransform.xy) < 1e-9 &&
                            std::abs(state.images[0].transform.yx - snappedTransform.yx) < 1e-9 &&
                            std::abs(state.images[0].transform.yy - snappedTransform.yy) < 1e-9,
                    "Redo should restore the snapped image rotation");
            state.redo();
            require(std::abs(state.images[0].transform.xy - rotatedTransform.xy) < 1e-9 &&
                            std::abs(state.images[0].transform.yx - rotatedTransform.yx) < 1e-9,
                    "Redo should restore the unsnapped Alt image rotation");
            imageVerifyExpected = state.images[0].transform;
            saveCopyTo((std::filesystem::path(options.smokeDirectory) / "image-saved.xopp").string());
            break;
        }
        case 6: {
            if (busy || autosaving) {
                --imageVerifyStep;
                return G_SOURCE_CONTINUE;
            }
            const auto path = (std::filesystem::path(options.smokeDirectory) / "image-saved.xopp").string();
            require(!modified(), "Saving a document with an edited image left it modified");
            require(savedCopyPath == path, "Saving an image copy did not update its destination");
            require(std::filesystem::is_regular_file(path), "Saving an image copy did not create the file");
            capture("image-rotated");
            requestOpenDocument(path);
            break;
        }
        case 7: {
            require(state.images.size() == 1 && state.images[0].encodedData.size() > 0 &&
                            std::abs(state.images[0].transform.xx - imageVerifyExpected.xx) < 1e-4 &&
                            std::abs(state.images[0].transform.yx - imageVerifyExpected.yx) < 1e-4 &&
                            std::abs(state.images[0].transform.xy - imageVerifyExpected.xy) < 1e-4 &&
                            std::abs(state.images[0].transform.yy - imageVerifyExpected.yy) < 1e-4 &&
                            std::abs(state.images[0].transform.x - imageVerifyExpected.x) < 1e-4 &&
                            std::abs(state.images[0].transform.y - imageVerifyExpected.y) < 1e-4,
                    "Saving and reopening changed the native image or its resized placement");
            capture("image-reopened");
            const auto pdf = (std::filesystem::path(options.smokeDirectory) / "image-exported.pdf").string();
            exportPdfTo(pdf);
            break;
        }
        default: {
            const auto pdf = (std::filesystem::path(options.smokeDirectory) / "image-exported.pdf").string();
            require(exportedPdfPath == pdf && std::filesystem::is_regular_file(pdf),
                    "Exporting the image document did not create a PDF");
            if (imageVerifyStep == 9) {
                activateSelection(Tool::SmartSelect);
                state.selectedImages = {0};
                copySelection(true);
                require(state.images.empty() && state.selectedImages.empty(),
                        "Cut did not remove the selected image after copying it");
                auto* clipboard = gtk_widget_get_clipboard(canvas);
                auto* formats = gdk_clipboard_get_formats(clipboard);
                require(gdk_content_formats_contain_mime_type(formats, prototype::clipboard::mime) &&
                                gdk_content_formats_contain_mime_type(formats, "image/png"),
                        "Image copy did not offer both the editable native object and PNG");
                pasteSelection();
                require(busy, "Native image paste did not begin its asynchronous clipboard read");
                break;
            }
            if (imageVerifyStep == 10) {
                require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0} &&
                                state.images[0].encodedData.size() > 0,
                        "Native image clipboard paste did not restore an editable image");
                state.undo();
                require(state.images.empty(), "Undo did not remove the pasted image");
                state.redo();
                require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0},
                        "Redo did not restore the pasted image selection");

                const auto pngData = prototype::clipboard::renderImagePng(state.images[0]);
                auto* bytes = g_bytes_new(pngData.data(), pngData.size());
                auto* provider = gdk_content_provider_new_for_bytes("image/png", bytes);
                g_bytes_unref(bytes);
                require(gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas), provider),
                        "Could not offer a PNG-only clipboard fixture");
                g_object_unref(provider);
                state.deleteImage(0);
                pasteSelection();
                require(busy, "PNG clipboard paste did not begin its asynchronous read");
                break;
            }
            if (imageVerifyStep == 11) {
                require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0} &&
                                prototype::validCanvasImage(state.images[0]),
                        "A standard PNG clipboard image was not pasted as an editable object");
                state.undo();
                selectedImageIndex.reset();
                require(state.images.empty(), "Undo did not remove the PNG-pasted image");
                state.redo();
                require(state.images.size() == 1, "Redo did not restore the PNG-pasted image");
                capture("image-png-pasted");

                duplicateSelection();
                require(state.images.size() == 2 && state.selectedImages == std::vector<size_t>{1},
                        "Single-image duplicate did not add an editable image");
                state.undo();
                require(state.images.size() == 1, "Undo did not remove the image duplicate");
                state.redo();
                require(state.images.size() == 2, "Redo did not restore the image duplicate");

                state.clearSelection();
                state.selectedImages = {0, 1};
                imageVerifyMultiDeltaX = state.images[1].transform.x - state.images[0].transform.x;
                imageVerifyMultiDeltaY = state.images[1].transform.y - state.images[0].transform.y;
                duplicateSelection();
                require(state.images.size() == 4 && state.selectedImages == std::vector<size_t>{2, 3} &&
                                std::abs(state.images[3].transform.x - state.images[2].transform.x -
                                         imageVerifyMultiDeltaX) < 1e-6 &&
                                std::abs(state.images[3].transform.y - state.images[2].transform.y -
                                         imageVerifyMultiDeltaY) < 1e-6,
                        "Duplicating a multi-image selection should preserve its relative layout");
                state.undo();
                require(state.images.size() == 2, "One undo did not remove the complete image duplicate group");
                state.redo();
                require(state.images.size() == 4 && state.selectedImages == std::vector<size_t>{2, 3},
                        "Redo did not restore the complete image duplicate group");

                state.clearSelection();
                state.selectedImages = {0, 1};
                copySelection(true);
                require(state.images.size() == 2 && state.selectedImages.empty(),
                        "Cut did not remove the selected images as one operation");
                auto* clipboard = gtk_widget_get_clipboard(canvas);
                auto* formats = gdk_clipboard_get_formats(clipboard);
                require(gdk_content_formats_contain_mime_type(formats, prototype::clipboard::mime) &&
                                gdk_content_formats_contain_mime_type(formats, "image/png"),
                        "Multi-image copy did not offer native objects and a composite PNG");
                state.undo();
                require(state.images.size() == 4, "One undo did not restore the complete image cut group");
                state.redo();
                require(state.images.size() == 2, "Redo did not remove the complete image cut group");
                pasteSelection();
                require(busy, "Multi-image clipboard paste did not begin its asynchronous read");
                break;
            }
            require(imageVerifyStep == 12 && state.images.size() == 4 &&
                            state.selectedImages == std::vector<size_t>{2, 3} &&
                            std::abs(state.images[3].transform.x - state.images[2].transform.x -
                                     imageVerifyMultiDeltaX) < 1e-6 &&
                            std::abs(state.images[3].transform.y - state.images[2].transform.y -
                                     imageVerifyMultiDeltaY) < 1e-6,
                    "Native multi-image clipboard paste did not preserve both images and their relative layout");
            state.undo();
            require(state.images.size() == 2, "One undo did not remove the complete image paste group");
            state.redo();
            require(state.images.size() == 4 && state.selectedImages == std::vector<size_t>{2, 3},
                    "Redo did not restore the complete image paste group");
            capture("image-multi-pasted");
            std::cout << "PASS: image insertion, resize, move, rotate, native save/reopen, PDF export, PNG "
                         "paste, and multi-image native copy/cut/paste, duplicate and grouped undo/redo\n";
            closeWindow();
            return G_SOURCE_REMOVE;
        }
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

gboolean Prototype::smokeLaserPointers() {
    switch (laserVerifyStep++) {
        case 0: {
            require(bool(nativeSession), "Laser verification requires a native document fixture");
            laserInitialInkCount = state.ink.size();
            laserInitialRevision = state.revision();
            laserInitialUndo = state.canUndo();
            selectTool(Tool::LaserPointerPen);
            require(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(laserButtons[0])) &&
                            std::abs(state.brush.width - 2.4) < 1e-6,
                    "Laser Pointer Pen toolbar selection or default preset mismatch");
            const auto from = state.pageScreenPoint(state.page, 70, 110);
            const auto to = state.pageScreenPoint(state.page, 210, 150);
            require(input.begin(Device::Stylus, {from.x, from.y, 0.25}),
                    "Laser Pointer Pen did not accept stylus input");
            input.motion(Device::Stylus, {to.x, to.y, 0.8});
            input.finish(Device::Stylus, 0.9);
            require(state.laserTrails.size() == 1 && state.laserTrails.front().strokes.size() == 1 &&
                            state.laserTrails.front().strokes.front().brush.tool == Tool::LaserPointerPen &&
                            state.ink.size() == laserInitialInkCount && state.revision() == laserInitialRevision &&
                            state.canUndo() == laserInitialUndo,
                    "Laser Pointer Pen wrote into the document or undo history");
            update();
            waitForNextPaint();
            capture("laser-pen");
            break;
        }
        case 1: {
            selectTool(Tool::LaserPointerHighlighter);
            require(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(laserButtons[1])) &&
                            std::abs(state.brush.width - 8.5) < 1e-6,
                    "Laser Pointer Highlighter toolbar selection or default preset mismatch");
            const auto from = state.pageScreenPoint(state.page, 70, 190);
            const auto to = state.pageScreenPoint(state.page, 220, 190);
            require(input.begin(Device::Mouse, {from.x, from.y, std::nullopt}),
                    "Laser Pointer Highlighter did not accept mouse input");
            input.motion(Device::Mouse, {to.x, to.y, std::nullopt});
            input.finish(Device::Mouse);
            require(state.laserTrails.size() == 1 && state.laserTrails.front().strokes.size() == 2 &&
                            state.laserTrails.front().strokes.back().brush.tool == Tool::LaserPointerHighlighter &&
                            !state.laserTrails.front().strokes.back().pressureSensitive &&
                            state.ink.size() == laserInitialInkCount && state.revision() == laserInitialRevision,
                    "Laser highlighter did not share a transient trail without changing the document");
            update();
            waitForNextPaint();
            capture("laser-highlighter");
            break;
        }
        case 2: {
            require(state.laserTrails.size() == 1 && state.laserTrails.front().fadeStart,
                    "Completed laser marks did not schedule their fade delay");
            require(state.laserTrails.front().alpha == 255,
                    "Laser marks began fading before the configured delay elapsed");
            break;
        }
        case 3:
            if (state.laserTrails.size() == 1 && state.laserTrails.front().alpha == 255) {
                --laserVerifyStep;
                return G_SOURCE_CONTINUE;
            }
            require(state.laserTrails.size() == 1 && state.laserTrails.front().alpha < 255,
                    "The real GTK timeout did not start fading the laser trail");
            capture("laser-fading");
            break;
        case 4:
            if (!state.laserTrails.empty()) {
                --laserVerifyStep;
                return G_SOURCE_CONTINUE;
            }
            capture("laser-faded");
            require(state.ink.size() == laserInitialInkCount && state.revision() == laserInitialRevision &&
                            state.canUndo() == laserInitialUndo,
                    "Faded laser marks changed saved or undoable document state");
            break;
        case 5:
            laserOutputPath = (std::filesystem::path(options.smokeDirectory) / "laser-pointer-saved.xopp").string();
            saveCopyTo(laserOutputPath, [this] { laserSaveComplete = true; });
            require(busy, "Saving after laser input did not start the worker operation");
            break;
        default:
            require(laserSaveComplete && std::filesystem::is_regular_file(laserOutputPath),
                    "The native document save after laser input did not finish");
            std::cout << "PASS: GTK laser pointer pen/highlighter trails fade without entering document history\n";
            closeWindow();
            return G_SOURCE_REMOVE;
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

gboolean Prototype::smokePlayObject() {
    switch (playObjectVerifyStep++) {
        case 0: {
            require(nativeSession && !audioTargets.empty() && !audioLayerDocuments.empty(),
                    "Play Object verification requires native audio-target and faded-layer previews");
            require(bool(audioPlayback), "Play Object verification has no audio playback controller");
            const auto& target = audioTargets.front();
            double localX = 0;
            double localY = 0;
            if (target.shape == prototype::AudioTargetShape::Stroke) {
                const auto& point = target.points[target.points.size() / 2];
                localX = point.x;
                localY = point.y;
            } else {
                localX = target.xx * target.naturalWidth / 2.0 + target.xy * target.naturalHeight / 2.0 + target.x;
                localY = target.yx * target.naturalWidth / 2.0 + target.yy * target.naturalHeight / 2.0 + target.y;
            }
            selectTool(Tool::PlayObject);
            waitForNextPaint();
            capture("play-object-faded");
            const auto click = state.pageScreenPoint(target.page, localX, localY);
            begin(Device::Mouse, click.x, click.y);
            require(audioPlayback->isActive() && gtk_widget_get_visible(audioControls),
                    "Clicking an audio-linked object did not start playback and show its controls");
            g_signal_emit_by_name(audioPauseButton, "clicked");
            require(audioPlayback->isPaused(), "Play Object playback did not pause");
            g_signal_emit_by_name(audioBackButton, "clicked");
            g_signal_emit_by_name(audioForwardButton, "clicked");
            g_signal_emit_by_name(audioPauseButton, "clicked");
            require(!audioPlayback->isPaused(), "Play Object playback did not resume");
            audioPlayback->stop();
            require(!audioPlayback->isActive() && !gtk_widget_get_visible(audioControls),
                    "Stopping audio did not hide the playback controls");
            selectTool(Tool::Pen);
            waitForNextPaint();
            capture("play-object-normal");
            selectTool(Tool::PlayObject);
            waitForNextPaint();
            capture("play-object-faded-again");
            break;
        }
        case 1: {
            const auto outside = state.pageScreenPoint(state.page, 25, 700);
            begin(Device::Mouse, outside.x, outside.y);
            require(!audioPlayback->isActive(), "Clicking away from audio-linked objects started playback");
            playObjectSaveOutput = (std::filesystem::path(options.smokeDirectory) / "play-object-saved.xopp").string();
            saveCopyTo(playObjectSaveOutput, [this] { playObjectSaveComplete = true; });
            require(busy, "Saving the audio-linked native document did not start");
            break;
        }
        default:
            require(playObjectSaveComplete && std::filesystem::is_regular_file(playObjectSaveOutput),
                    "The native document save after Play Object verification did not finish");
            std::cout << "PASS: GTK Play Object hit testing, playback controls, fading, and native save\n";
            closeWindow();
            return G_SOURCE_REMOVE;
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

gboolean Prototype::smokeLinks() {
    switch (linkVerifyStep++) {
        case 0: {
            require(nativeSession && state.texts.size() >= 1 &&
                            std::any_of(state.texts.begin(), state.texts.end(),
                                        [](const prototype::TextItem& item) { return item.isLink; }),
                    "Link verification requires an editable native link in the source document");
            const auto original = std::find_if(state.texts.begin(), state.texts.end(),
                                               [](const prototype::TextItem& item) { return item.isLink; });
            const size_t originalIndex = static_cast<size_t>(std::distance(state.texts.begin(), original));
            const auto originalUrl = original->linkUrl;
            const auto originalText = original->text;
            const auto linkPoint = state.pageScreenPoint(original->page, original->x + 2, original->y + 2);

            selectTool(Tool::Link);
            updateLinkHover(linkPoint.x, linkPoint.y);
            require(linkInfoPopover && gtk_widget_get_visible(linkInfoPopover),
                    "Hovering an existing link did not show its URL popover");
            handleLinkClick(linkPoint.x, linkPoint.y, 1, true);
            require(openedLinkForTest == originalUrl,
                    "Control-click did not activate the URL for the link under the pointer");
            handleLinkClick(linkPoint.x, linkPoint.y, 2, false);
            require(messageDialog && editingLinkIndex == originalIndex,
                    "Double-clicking an existing link did not open its editor");
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), "Edited GTK4 link", -1);
            gtk_drop_down_set_selected(linkPrefixDropdown, 0);
            gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), "migration.example/phase29");
            gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), "Sans");
            gtk_spin_button_set_value(linkFontSize, 15);
            gtk_drop_down_set_selected(linkAlignmentDropdown, 1);
            answerDialog("save");
            require(state.texts[originalIndex].isLink && state.texts[originalIndex].text == "Edited GTK4 link" &&
                            state.texts[originalIndex].linkUrl == "https://migration.example/phase29" &&
                            state.texts[originalIndex].alignment == 1 && state.canUndo(),
                    "Editing a native link did not preserve its type, save its fields and enter undo history");
            const auto editedPoint =
                    state.pageScreenPoint(state.texts[originalIndex].page, state.texts[originalIndex].x + 2,
                                          state.texts[originalIndex].y + 2);
            openedLinkForTest.clear();
            handleLinkClick(editedPoint.x, editedPoint.y, 1, true);
            require(openedLinkForTest == "https://migration.example/phase29",
                    "Control-click after editing did not use the saved URL");
            state.undo();
            require(state.texts[originalIndex].text == originalText &&
                            state.texts[originalIndex].linkUrl == originalUrl,
                    "Undo did not restore the original native link");
            state.redo();
            require(state.texts[originalIndex].text == "Edited GTK4 link" &&
                            state.texts[originalIndex].linkUrl == "https://migration.example/phase29",
                    "Redo did not restore the edited native link");
            capture("link-edited");

            const size_t beforeInsert = state.texts.size();
            const auto newPosition = state.pageScreenPoint(state.page, 330, 280);
            handleLinkClick(newPosition.x, newPosition.y, 1, false);
            require(messageDialog && !editingLinkIndex, "Clicking empty page space did not open the Add Link form");
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), "Contact", -1);
            gtk_drop_down_set_selected(linkPrefixDropdown, 2);
            gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), "migration@example.org");
            gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), "Sans");
            gtk_spin_button_set_value(linkFontSize, 12);
            gtk_drop_down_set_selected(linkAlignmentDropdown, 0);
            answerDialog("save");
            require(state.texts.size() == beforeInsert + 1 && state.texts.back().isLink &&
                            state.texts.back().text == "Contact" &&
                            state.texts.back().linkUrl == "mailto:migration@example.org" && state.canUndo(),
                    "Creating a link did not add a native, undoable link object");
            const auto newIndex = state.texts.size() - 1;
            state.undo();
            require(state.texts.size() == beforeInsert, "Undo did not remove the newly created link");
            state.redo();
            require(state.texts.size() == beforeInsert + 1 &&
                            state.texts[newIndex].linkUrl == "mailto:migration@example.org",
                    "Redo did not restore the newly created link");
            capture("link-created");

            linkOutputPath = (std::filesystem::path(options.smokeDirectory) / "links-saved.xopp").string();
            saveCopyTo(linkOutputPath, [this] { linkSaveComplete = true; });
            require(busy, "Saving edited links did not start a document operation");
            break;
        }
        default: {
            require(linkSaveComplete && std::filesystem::is_regular_file(linkOutputPath),
                    "The native link save did not finish");
            capture("link-saved");
            std::cout << "PASS: GTK native links create, edit, activate, undo/redo and save\n";
            closeWindow();
            return G_SOURCE_REMOVE;
        }
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

void Prototype::verifyTextClipboard() {
    const size_t originalStrokeCount = state.ink.size();
    const size_t originalTextCount = state.texts.size();
    const size_t originalImageCount = state.images.size();
    prototype::TextItem richText;
    richText.page = state.page;
    richText.text = "Rich clipboard text";
    richText.fontName = "Sans";
    richText.fontSize = 12;
    richText.x = 70;
    richText.y = 600;
    richText.layoutWidth = 130;
    richText.layoutHeight = 18;
    richText.styleRuns = {{0, 4, "Sans Bold", 14, 0xffcc2244U, 1}};
    prototype::TextItem linkText;
    linkText.page = state.page;
    linkText.text = "linked text";
    linkText.fontName = "Sans";
    linkText.fontSize = 12;
    linkText.isLink = true;
    linkText.linkUrl = "https://example.org/clipboard";
    linkText.x = 70;
    linkText.y = 640;
    linkText.layoutWidth = 90;
    linkText.layoutHeight = 18;
    require(state.insertText(richText) && state.insertText(linkText),
            "Could not create rich text and link fixtures for clipboard verification");
    const size_t firstTextIndex = state.texts.size() - 2;
    state.selected.clear();
    state.selectedImages.clear();
    state.selectedTexts = {firstTextIndex, firstTextIndex + 1};
    duplicateSelection();
    require(state.texts.size() == originalTextCount + 4, "Duplicate did not add the complete selected text group");
    state.undo();
    require(state.texts.size() == originalTextCount + 2 &&
                    state.selectedTexts == std::vector<size_t>{firstTextIndex, firstTextIndex + 1},
            "One undo did not remove the duplicated text group and restore its source selection");
    copySelection();
    const auto* textFormats = gdk_clipboard_get_formats(gtk_widget_get_clipboard(canvas));
    require(gdk_content_formats_contain_mime_type(textFormats, prototype::clipboard::mime) &&
                    gdk_content_formats_contain_mime_type(textFormats, "text/plain;charset=utf-8"),
            "Copy did not offer native Xournal and plain-text targets");
    copySelection(true);
    require(state.texts.size() == originalTextCount, "Cut did not remove the selected text and link objects");
    pasteSelection();
    const gint64 clipboardDeadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (busy && g_get_monotonic_time() < clipboardDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(!busy && state.texts.size() == originalTextCount + 2 && state.texts.back().isLink &&
                    state.texts.back().linkUrl == "https://example.org/clipboard" &&
                    state.texts[state.texts.size() - 2].styleRuns == richText.styleRuns,
            "Native text paste lost formatting, links or group insertion");
    state.undo();
    require(state.texts.size() == originalTextCount, "One undo did not remove the pasted text group");
    state.redo();
    require(state.texts.size() == originalTextCount + 2, "Redo did not restore the pasted text group");
    state.undo();
    state.undo();
    state.undo();
    state.undo();
    require(state.texts.size() == originalTextCount,
            "Undo did not restore the pre-clipboard document after the text smoke check");

    const std::string plainText = "Pasted plain text";
    auto* plainBytes = g_bytes_new(plainText.data(), plainText.size());
    auto* plainProvider = gdk_content_provider_new_for_bytes("text/plain;charset=utf-8", plainBytes);
    g_bytes_unref(plainBytes);
    require(gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas), plainProvider),
            "Could not offer a plain-text clipboard fixture");
    g_object_unref(plainProvider);
    pasteSelection();
    const gint64 plainDeadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (busy && g_get_monotonic_time() < plainDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(!busy && state.texts.size() == originalTextCount + 1 && state.texts.back().text == plainText,
            "Plain-text clipboard paste did not create an editable text object");
    state.undo();
    require(state.texts.size() == originalTextCount, "Undo did not remove pasted plain text");

    const auto layerId = state.activeLayerId(state.page);
    const auto originLayer = state.activeLayerOrigin(state.page);
    Ink mixedStroke{state.page, {{70, 690, 0.7}, {120, 705, 0.8}}, {Tool::Pen, {0.1, 0.3, 0.8}, 3}, true};
    mixedStroke.layerId = layerId;
    mixedStroke.originLayer = originLayer;
    mixedStroke.layerOrder = 1000;
    richText.page = state.page;
    richText.layerId = layerId;
    richText.originLayer = originLayer;
    richText.layerOrder = 1001;
    linkText.page = state.page;
    linkText.layerId = layerId;
    linkText.originLayer = originLayer;
    linkText.layerOrder = 1003;

    auto* mixedImageSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
    auto* mixedImageContext = cairo_create(mixedImageSurface);
    cairo_set_source_rgba(mixedImageContext, 0.2, 0.7, 0.4, 1);
    cairo_paint(mixedImageContext);
    cairo_destroy(mixedImageContext);
    std::string mixedImageBytes;
    const auto writeMixedImage = [](void* data, const unsigned char* bytes, unsigned int length) {
        auto& output = *static_cast<std::string*>(data);
        output.append(reinterpret_cast<const char*>(bytes), length);
        return CAIRO_STATUS_SUCCESS;
    };
    require(cairo_surface_write_to_png_stream(mixedImageSurface, writeMixedImage, &mixedImageBytes) ==
                            CAIRO_STATUS_SUCCESS &&
                    !mixedImageBytes.empty(),
            "Could not create a raster image for mixed clipboard verification");
    prototype::CanvasImage mixedImage;
    mixedImage.page = state.page;
    mixedImage.layerId = layerId;
    mixedImage.originLayer = originLayer;
    mixedImage.layerOrder = 1002;
    mixedImage.pixelWidth = 8;
    mixedImage.pixelHeight = 8;
    mixedImage.transform.x = 230;
    mixedImage.transform.y = 695;
    mixedImage.encodedData = std::move(mixedImageBytes);
    mixedImage.surface = std::shared_ptr<cairo_surface_t>(mixedImageSurface, cairo_surface_destroy);

    state.clearSelection();
    state.ink.push_back(mixedStroke);
    state.texts.push_back(richText);
    state.images.push_back(mixedImage);
    state.texts.push_back(linkText);
    state.selected = {state.ink.size() - 1};
    state.selectedTexts = {state.texts.size() - 2, state.texts.size() - 1};
    state.selectedImages = {state.images.size() - 1};
    selectedImageIndex.reset();
    copySelection();
    const auto* mixedFormats = gdk_clipboard_get_formats(gtk_widget_get_clipboard(canvas));
    require(gdk_content_formats_contain_mime_type(mixedFormats, prototype::clipboard::mime) &&
                    gdk_content_formats_contain_mime_type(mixedFormats, "image/png") &&
                    gdk_content_formats_contain_mime_type(mixedFormats, "image/svg") &&
                    gdk_content_formats_contain_mime_type(mixedFormats, "image/svg+xml") &&
                    gdk_content_formats_contain_mime_type(mixedFormats, "text/plain;charset=utf-8"),
            "Mixed copy did not offer native, PNG, SVG and plain-text clipboard targets");
    const size_t sourceStrokeIndex = state.ink.size() - 1;
    const size_t sourceTextIndex = state.texts.size() - 2;
    const size_t sourceImageIndex = state.images.size() - 1;
    duplicateSelection();
    require(state.ink.size() == originalStrokeCount + 2 && state.texts.size() == originalTextCount + 4 &&
                    state.images.size() == originalImageCount + 2,
            "Mixed duplicate did not add the complete object group");
    state.undo();
    require(state.ink.size() == originalStrokeCount + 1 && state.texts.size() == originalTextCount + 2 &&
                    state.images.size() == originalImageCount + 1 &&
                    state.selected == std::vector<size_t>{sourceStrokeIndex} &&
                    state.selectedTexts == std::vector<size_t>{sourceTextIndex, sourceTextIndex + 1} &&
                    state.selectedImages == std::vector<size_t>{sourceImageIndex},
            "Undo did not restore the mixed source selection after duplicate");
    copySelection(true);
    require(state.ink.size() == originalStrokeCount && state.texts.size() == originalTextCount &&
                    state.images.size() == originalImageCount,
            "Mixed cut did not remove all selected object types");
    pasteSelection();
    const gint64 mixedDeadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (busy && g_get_monotonic_time() < mixedDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    const auto pastedMixedObjects = state.copySelectedObjects();
    require(!busy && state.ink.size() == originalStrokeCount + 1 && state.texts.size() == originalTextCount + 2 &&
                    state.images.size() == originalImageCount + 1 && state.ink.back().points.size() == 2 &&
                    state.texts[state.texts.size() - 2].styleRuns == richText.styleRuns && state.texts.back().isLink &&
                    state.texts.back().linkUrl == linkText.linkUrl &&
                    state.images.back().encodedData == mixedImage.encodedData && state.selected.size() == 1 &&
                    state.selectedTexts.size() == 2 && state.selectedImages.size() == 1 &&
                    pastedMixedObjects.size() == 4 && std::holds_alternative<Ink>(pastedMixedObjects[0]) &&
                    std::holds_alternative<prototype::TextItem>(pastedMixedObjects[1]) &&
                    std::holds_alternative<prototype::CanvasImage>(pastedMixedObjects[2]) &&
                    std::holds_alternative<prototype::TextItem>(pastedMixedObjects[3]),
            "Mixed paste lost object data, relative order or selection");
    state.undo();
    require(state.ink.size() == originalStrokeCount && state.texts.size() == originalTextCount &&
                    state.images.size() == originalImageCount,
            "One undo did not remove the mixed pasted object group");
    state.redo();
    require(state.ink.size() == originalStrokeCount + 1 && state.texts.size() == originalTextCount + 2 &&
                    state.images.size() == originalImageCount + 1,
            "Redo did not restore the mixed pasted object group");
    state.undo();
    state.undo();
    require(state.ink.size() == originalStrokeCount + 1 && state.texts.size() == originalTextCount + 2 &&
                    state.images.size() == originalImageCount + 1,
            "Undo did not restore the mixed source group after paste");
    state.clearSelection();
    state.ink.resize(originalStrokeCount);
    state.texts.resize(originalTextCount);
    state.images.resize(originalImageCount);
    state.clearSelection();
    update();
    std::cout << "PASS: rich text/link and mixed clipboard copy/cut/paste/duplicate, PNG/SVG/plain-text targets, "
                 "grouped undo/redo\n";
}

}  // namespace prototype::gtk4
