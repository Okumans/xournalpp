#include "Prototype.h"

namespace prototype::gtk4 {

GtkWidget* Prototype::responseButton(GtkWidget* widget, const char* label) {
    if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0) {
        return widget;
    }
    for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        if (auto* found = responseButton(child, label)) {
            return found;
        }
    }
    return nullptr;
}

GtkWidget* Prototype::findWidgetByData(GtkWidget* widget, const char* key, const char* value) {
    if (!widget) {
        return nullptr;
    }
    const auto* stored = static_cast<const char*>(g_object_get_data(G_OBJECT(widget), key));
    if (stored && g_strcmp0(stored, value) == 0) {
        return widget;
    }
    for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        if (auto* found = findWidgetByData(child, key, value)) {
            return found;
        }
    }
    return nullptr;
}

bool Prototype::dialogHasResponseLabel(const char* response, const char* expectedLabel) const {
    if (!messageDialog || !adw_alert_dialog_has_response(messageDialog, response)) {
        if (messageDialog) {
            const char* heading = adw_alert_dialog_get_heading(messageDialog);
            const char* body = adw_alert_dialog_get_body(messageDialog);
            std::cerr << "Missing dialog response " << response << " in alert '" << (heading ? heading : "")
                      << "': " << (body ? body : "") << '\n';
        }
        return false;
    }
    return g_strcmp0(adw_alert_dialog_get_response_label(messageDialog, response), expectedLabel) == 0;
}

void Prototype::answerDialog(const char* response) {
    require(messageDialog, "Expected a document dialog");
    require(adw_alert_dialog_has_response(messageDialog, response), "Dialog response is unavailable");
    auto* button =
            responseButton(GTK_WIDGET(messageDialog), adw_alert_dialog_get_response_label(messageDialog, response));
    require(button && gtk_widget_is_sensitive(button), "Dialog response is unavailable");
    g_signal_emit_by_name(button, "clicked");
}

bool Prototype::cancelPrivateChooser() {
    auto* windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
        auto* candidate = GTK_WINDOW(g_list_model_get_item(windows, i));
        const bool chooser = candidate != GTK_WINDOW(window) &&
                             gtk_window_get_transient_for(candidate) == GTK_WINDOW(window) &&
                             gtk_window_get_modal(candidate);
        if (chooser) {
            gtk_window_close(candidate);
        }
        g_object_unref(candidate);
        if (chooser) {
            return true;
        }
    }
    return false;
}

gboolean Prototype::smokeDocuments() {
    const auto first = (std::filesystem::path(options.smokeDirectory) / "saved-copy.xopp").string();
    const auto second = (std::filesystem::path(options.smokeDirectory) / "saved-again.xopp").string();
    switch (documentSmokeStep++) {
        case 0:
            require(modified() && nativeSession, "Native document or dirty state missing");
            capture("document-dirty");
            saveCopyTo(first);
            require(busy && !gtk_widget_is_sensitive(canvas), "Saving did not block concurrent editing");
            gtk_window_close(GTK_WINDOW(window));
            require(!closed, "Window closed during an active save");
            break;
        case 1:
            require(!modified() && std::filesystem::is_regular_file(first), "Save did not commit clean state");
            require(state.ink.size() == 3 && state.canUndo(), "Saving discarded editable ink or undo history");
            capture("document-saved");
            state.undo();
            require(modified(), "Undo away from saved state must be dirty");
            protectDocument([this] { smokeContinued = true; });
            require(messageDialog, "Unsaved changes did not require a response");
            break;
        case 2:
            capture("unsaved");
            answerDialog("cancel");
            require(!smokeContinued && !messageDialog && state.ink.size() == 2,
                    "Cancel discarded changes or continued the action");
            openDocument(first + ".missing");
            break;
        case 3:
            require(ioErrors == 1 && messageDialog && state.ink.size() == 2 && modified(),
                    "Failed open lost the current document or changes");
            capture("open-error");
            answerDialog("close");
            saveCopyTo(first, [this] { smokeContinued = true; });
            break;
        case 4:
            require(ioErrors == 2 && messageDialog && modified() && !smokeContinued,
                    "Failed save cleared changes or ran its continuation");
            answerDialog("close");
            state.redo();
            require(!modified(), "Redo back to saved revision must be clean");
            openDocument(first);
            break;
        case 5: {
            require(nativeSession && sourcePath == first && state.ink.size() == 3 && !modified() && !state.canUndo() &&
                            !state.canRedo() && pageCount == 2,
                    "Reopen did not hydrate native strokes at clean revision 0");
            state.erase(state.offsetX + 80 * state.zoom, state.offsetY + 80 * state.zoom);
            require(modified() && state.canUndo() && state.ink.size() != 3,
                    "Erasing a preloaded native stroke did not mark the document dirty");
            state.undo();
            require(!modified() && !state.canUndo() && state.ink.size() == 3,
                    "Undoing native stroke erase did not restore clean revision 0");
            capture("reopened");
            const auto x = state.offsetX + 60 * state.zoom;
            const auto y = state.offsetY + 60 * state.zoom;
            input.begin(Device::Stylus, {x, y, 0.25});
            input.motion(Device::Stylus, {x + 40 * state.zoom, y + 20 * state.zoom, 0.75});
            input.finish(Device::Stylus);
            protectDocument([this] { smokeContinued = true; });
            require(messageDialog && state.ink.size() == 4 && state.ink.back().pressureSensitive,
                    "Pressure ink not retained for saving");
            break;
        }
        case 6:
            capture("save-prompt");
            answerDialog("save");
            require(busy && !smokeContinued, "Save continuation ran before completion");
            break;
        case 7:
            require(smokeContinued && !modified() && saveTarget == first && std::filesystem::is_regular_file(first),
                    "Normal Save from the prompt failed before continuing");
            require(!std::filesystem::exists(first + "~"), "Successful Save retained its temporary backup");
            smokeSaveChoice = second;
            chooseSave();
            require(busy, "Save As did not start");
            break;
        case 8:
            require(!modified() && saveTarget == second && sourcePath == second &&
                            std::filesystem::is_regular_file(second),
                    "Save As did not associate the new filename");
            capture("saved-again");
            state.undo();
            smokeContinued = false;
            protectDocument([this] { smokeContinued = true; });
            break;
        case 9:
            capture("discard-prompt");
            answerDialog("discard");
            require(smokeContinued && !messageDialog, "Discard did not continue the requested action");
            gtk_window_close(GTK_WINDOW(window));
            require(messageDialog && !closed, "Close did not protect unsaved changes");
            answerDialog("cancel");
            require(!closed && gtk_widget_get_visible(window), "Cancel did not keep the window open");
            break;
        case 10:
            chooseOpen();
            require(choosingFile, "Open action did not request a native file chooser");
            break;
        case 11:
            if (!cancelPrivateChooser()) {
                require(++chooserWaits < 60, "Native Open chooser did not appear");
                --documentSmokeStep;
            }
            break;
        case 12:
            require(!choosingFile && !messageDialog && sourcePath == second && modified(),
                    "Cancelling Open changed the current document");
            chooseSave();
            require(choosingFile, "Save action did not request a native file chooser");
            break;
        case 13:
            if (!cancelPrivateChooser()) {
                require(++chooserWaits < 60, "Native Save chooser did not appear");
                --documentSmokeStep;
            }
            break;
        case 14:
            require(!choosingFile && !messageDialog && modified() && !closed,
                    "Cancelling Save discarded changes or closed the document");
            break;
        case 15:
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "normal-save.xopp").string();
            chooseSave();
            break;
        case 16: {
            require(!modified() && saveTarget.ends_with("normal-save.xopp"), "New Save As failed");
            const auto x = state.offsetX + 140 * state.zoom;
            const auto y = state.offsetY + 160 * state.zoom;
            input.begin(Device::Mouse, {x, y, std::nullopt});
            input.finish(Device::Mouse);
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_s, 0, GDK_CONTROL_MASK, &handled);
            require(handled && busy, "Ctrl+S did not start normal Save");
            break;
        }
        case 17: {
            require(!modified() && !std::filesystem::exists(saveTarget + "~"),
                    "Normal Save did not complete or remove its temporary backup");
            capture("normal-saved");
            auto packets = prototype::bridge::transferInk(state.ink);
            auto texts = state.texts;
            auto images = prototype::bridge::transferImages(state.images);
            auto layers = state.pageLayers;
            auto pageSizes = currentPageSizes(state);
            packets.push_back({0, false, false, 2, {0, 1, 0}, {{200, 220, 1}, {230, 220, 1}}});
            const auto session = nativeSession;
            const auto worker = options.worker;
            const auto target = saveTarget;
            const auto before = saveVersion;
            runDocumentJob(
                    "Testing external edit…",
                    [session, worker, target, before, packets, texts, images, layers, pageSizes] {
                        session->save(worker, target, packets, texts, images, layers, pageSizes, before);
                    },
                    [] {});
            break;
        }
        case 18: {
            smokeExternalVersion = prototype::bridge::documentVersion(saveTarget).token;
            require(smokeExternalVersion != saveVersion, "External edit simulation did not change the native file");
            const auto x = state.offsetX + 170 * state.zoom;
            const auto y = state.offsetY + 190 * state.zoom;
            input.begin(Device::Mouse, {x, y, std::nullopt});
            input.finish(Device::Mouse);
            save();
            break;
        }
        case 19:
            require(ioErrors == 3 && messageDialog && modified() && state.ink.size() == 5 &&
                            prototype::bridge::documentVersion(saveTarget).token == smokeExternalVersion,
                    "A stale Save overwrote the external edit or lost pending session ink");
            capture("save-conflict");
            answerDialog("close");
            break;
        case 20:
            // Explicit Save As may replace a chosen current filename after
            // the native chooser's overwrite confirmation. Ctrl+S may not.
            smokeSaveChoice = saveTarget;
            chooseSave();
            break;
        case 21: {
            require(!modified() && prototype::bridge::documentVersion(saveTarget).token == saveVersion &&
                            saveVersion != smokeExternalVersion && !std::filesystem::exists(saveTarget + "~"),
                    "Explicit Save As did not resolve the selected filename conflict");
            capture("conflict-resolved");
            const auto x = state.offsetX + 210 * state.zoom;
            const auto y = state.offsetY + 230 * state.zoom;
            input.begin(Device::Mouse, {x, y, std::nullopt});
            input.finish(Device::Mouse);
            break;
        }
        case 22: {
            state.undo();
            require(!modified(), "Undo before autosave test must restore clean state");
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "autosave-note.xopp").string();
            chooseSave();
            break;
        }
        case 23: {
            require(!modified() && saveTarget.ends_with("autosave-note.xopp") && activeAutosavePath.empty(),
                    "Baseline autosave document was not saved cleanly");
            smokeAutosaveBaseVersion = saveVersion;
            const auto x = state.offsetX + 90 * state.zoom;
            const auto y = state.offsetY + 250 * state.zoom;
            begin(Device::Stylus, x, y, 0.3);
            motion(Device::Stylus, x + 55 * state.zoom, y + 18 * state.zoom, 0.8);
            finish(Device::Stylus, 0.8);
            // Keep a second stylus stroke actively in progress while background autosave runs.
            begin(Device::Stylus, x, y + 36 * state.zoom, 0.4);
            motion(Device::Stylus, x + 45 * state.zoom, y + 48 * state.zoom, 0.7);
            require(modified() && input.active() && state.current.has_value(),
                    "Active stylus stroke was not started before autosave");
            scheduleAutosave(1);
            autosaveWaits = 0;
            break;
        }
        case 24: {
            const auto autosaveSidecar = prototype::bridge::autosavePathForTarget(saveTarget, cacheRoot(),
                                                                                  static_cast<long long>(::getpid()))
                                                 .string();
            require(input.active() && state.current.has_value() && gtk_widget_is_sensitive(canvas),
                    "Background autosave interrupted an active stroke or disabled the canvas");
            if (autosaving || activeAutosavePath != autosaveSidecar || autosavedRevision != state.revision()) {
                require(++autosaveWaits < 60, "Periodic background autosave did not complete");
                --documentSmokeStep;
                break;
            }
            require(std::filesystem::is_regular_file(autosaveSidecar) &&
                            prototype::bridge::documentVersion(saveTarget).token == smokeAutosaveBaseVersion &&
                            !std::filesystem::exists(saveTarget + "~") &&
                            !std::filesystem::exists(autosaveSidecar + ".swap"),
                    "Autosave did not write sidecar cleanly or touched the primary saved document");
            finish(Device::Stylus, 0.7);
            require(!input.active() && state.ink.size() == 7, "Active stroke did not commit after autosave");
            autosaveWaits = 0;
            break;
        }
        case 25:
            if (autosaving || autosavedRevision != state.revision()) {
                require(++autosaveWaits < 60, "Updated periodic autosave did not complete");
                --documentSmokeStep;
                break;
            }
            break;  // Paint the updated "Unsaved changes · Autosaved" header before capturing.
        case 26: {
            const auto autosaveSidecar = activeAutosavePath;
            capture("autosaved");
            state.undo();
            state.undo();
            require(!modified() && std::filesystem::is_regular_file(autosaveSidecar),
                    "Expected clean state prior to autosave cleanup");
            startAutosave();
            require(activeAutosavePath.empty() && !std::filesystem::exists(autosaveSidecar),
                    "Autosave check did not remove redundant sidecar after undoing to clean state");
            state.redo();
            state.redo();
            autosaveWaits = 0;
            break;
        }
        case 27: {
            const auto autosaveDoc = saveTarget;
            const auto autosaveSidecar = prototype::bridge::autosavePathForTarget(autosaveDoc, cacheRoot(),
                                                                                  static_cast<long long>(::getpid()))
                                                 .string();
            if (autosaving || activeAutosavePath != autosaveSidecar || autosavedRevision != state.revision()) {
                require(++autosaveWaits < 60, "Autosave did not recreate sidecar after redo");
                --documentSmokeStep;
                break;
            }
            const auto discardDoc = (std::filesystem::path(options.smokeDirectory) / "discard-autosave.xopp").string();
            const auto discardSidecar = prototype::bridge::autosavePathForTarget(discardDoc, cacheRoot(),
                                                                                 static_cast<long long>(::getpid()))
                                                .string();
            const auto recoverDoc = (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
            const auto recoverSidecar = prototype::bridge::autosavePathForTarget(recoverDoc, cacheRoot(),
                                                                                 static_cast<long long>(::getpid()))
                                                .string();
            std::filesystem::copy_file(autosaveDoc, discardDoc, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::copy_file(autosaveSidecar, discardSidecar,
                                       std::filesystem::copy_options::overwrite_existing);
            std::filesystem::copy_file(autosaveDoc, recoverDoc, std::filesystem::copy_options::overwrite_existing);
            std::filesystem::copy_file(autosaveSidecar, recoverSidecar,
                                       std::filesystem::copy_options::overwrite_existing);
            const auto cacheProtected = prototype::bridge::autosavePathForTarget({}, cacheRoot(), 999999).string();
            std::filesystem::create_directories(std::filesystem::path(cacheProtected).parent_path());
            std::filesystem::copy_file(autosaveSidecar, cacheProtected,
                                       std::filesystem::copy_options::overwrite_existing);
            requestOpenDocument(cacheProtected);
            require(ioErrors == 4 && messageDialog && modified() && saveTarget == autosaveDoc,
                    "Direct open inside cache directory was not refused safely");
            std::filesystem::remove(cacheProtected);
            answerDialog("close");
            scheduleAutosave(static_cast<guint>(options.autosaveSeconds));
            save();
            break;
        }
        case 28: {
            const auto previousSidecar = prototype::bridge::autosavePathForTarget(saveTarget, cacheRoot(),
                                                                                  static_cast<long long>(::getpid()))
                                                 .string();
            require(!modified() && activeAutosavePath.empty() && !std::filesystem::exists(previousSidecar),
                    "Normal Save did not remove active autosave sidecar");
            const auto discardDoc = (std::filesystem::path(options.smokeDirectory) / "discard-autosave.xopp").string();
            requestOpenDocument(discardDoc);
            require(messageDialog, "Opening a document with a sidecar autosave did not prompt");
            answerDialog("discard");
            break;
        }
        case 29: {
            const auto discardDoc = (std::filesystem::path(options.smokeDirectory) / "discard-autosave.xopp").string();
            const auto discardSidecar = prototype::bridge::autosavePathForTarget(discardDoc, cacheRoot(),
                                                                                 static_cast<long long>(::getpid()))
                                                .string();
            require(!modified() && sourcePath == discardDoc && state.ink.size() == 5 &&
                            !std::filesystem::exists(discardSidecar),
                    "Discard Autosave did not remove the sidecar or opened dirty state");
            const auto recoverDoc = (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
            smokeAutosaveBaseVersion = prototype::bridge::documentVersion(recoverDoc).token;
            requestOpenDocument(recoverDoc);
            require(messageDialog, "Sidecar recovery prompt did not appear");
            break;
        }
        case 30:
            capture("autosave-prompt");
            answerDialog("recover");
            break;
        case 31: {
            const auto recoverDoc = (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
            const auto recoverSidecar = prototype::bridge::autosavePathForTarget(recoverDoc, cacheRoot(),
                                                                                 static_cast<long long>(::getpid()))
                                                .string();
            require(modified() && recoveredChanges && saveTarget == recoverDoc &&
                            activeAutosavePath == recoverSidecar && std::filesystem::is_regular_file(recoverSidecar) &&
                            prototype::bridge::documentVersion(recoverDoc).token == smokeAutosaveBaseVersion,
                    "Recovered document did not stay dirty, keep its sidecar, or leave saved file untouched");
            const auto x = state.offsetX + 120 * state.zoom;
            const auto y = state.offsetY + 260 * state.zoom;
            input.begin(Device::Mouse, {x, y, std::nullopt});
            input.finish(Device::Mouse);
            state.undo();
            require(modified(), "Undoing back to revision 0 after recovery falsely marked document clean");
            capture("autosave-recovered");
            save();
            break;
        }
        case 32: {
            const auto recoverDoc = (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
            const auto recoverSidecar = prototype::bridge::autosavePathForTarget(recoverDoc, cacheRoot(),
                                                                                 static_cast<long long>(::getpid()))
                                                .string();
            require(!modified() && !recoveredChanges && activeAutosavePath.empty() &&
                            !std::filesystem::exists(recoverSidecar) &&
                            prototype::bridge::documentVersion(recoverDoc).token != smokeAutosaveBaseVersion,
                    "Saving a recovered document did not update target or clean up its sidecar autosave");
            capture("autosave-saved");
            const auto x = state.offsetX + 80 * state.zoom;
            const auto y = state.offsetY + 360 * state.zoom;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
            begin(Device::Stylus, x, y, 0.3);
            motion(Device::Stylus, x + 160 * state.zoom, y, 0.9);
            finish(Device::Stylus, 0.9);
            // Split the stylus stroke with Standard eraser and add a Whiteout stroke, then save natively.
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 2u);
            begin(Device::Mouse, x + 80 * state.zoom, y);
            finish(Device::Mouse);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 1u);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
            begin(Device::Mouse, x + 30 * state.zoom, y - 25 * state.zoom);
            motion(Device::Mouse, x + 30 * state.zoom, y + 25 * state.zoom);
            finish(Device::Mouse);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            require(state.ink.size() == 10 && state.ink.back().brush.tool == Tool::Eraser,
                    "Expected two split pen segments and one whiteout stroke before native save");
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "eraser-modes.xopp").string();
            chooseSave();
            break;
        }
        case 33: {
            require(!modified() && saveTarget.ends_with("eraser-modes.xopp") &&
                            std::filesystem::is_regular_file(saveTarget),
                    "Saving split ink and whiteout stroke failed");
            const auto nativeEdited = (std::filesystem::path(options.smokeDirectory) / "native-edited.xopp").string();
            std::filesystem::copy_file(saveTarget, nativeEdited, std::filesystem::copy_options::overwrite_existing);
            openDocument(nativeEdited);
            break;
        }
        case 34: {
            const auto nativeEdited = (std::filesystem::path(options.smokeDirectory) / "native-edited.xopp").string();
            require(!modified() && sourcePath == nativeEdited && saveTarget == nativeEdited && state.ink.size() == 10 &&
                            state.ink.back().brush.tool == Tool::Eraser && !state.canUndo() && !state.canRedo(),
                    "Reopening native document did not load all 10 editable top-layer strokes");
            capture("native-loaded");
            const auto y = state.offsetY + 360 * state.zoom;
            // 1. Partially split a pre-existing native pressure-sensitive pen stroke.
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 2u);
            begin(Device::Mouse, state.offsetX + 205 * state.zoom, y);
            finish(Device::Mouse);
            require(state.ink.size() == 11 && modified(), "Standard eraser did not split preloaded native pen stroke");
            // 2. Delete the pre-existing native Whiteout stroke and the left native pen segment.
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 2u);
            begin(Device::Mouse, state.offsetX + 110 * state.zoom, state.offsetY + 338 * state.zoom);
            finish(Device::Mouse);
            require(state.ink.size() == 10, "Delete Stroke did not remove preloaded native whiteout stroke");
            begin(Device::Mouse, state.offsetX + 135 * state.zoom, y);
            finish(Device::Mouse);
            require(state.ink.size() == 9, "Delete Stroke did not remove preloaded native pen segment");
            // 3. Verify full Undo back to clean revision 0 and Redo back to revision 3.
            state.undo();
            state.undo();
            state.undo();
            require(!modified() && state.ink.size() == 10 && !state.canUndo(),
                    "Undoing native stroke edits did not restore clean revision 0");
            state.redo();
            state.redo();
            state.redo();
            require(modified() && state.ink.size() == 9 && !state.canRedo(),
                    "Redoing native stroke edits did not restore edited state");
            break;
        }
        case 35: {
            capture("native-erased");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_s, 0, GDK_CONTROL_MASK, &handled);
            require(handled && busy, "Ctrl+S did not start Save for edited native strokes");
            break;
        }
        case 36: {
            require(!modified() && saveTarget.ends_with("native-edited.xopp") &&
                            !std::filesystem::exists(saveTarget + "~") && pageCount == 2,
                    "Ctrl+S did not persist edited native strokes cleanly");
            capture("native-saved");
            // Position the continuous viewport across the Page 1 / Page 2 boundary so Page 2 holds >50% area.
            const double viewW = gtk_widget_get_width(canvas);
            const double viewH = gtk_widget_get_height(canvas);
            fitMode = false;
            fitted = true;
            state.offsetY = viewH * 0.42 - state.pageTop(1) * state.zoom;
            state.syncActivePage(viewW, viewH);
            require(state.page == 1, "Continuous scroll did not promote Page 2 as active page");
            // Dragging inside the 15pt vertical inter-page gap must temporarily pan without drawing ink.
            const auto gapPt = state.pageScreenPoint(0, 120, state.pageSize(0).height + 7.5);
            const double oldOffsetY = state.offsetY;
            g_signal_emit_by_name(mouse, "drag-begin", gapPt.x, gapPt.y);
            require(input.panning(), "Dragging in the 15pt inter-page gap must activate the hand tool");
            g_signal_emit_by_name(mouse, "drag-update", 0.0, -12.0);
            g_signal_emit_by_name(mouse, "drag-end", 0.0, -12.0);
            require(state.ink.size() == 9 && std::abs(state.offsetY - (oldOffsetY - 12.0)) < 0.001,
                    "Inter-page gap pan unexpectedly drew ink or miscalculated offset");
            // Draw highlighter + pressure pen ink directly onto Page 2 without clicking the sidebar.
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[1]), true);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
            const auto hStart = state.pageScreenPoint(1, 70, 95);
            const auto hEnd = state.pageScreenPoint(1, 310, 95);
            begin(Device::Mouse, hStart.x, hStart.y);
            motion(Device::Mouse, hEnd.x, hEnd.y);
            finish(Device::Mouse);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
            const auto pStart = state.pageScreenPoint(1, 70, 135);
            const auto pEnd = state.pageScreenPoint(1, 290, 135);
            begin(Device::Stylus, pStart.x, pStart.y, 0.35);
            motion(Device::Stylus, pEnd.x, pEnd.y, 0.90);
            finish(Device::Stylus, 0.90);
            require(modified() && state.ink.size() == 11 && state.ink[9].page == 1 && state.ink[10].page == 1,
                    "Direct drawing on Page 2 in continuous view did not target Page 2");
            break;
        }
        case 37: {
            capture("continuous-pages");
            // Cross-page scrolling must reuse the bounded 4-page LRU raster cache without rerasterizing Poppler.
            const size_t rastersBeforeScroll = canvasRenderer.completedRequests();
            const double zoomBeforeShortcuts = state.zoom;
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Home, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && state.page == 0 && std::abs(state.zoom - zoomBeforeShortcuts) < 1e-9,
                    "Ctrl+Home failed or reset zoom");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_J, 0u, GDK_SHIFT_MASK, &handled);
            require(handled && state.page == 1 && std::abs(state.zoom - zoomBeforeShortcuts) < 1e-9,
                    "Shift+J next-page shortcut failed or reset zoom");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_K, 0u, GDK_SHIFT_MASK, &handled);
            require(handled && state.page == 0, "Shift+K previous-page shortcut failed");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_End, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && state.page == 1, "Ctrl+End last-page shortcut failed");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_K, 0u, GDK_SHIFT_MASK, &handled);
            require(handled && state.page == 0 && canvasRenderer.completedRequests() == rastersBeforeScroll,
                    "Page navigation across cached pages unnecessarily rerasterized PDF");
            // Exercise ':' and Ctrl+G Go-to-Page dialogs.
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_colon, 0u, GdkModifierType(0), &handled);
            require(handled && messageDialog && gotoPageSpin, "Colon shortcut did not open Go-to-Page dialog");
            answerDialog("cancel");
            require(!messageDialog && state.page == 0, "Cancelling Go-to-Page dialog changed active page");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_g, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && messageDialog && gotoPageSpin, "Ctrl+G did not open Go-to-Page dialog");
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(gotoPageSpin), 2);
            break;
        }
        case 38:
            break;  // Let AdwAlertDialog open animation finish before capturing goto-page-dialog.
        case 39: {
            capture("goto-page-dialog");
            answerDialog("go");
            require(!messageDialog && state.page == 1, "Go-to-Page dialog did not jump to Page 2");
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_d, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && busy, "Ctrl+D did not start Insert Page After");
            break;
        }
        case 40: {
            require(pageCount == 3 && state.page == 2 && modified() && gtk_widget_get_sensitive(deletePageButton),
                    "Insert Page After did not create Page 3 or mark document modified");
            const auto p3Start = state.pageScreenPoint(2, 80, 90);
            const auto p3End = state.pageScreenPoint(2, 260, 90);
            begin(Device::Mouse, p3Start.x, p3Start.y);
            motion(Device::Mouse, p3End.x, p3End.y);
            finish(Device::Mouse);
            require(state.ink.size() == 12 && state.ink.back().page == 2,
                    "Drawing on newly inserted Page 3 did not record page index 2");
            // Frame the boundary between Page 2 and newly inserted lined Page 3.
            const double viewW = gtk_widget_get_width(canvas);
            const double viewH = gtk_widget_get_height(canvas);
            fitMode = false;
            fitted = true;
            state.offsetY = viewH * 0.36 - state.pageTop(2) * state.zoom;
            state.syncActivePage(viewW, viewH);
            break;
        }
        case 41: {
            capture("page-inserted");
            // Export as PDF (Ctrl+E) must include unsaved strokes and newly inserted Page 3 without clearing dirty
            // state.
            smokeExportChoice = (std::filesystem::path(options.smokeDirectory) / "exported-notes.pdf").string();
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_e, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && busy, "Ctrl+E did not start PDF export");
            break;
        }
        case 42: {
            const auto exportedPdf = (std::filesystem::path(options.smokeDirectory) / "exported-notes.pdf").string();
            require(std::filesystem::is_regular_file(exportedPdf) && modified(),
                    "PDF export failed or falsely cleared .xopp unsaved changes");
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "multipage-edited.xopp").string();
            chooseSave();
            break;
        }
        case 43: {
            require(!modified() && saveTarget.ends_with("multipage-edited.xopp") && pageCount == 3,
                    "Saving 3-page document failed");
            navigateToPage(1);
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Delete, 0u,
                                  GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
            require(handled && busy, "Ctrl+Shift+Delete did not start Delete Current Page");
            break;
        }
        case 44: {
            // Deleting old Page 2 (index 1, 2 strokes) shifts old Page 3 (1 stroke) down to index 1 -> 10 strokes
            // total.
            require(pageCount == 2 && modified() && state.ink.size() == 10 && state.ink.back().page == 1,
                    "Deleting Page 2 did not remove its strokes or shift Page 3 strokes to index 1");
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "after-delete-page.xopp").string();
            chooseSave();
            break;
        }
        case 45: {
            require(!modified() && saveTarget.ends_with("after-delete-page.xopp") && pageCount == 2,
                    "Saving document after deleting Page 2 failed");
            const auto p0 = state.pageScreenPoint(state.page, 210, 230);
            input.begin(Device::Mouse, {p0.x, p0.y, std::nullopt});
            input.finish(Device::Mouse);
            break;
        }
        case 46: {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            navigateToPage(0);
            const auto a = state.pageScreenPoint(0, 300, 400);
            const auto b = state.pageScreenPoint(0, 350, 420);
            input.begin(Device::Stylus, {a.x, a.y, .25});
            input.motion(Device::Stylus, {b.x, b.y, .75});
            input.finish(Device::Stylus);
            smokeSelectionIndex = state.ink.size() - 1;
            const auto originalRevision = state.revision();
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_R, 0u,
                                  GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
            require(handled && state.brush.tool == Tool::SelectRectangle, "Rectangle selection shortcut failed");
            g_signal_emit_by_name(keys, "key-released", GDK_KEY_Shift_L, 0u, GdkModifierType(0));
            const auto from = state.pageScreenPoint(0, 295, 395);
            const auto to = state.pageScreenPoint(0, 355, 425);
            g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
            g_signal_emit_by_name(mouse, "drag-update", to.x - from.x, to.y - from.y);
            g_signal_emit_by_name(mouse, "drag-end", to.x - from.x, to.y - from.y);
            require(state.selected == std::vector<size_t>{smokeSelectionIndex} && state.revision() == originalRevision,
                    "Rectangle selection changed ink or saved status");
            break;
        }
        case 47: {
            capture("selection");
            const auto from = state.pageScreenPoint(0, 320, 410);
            const auto to = state.pageScreenPoint(0, 340, 450);
            g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
            g_signal_emit_by_name(mouse, "drag-update", to.x - from.x, to.y - from.y);
            require(std::abs(state.selectionBox()->x0 - 320) < 1e-6 &&
                            std::abs(state.committedInk()[smokeSelectionIndex].points.front().x - 300) < 1e-6,
                    "Selection preview leaked into committed autosave state");
            g_signal_emit_by_name(mouse, "drag-end", to.x - from.x, to.y - from.y);
            require(std::abs(state.ink[smokeSelectionIndex].points.front().y - 440) < 1e-6,
                    "Selection drag did not apply page-coordinate translation");
            break;
        }
        case 48:
            capture("selection-moved");
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "selection-moved.xopp").string();
            chooseSave();
            break;
        case 49: {
            require(!modified() && !state.selected.empty(), "Saving discarded selection or failed");
            g_signal_emit_by_name(undoButton, "clicked");
            require(modified() && std::abs(state.ink[smokeSelectionIndex].points.front().x - 300) < 1e-6 &&
                            state.selected == std::vector<size_t>{smokeSelectionIndex},
                    "Move undo did not retain selection and restore coordinates");
            g_signal_emit_by_name(redoButton, "clicked");
            require(!modified() && std::abs(state.ink[smokeSelectionIndex].points.front().x - 320) < 1e-6,
                    "Move redo did not return to saved state");
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Delete, 0u, GdkModifierType(0), &handled);
            require(handled && state.ink.size() == smokeSelectionIndex && state.selected.empty(),
                    "Delete did not remove the selected stroke");
            break;
        }
        case 50:
            capture("selection-deleted");
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "selection-deleted.xopp").string();
            chooseSave();
            break;
        case 51: {
            require(!modified(), "Saving deleted native ink failed");
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.ink.size() == smokeSelectionIndex + 1 && state.selected.size() == 1 && modified(),
                    "Delete undo did not restore the selected native stroke");
            const auto from = state.pageScreenPoint(0, 340, 450);
            g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
            g_signal_emit_by_name(mouse, "drag-update", 30.0, 20.0);
            g_signal_emit_by_name(mouse, "cancel", nullptr);
            require(std::abs(state.ink[smokeSelectionIndex].points.front().x - 320) < 1e-6 && !state.selecting(),
                    "Selection cancellation retained transient coordinates");
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_a, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && !state.selected.empty(), "Select All failed");
            for (auto index: state.selected) {
                require(state.ink[index].page == state.page, "Select All crossed page boundaries");
            }
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Escape, 0u, GdkModifierType(0), &handled);
            require(state.selected.empty(), "Escape did not clear selection");

            verifyTextClipboard();
            std::cout << "PASS: rectangle selection, native stroke move/delete, saved-state undo/redo, "
                         "selection cancellation, Select All and Escape\n";
            break;
        }
        case 52: {
            activateSelection(Tool::SmartSelect);
            state.clearSelection();
            navigateToPage(0);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            const auto a = state.pageScreenPoint(0, 200, 450);
            const auto b = state.pageScreenPoint(0, 300, 470);
            const auto c = state.pageScreenPoint(0, 240, 530);
            input.begin(Device::Stylus, {a.x, a.y, .25});
            input.motion(Device::Stylus, {b.x, b.y, .5});
            input.motion(Device::Stylus, {c.x, c.y, .75});
            input.finish(Device::Stylus);
            smokeSelectionIndex = state.ink.size() - 1;
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_G, 0u,
                                  GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
            require(handled && state.brush.tool == Tool::SelectLasso, "Lasso shortcut failed");
            g_signal_emit_by_name(keys, "key-released", GDK_KEY_Shift_L, 0u, GdkModifierType(0));
            const auto from = state.pageScreenPoint(0, 190, 440);
            g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
            for (const auto p: {prototype::Point{310, 440}, prototype::Point{310, 540}, prototype::Point{190, 540},
                                prototype::Point{190, 440}}) {
                const auto to = state.pageScreenPoint(0, p.x, p.y);
                g_signal_emit_by_name(mouse, "drag-update", to.x - from.x, to.y - from.y);
            }
            g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
            require(state.ink[smokeSelectionIndex].page == 0 &&
                            state.selected == std::vector<size_t>{smokeSelectionIndex} && state.selectedTexts.empty(),
                    "Lasso did not isolate the native stroke for the stroke clipboard check");
            break;
        }
        case 53: {
            capture("lasso-selection");
            const auto handles = state.selectionHandles();
            const auto a = state.pageScreenPoint(0, handles[4].x, handles[4].y);
            const auto b = state.pageScreenPoint(0, handles[0].x + 1.5 * (handles[4].x - handles[0].x),
                                                 handles[0].y + 1.5 * (handles[4].y - handles[0].y));
            smokeTransformWidth = state.ink.back().brush.width;
            const bool oldSnapGridEnabled = state.snapGridEnabled;
            state.snapGridEnabled = false;
            g_signal_emit_by_name(mouse, "drag-begin", a.x, a.y);
            require(state.selected == std::vector<size_t>{smokeSelectionIndex} && state.selectedTexts.empty(),
                    "Resize handle press discarded the selected stroke");
            g_signal_emit_by_name(mouse, "drag-update", b.x - a.x, b.y - a.y);
            require(state.committedInk().back().brush.width == smokeTransformWidth,
                    "Resize preview leaked into autosave");
            g_signal_emit_by_name(mouse, "drag-end", b.x - a.x, b.y - a.y);
            state.snapGridEnabled = oldSnapGridEnabled;
            require(std::abs(state.ink.back().brush.width - 1.5 * smokeTransformWidth) < 1e-6,
                    "Resize lost native width scaling");
            break;
        }
        case 54: {
            capture("selection-resized");
            const auto box = state.selectionBox();
            const auto h = state.selectionHandles()[8];
            const auto cx = (box->x0 + box->x1) / 2, cy = (box->y0 + box->y1) / 2;
            const auto a = state.pageScreenPoint(0, h.x, h.y);
            const auto b = state.pageScreenPoint(0, cx - (h.y - cy), cy + (h.x - cx));
            g_signal_emit_by_name(mouse, "drag-begin", a.x, a.y);
            g_signal_emit_by_name(mouse, "drag-update", b.x - a.x, b.y - a.y);
            g_signal_emit_by_name(mouse, "drag-end", b.x - a.x, b.y - a.y);
            require(std::abs(state.ink.back().brush.width - 1.5 * smokeTransformWidth) < 1e-6,
                    "Rotation changed stroke width");
            break;
        }
        case 55: {
            capture("selection-rotated");
            smokeClipboardCount = state.ink.size();
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_c, 0u, GDK_CONTROL_MASK, &handled);
            require(handled, "Copy shortcut failed");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_v, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && busy, "Paste did not start async clipboard read");
            break;
        }
        case 56: {
            require(state.ink.size() == smokeClipboardCount + 1 && state.selected.size() == 1 &&
                            std::abs(state.ink.back().brush.width - 1.5 * smokeTransformWidth) < 1e-6,
                    "Clipboard paste lost native stroke or width");
            capture("selection-pasted");
            duplicateSelection();
            require(state.ink.size() == smokeClipboardCount + 2, "Duplicate selection failed");
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.ink.size() == smokeClipboardCount + 1, "Duplicate was not a single undo action");
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_x, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && state.ink.size() == smokeClipboardCount, "Cut did not remove clipboard selection");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_v, 0u, GDK_CONTROL_MASK, &handled);
            break;
        }
        case 57: {
            require(state.ink.size() == smokeClipboardCount + 1 && state.selected.size() == 1, "Cut/paste failed");
            const auto box = state.selectionBox();
            const auto a = state.pageScreenPoint(0, (box->x0 + box->x1) / 2, (box->y0 + box->y1) / 2);
            const auto b = state.pageScreenPoint(1, 180, 220);
            g_signal_emit_by_name(mouse, "drag-begin", a.x, a.y);
            g_signal_emit_by_name(mouse, "drag-update", b.x - a.x, b.y - a.y);
            require(state.displayPage(state.selected.front()) == 1 && state.committedInk().back().page == 0,
                    "Cross-page preview leaked into committed document");
            g_signal_emit_by_name(mouse, "drag-end", b.x - a.x, b.y - a.y);
            require(state.ink.back().page == 1, "Cross-page transfer did not reassign page");
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.ink.back().page == 0, "Cross-page undo failed");
            g_signal_emit_by_name(redoButton, "clicked");
            navigateToPage(1);
            break;
        }
        case 58:
            smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "selection-transforms.xopp").string();
            chooseSave();
            break;
        case 59: {
            require(!modified() && state.ink.back().page == 1, "Saving selection transforms failed");
            capture("selection-transferred");
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Escape, 0u, GdkModifierType(0), &handled);
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_s, 0u, GdkModifierType(0), &handled);
            require(handled && state.brush.tool == Tool::SmartSelect, "Smart Select shortcut failed");
            const auto p = state.ink.back().points.front();
            const auto from = state.pageScreenPoint(1, p.x, p.y);
            g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
            g_signal_emit_by_name(mouse, "drag-update", 15.0, 15.0);
            g_signal_emit_by_name(mouse, "drag-end", 15.0, 15.0);
            require(state.selected.size() == 1 && modified(), "Smart Select did not pick and move stroke");
            capture("smart-selection");
            std::cout << "PASS: lasso, native resize/rotate, real GDK Copy/Cut/Paste, duplicate, cross-page transfer "
                         "and Smart Select\n";
            break;
        }
        case 60: {
            smokeClipboardCount = state.ink.size();
            auto* bytes = g_bytes_new_static("malformed", 9);
            auto* provider = gdk_content_provider_new_for_bytes(prototype::clipboard::mime, bytes);
            require(gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas), provider),
                    "Test clipboard offer failed");
            g_object_unref(provider);
            g_bytes_unref(bytes);
            pasteSelection();
            break;
        }
        case 61:
            require(messageDialog && state.ink.size() == smokeClipboardCount && state.selected.size() == 1,
                    "Malformed clipboard changed selection or ink");
            answerDialog("close");
            std::cout << "PASS: malformed async clipboard rejected atomically\n";
            break;
        default:
            require(busyHeartbeats > 0, "The UI main loop did not run during document I/O");
            std::cout << "PASS: document actions, async open/save/export-pdf, continuous multi-page viewport, "
                         "Go-to-Page, Insert/Delete Page, saved revisions, native reopen, pressure transfer, "
                         "failed-open/save retention, Save/Discard/Cancel, close protection, native Open/Save "
                         "chooser cancellation, normal Save/Save As, external-edit conflict, background autosave, "
                         "cache-open guard, sidecar recovery\n"
                      << "busy_main_loop_ticks=" << busyHeartbeats << " autosave_main_loop_ticks=" << autosaveHeartbeats
                      << '\n';
            gtk_window_close(GTK_WINDOW(window));
            require(messageDialog, "Close prompt disappeared");
            answerDialog("discard");
            return G_SOURCE_REMOVE;
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

}  // namespace prototype::gtk4
