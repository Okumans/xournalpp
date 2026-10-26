#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::runDocumentJob(const char* status, std::function<void()> work, std::function<void()> success,
                               bool initial) {
    if (busy) {
        return;
    }
    resetInput(true);
    busy = true;
    busyText = status;
    gtk_widget_set_sensitive(contentView, false);
    updateTitle();
    auto* application = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
    g_application_hold(application);
    prototype::bridge::documentJob(
            std::move(work), [this, application, success = std::move(success), initial](const std::string& error) {
                busy = false;
                gtk_widget_set_sensitive(contentView, true);
                try {
                    if (!error.empty()) {
                        throw std::runtime_error(error);
                    }
                    success();
                } catch (const std::exception& exception) {
                    if (initial && !options.smokeDirectory.empty()) {
                        failed = true;
                        std::cerr << "Cannot load test document: " << exception.what() << '\n';
                        g_application_quit(application);
                    } else {
                        showError(exception.what());
                    }
                }
                lastDocumentCompletion = g_get_monotonic_time();
                if (!closed) {
                    update();
                }
                g_application_release(application);
            });
}

void Prototype::replaceDocument(const std::shared_ptr<prototype::bridge::DocumentSession>& session,
                                const std::string& source, const std::optional<RecoveryInfo>& recovery) {
    auto [candidate, pageSizes] = openPreviewPdf(session->previewPath().string());
    const int count = static_cast<int>(pageSizes.size());
    std::vector<PopplerDocument*> candidateLayers;
    std::vector<PopplerDocument*> candidateAudioLayers;
    auto [hydratedInk, hydratedImages] = [&] {
        try {
            candidateLayers = openLayerPreviewPdfs(session->layerPreviewPaths, count);
            candidateAudioLayers = openAudioLayerPreviewPdfs(session->audioLayerPreviewPaths, count);
            auto ink = prototype::bridge::sessionInk(session->initialInk);
            std::vector<prototype::CanvasImage> images;
            images.reserve(session->initialImages.size());
            for (const auto& packet: session->initialImages) {
                images.push_back(prototype::hydrateCanvasImage(prototype::bridge::canvasImage(packet)));
            }
            return std::pair{std::move(ink), std::move(images)};
        } catch (...) {
            g_object_unref(candidate);
            releaseLayerPreviewPdfs(candidateLayers);
            releaseLayerPreviewPdfs(candidateAudioLayers);
            throw;
        }
    }();
    // Commit the replacement only after the new native snapshot and preview
    // are valid. Failed opens leave the old document and undo history intact.
    if (!recovery || recovery->autosavePath != activeAutosavePath) {
        cleanupActiveAutosave();
    }
    input.reset();
    inkRasterCache.clear();
    imageGesture.reset();
    selectedImageIndex.reset();
    if (canvasPreloadSource) {
        g_source_remove(canvasPreloadSource);
        canvasPreloadSource = 0;
    }
    lastPreloadPage = -1;
    canvasPreloadedPages.clear();
    const auto brush = state.brush;
    const auto eraserMode = state.eraserMode;
    const auto eraserWidth = state.eraserWidth;
    state = CanvasState{};
    prototype::applyImportedSettingsToCanvas(state, importedConfig.settings);
    state.backgroundChangedCallback = [this](int page) { refreshNativeDocumentRevision(page); };
    state.historyChangedCallback = [this](const std::vector<std::string>& snapshots) {
        if (nativeSession) {
            nativeSession->prunePageHistorySnapshots(snapshots);
        }
    };
    state.setPages(std::move(pageSizes));
    state.selectActivePage(0);
    state.setLayerDocument(session->initialLayers);
    state.ink = std::move(hydratedInk);
    state.texts = session->initialTexts;
    state.images = std::move(hydratedImages);
    state.brush = brush;
    state.eraserMode = eraserMode;
    state.eraserWidth = eraserWidth;
    savedRevision = recovery ? std::optional<size_t>{} : std::optional<size_t>{0};
    autosavedRevision = 0;
    recoveredChanges = bool(recovery);
    activeAutosavePath = recovery ? recovery->autosavePath : std::string{};
    savedCopyPath.clear();
    exportedPdfPath.clear();
    savedNormally = false;
    g_clear_object(&document);
    document = candidate;
    releaseLayerPreviewPdfs(layerDocuments);
    layerDocuments = std::move(candidateLayers);
    releaseLayerPreviewPdfs(audioLayerDocuments);
    audioLayerDocuments = std::move(candidateAudioLayers);
    audioTargets = session->initialAudioTargets;
    pageCount = count;
    nativeSession = session;
    sourcePath = source;
    saveTarget = recovery ? recovery->saveTarget : session->originalSavePath;
    saveVersion = recovery ? recovery->saveVersion : session->originalVersion;
    canvasRenderer.invalidate();
    thumbnailRenderer.invalidate();
    thumbnailWidgets.clear();
    pageList = nullptr;
    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
    fitMode = true;
    fitted = false;
    if (canvas) {
        update();
    }
}

void Prototype::openDocument(const std::string& path, bool initial) {
    if (autosaving) {
        waitForAutosave([this, path, initial] { openDocument(path, initial); });
        return;
    }
    auto loaded = std::make_shared<std::shared_ptr<prototype::bridge::DocumentSession>>();
    const auto worker = options.worker;
    const auto audioFolder = importedConfig.settings.audioFolder;
    runDocumentJob(
            "Opening document…",
            [loaded, worker, path, audioFolder] {
                *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, path, audioFolder);
            },
            [this, loaded, path, initial] {
                replaceDocument(*loaded, path);
                if (initial && path.empty() && !checkedStartupRecovery) {
                    checkedStartupRecovery = true;
                    checkStartupRecovery();
                }
            },
            initial);
}

void Prototype::recoverSidecarAutosave(const std::string& path, const std::string& sidecar, bool initial) {
    auto loaded = std::make_shared<std::shared_ptr<prototype::bridge::DocumentSession>>();
    auto targetSave = std::make_shared<std::string>();
    auto targetVersion = std::make_shared<std::string>();
    const auto worker = options.worker;
    const auto audioFolder = importedConfig.settings.audioFolder;
    runDocumentJob(
            "Recovering autosave…",
            [loaded, targetSave, targetVersion, worker, path, sidecar, audioFolder] {
                *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, sidecar, audioFolder);
                std::error_code statusError;
                if (g_ascii_strcasecmp(std::filesystem::path(path).extension().string().c_str(), ".xopp") == 0 &&
                    std::filesystem::is_regular_file(std::filesystem::symlink_status(path, statusError)) &&
                    !statusError) {
                    *targetSave = path;
                    *targetVersion = prototype::bridge::documentVersion(path).token;
                }
            },
            [this, loaded, targetSave, targetVersion, path, sidecar] {
                replaceDocument(*loaded, path, RecoveryInfo{*targetSave, *targetVersion, sidecar});
            },
            initial);
}

void Prototype::promptSidecarRecovery(const std::string& path, const std::string& sidecar, bool initial) {
    const auto body = "An autosave file exists for \"" + std::filesystem::path(path).filename().string() +
                      "\". Recover unsaved changes from the autosave file, or discard it and open the saved "
                      "document?";
    messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Autosave file detected", body.c_str()));
    adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "discard", "Discard Autosave", "recover",
                                   "Recover", nullptr);
    adw_alert_dialog_set_response_appearance(messageDialog, "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(messageDialog, "recover", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(messageDialog, "cancel");
    adw_alert_dialog_set_default_response(messageDialog, "recover");
    struct SidecarPrompt {
        Prototype* self;
        std::string path;
        std::string sidecar;
        bool initial;
    };
    g_signal_connect_data(
            messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                auto* prompt = static_cast<SidecarPrompt*>(data);
                auto* self = prompt->self;
                self->messageDialog = nullptr;
                const std::string reply = response ? response : "";
                if (reply == "discard") {
                    prototype::bridge::removeAutosaveFiles(prompt->sidecar);
                    self->openDocument(prompt->path, prompt->initial);
                } else if (reply == "recover") {
                    self->recoverSidecarAutosave(prompt->path, prompt->sidecar, prompt->initial);
                } else if (prompt->initial && !self->nativeSession) {
                    self->openDocument("", true);
                }
            }),
            new SidecarPrompt{this, path, sidecar, initial},
            +[](gpointer data, GClosure*) { delete static_cast<SidecarPrompt*>(data); }, GConnectFlags(0));
    adw_dialog_present(ADW_DIALOG(messageDialog), window);
}

void Prototype::restoreStartupRecovery(const std::string& recoveryPath) {
    auto loaded = std::make_shared<std::shared_ptr<prototype::bridge::DocumentSession>>();
    const auto adopted =
            prototype::bridge::autosavePathForTarget({}, cacheRoot(), static_cast<long long>(::getpid())).string();
    const auto worker = options.worker;
    const auto audioFolder = importedConfig.settings.audioFolder;
    runDocumentJob(
            "Restoring recovery file…",
            [loaded, worker, recoveryPath, adopted, audioFolder] {
                *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, recoveryPath, audioFolder);
                (*loaded)->autosave(worker, adopted, (*loaded)->initialInk, (*loaded)->initialTexts,
                                    (*loaded)->initialImages, (*loaded)->initialLayers, (*loaded)->initialPageSizes);
                if (adopted != recoveryPath) {
                    prototype::bridge::removeAutosaveFiles(recoveryPath);
                }
            },
            [this, loaded, adopted] { replaceDocument(*loaded, "", RecoveryInfo{"", "", adopted}); });
}

void Prototype::checkStartupRecovery() {
    const auto found =
            prototype::bridge::findStartupRecoveryFile(configRoot(), cacheRoot(), static_cast<long long>(::getpid()));
    if (!found || busy || choosingFile || messageDialog) {
        return;
    }
    const auto recoveryPath = found->string();
    messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
            "Recovery file detected",
            "Xournal++ did not exit cleanly last time. Would you like to restore the unsaved recovery file?"));
    adw_alert_dialog_add_responses(messageDialog, "delete", "Delete file", "restore", "Restore file", nullptr);
    adw_alert_dialog_set_response_appearance(messageDialog, "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(messageDialog, "restore", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(messageDialog, "delete");
    adw_alert_dialog_set_default_response(messageDialog, "restore");
    struct RecoveryPrompt {
        Prototype* self;
        std::string path;
    };
    g_signal_connect_data(
            messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                auto* prompt = static_cast<RecoveryPrompt*>(data);
                auto* self = prompt->self;
                self->messageDialog = nullptr;
                const std::string reply = response ? response : "";
                if (reply == "delete") {
                    prototype::bridge::removeAutosaveFiles(prompt->path);
                } else if (reply == "restore") {
                    self->restoreStartupRecovery(prompt->path);
                }
            }),
            new RecoveryPrompt{this, recoveryPath},
            +[](gpointer data, GClosure*) { delete static_cast<RecoveryPrompt*>(data); }, GConnectFlags(0));
    adw_dialog_present(ADW_DIALOG(messageDialog), window);
}

void Prototype::requestOpenDocument(const std::string& path, bool initial) {
    if (busy || choosingFile || messageDialog) {
        return;
    }
    if (autosaving) {
        waitForAutosave([this, path, initial] { requestOpenDocument(path, initial); });
        return;
    }
    if (!path.empty() && prototype::bridge::isCacheProtectedPath(path, cacheRoot())) {
        showError("Do not open autosave files directly from the cache folder (" + (cacheRoot() / "xournalpp").string() +
                  "). Copy the file elsewhere or use automatic recovery on startup.");
        return;
    }
    if (!path.empty() && !prototype::bridge::isRecoveryArtifact(path)) {
        const auto extension = std::filesystem::path(path).extension().string();
        if (g_ascii_strcasecmp(extension.c_str(), ".xopp") == 0 || g_ascii_strcasecmp(extension.c_str(), ".xoj") == 0) {
            const auto sidecar =
                    prototype::bridge::autosavePathForTarget(path, cacheRoot(), static_cast<long long>(::getpid()));
            std::error_code statusError;
            if (sidecar.string() != activeAutosavePath &&
                std::filesystem::is_regular_file(std::filesystem::symlink_status(sidecar, statusError)) &&
                !statusError) {
                promptSidecarRecovery(path, sidecar.string(), initial);
                return;
            }
        }
    }
    openDocument(path, initial);
}

void Prototype::saveCopyTo(const std::string& path, std::function<void()> after) {
    if (!nativeSession || busy) {
        return;
    }
    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this, path, after = std::move(after)]() mutable { saveCopyTo(path, std::move(after)); });
        return;
    }
    resetInput(true);
    const auto revision = state.revision();
    const auto packets = prototype::bridge::transferInk(state.ink);
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    const auto session = nativeSession;
    const auto worker = options.worker;
    runDocumentJob(
            "Saving copy…",
            [session, worker, path, packets, texts, images, layers, pageSizes] {
                session->saveCopy(worker, path, packets, texts, images, layers, pageSizes);
            },
            [this, revision, path, after = std::move(after)] {
                savedRevision = revision;
                autosavedRevision = revision;
                recoveredChanges = false;
                savedCopyPath = path;
                cleanupActiveAutosave();
                if (after) {
                    after();
                }
            });
}

void Prototype::save(std::function<void()> after) {
    if (!nativeSession || busy || choosingFile || messageDialog) {
        return;
    }
    if (autosaving) {
        waitForAutosave([this, after = std::move(after)]() mutable { save(std::move(after)); });
        return;
    }
    if (saveTarget.empty()) {
        chooseSave(std::move(after));
    } else {
        saveTo(saveTarget, saveVersion, std::move(after));
    }
}

void Prototype::saveTo(const std::string& path, const std::string& expected, std::function<void()> after) {
    if (!nativeSession || busy) {
        return;
    }
    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this, path, expected, after = std::move(after)]() mutable {
            saveTo(path, expected, std::move(after));
        });
        return;
    }
    resetInput(true);
    const auto revision = state.revision();
    const auto packets = prototype::bridge::transferInk(state.ink);
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto version = std::make_shared<std::string>();
    runDocumentJob(
            "Saving…",
            [session, worker, path, expected, packets, texts, images, layers, pageSizes, version] {
                const auto before = expected.empty() ? prototype::bridge::documentVersion(path).token : expected;
                *version = session->save(worker, path, packets, texts, images, layers, pageSizes, before);
            },
            [this, path, revision, version, after = std::move(after)] {
                saveTarget = path;
                saveVersion = *version;
                sourcePath = path;
                savedNormally = true;
                savedCopyPath.clear();
                savedRevision = revision;
                autosavedRevision = revision;
                recoveredChanges = false;
                cleanupActiveAutosave();
                if (after) {
                    after();
                }
            });
}

void Prototype::chooseOpen() {
    if (busy || choosingFile || messageDialog) {
        return;
    }
    if (autosaving) {
        waitForAutosave([this] { chooseOpen(); });
        return;
    }
    choosingFile = true;
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open document");
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Xournal++ documents and PDFs");
    gtk_file_filter_add_pattern(filter, "*.xopp");
    gtk_file_filter_add_pattern(filter, "*.xoj");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_open(
            dialog, GTK_WINDOW(window), nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                self->choosingFile = false;
                GError* error = nullptr;
                auto* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
                self->finishFileChoice(file, error,
                                       [self](const std::string& path) { self->requestOpenDocument(path); });
            },
            this);
    g_object_unref(dialog);
}

void Prototype::chooseSave(std::function<void()> after) {
    if (busy || choosingFile || messageDialog || !nativeSession) {
        return;
    }
    if (autosaving) {
        waitForAutosave([this, after = std::move(after)]() mutable { chooseSave(std::move(after)); });
        return;
    }
    resetInput(true);
    if (!smokeSaveChoice.empty()) {
        const auto path = std::exchange(smokeSaveChoice, {});
        saveTo(path, {}, std::move(after));
        return;
    }
    choosingFile = true;
    struct Choice {
        Prototype* self;
        std::function<void()> after;
    };
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save As");
    const auto name = sourcePath.empty() ? std::string("Notebook.xopp") :
                                           std::filesystem::path(sourcePath).stem().string() + ".xopp";
    gtk_file_dialog_set_initial_name(dialog, name.c_str());
    gtk_file_dialog_save(
            dialog, GTK_WINDOW(window), nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Choice> choice(static_cast<Choice*>(data));
                auto* self = choice->self;
                self->choosingFile = false;
                GError* error = nullptr;
                auto* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
                self->finishFileChoice(file, error, [self, after = std::move(choice->after)](const std::string& path) {
                    self->saveTo(path, {}, after);
                });
            },
            new Choice{this, std::move(after)});
    g_object_unref(dialog);
}

void Prototype::finishFileChoice(GFile* file, GError* error, const std::function<void(const std::string&)>& chosen) {
    if (file) {
        auto* path = g_file_get_path(file);
        g_object_unref(file);
        if (path) {
            const std::string localPath = path;
            g_free(path);
            chosen(localPath);
        } else {
            showError("Choose a local file for this document.");
        }
    } else if (error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
               !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
               !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        showError(error->message);
    }
    g_clear_error(&error);
}

void Prototype::protectDocument(std::function<void()> after) {
    if (busy || choosingFile || messageDialog) {
        return;
    }
    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this, after = std::move(after)]() mutable { protectDocument(std::move(after)); });
        return;
    }
    resetInput(true);
    if (!modified()) {
        after();
        return;
    }
    afterConfirmation = std::move(after);
    messageDialog = ADW_ALERT_DIALOG(
            adw_alert_dialog_new("Save changes?", "This document has unsaved changes. Save them before continuing?"));
    adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "discard", "Discard", "save",
                                   saveTarget.empty() ? "Save As…" : "Save", nullptr);
    adw_alert_dialog_set_response_enabled(messageDialog, "save", bool(nativeSession));
    adw_alert_dialog_set_response_appearance(messageDialog, "discard", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance(messageDialog, "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(messageDialog, "cancel");
    adw_alert_dialog_set_default_response(messageDialog, "save");
    g_signal_connect(messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->messageDialog = nullptr;
                         auto after = std::move(self->afterConfirmation);
                         if (std::string(response) == "discard") {
                             self->cleanupActiveAutosave();
                             after();
                         } else if (std::string(response) == "save") {
                             self->save(std::move(after));
                         }
                     }),
                     this);
    adw_dialog_present(ADW_DIALOG(messageDialog), window);
}

void Prototype::scheduleCanvasPagePreload() {
    if (canvasPreloadSource) {
        g_source_remove(canvasPreloadSource);
        canvasPreloadSource = 0;
    }
    if (state.page < 0 || state.page >= pageCount) {
        return;
    }
    if (lastPreloadPage != state.page) {
        if (lastPreloadPage >= 0) {
            canvasPreloadDirection = state.page > lastPreloadPage ? 1 : -1;
        }
        lastPreloadPage = state.page;
    }
    canvasPreloadedPages.clear();
    canvasPreloadSource = g_timeout_add(
            150,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                self->canvasPreloadSource = 0;
                const int page = self->state.page;
                if (page >= 0 && page < self->pageCount) {
                    const auto& settings = self->importedConfig.settings;
                    const int before = std::min<int>(settings.preloadPagesBefore, page);
                    const int after = std::min<int>(settings.preloadPagesAfter, self->pageCount - page - 1);
                    const int lower = page - before;
                    const int upper = page + after + 1;
                    double priority = 0;
                    const auto addBefore = [&] {
                        for (int adjacent = page; adjacent > lower;) {
                            self->canvasPreloadedPages.emplace(--adjacent, priority++);
                        }
                    };
                    const auto addAfter = [&] {
                        for (int adjacent = page + 1; adjacent < upper; ++adjacent) {
                            self->canvasPreloadedPages.emplace(adjacent, priority++);
                        }
                    };
                    if (self->canvasPreloadDirection < 0) {
                        addBefore();
                        addAfter();
                    } else {
                        addAfter();
                        addBefore();
                    }
                    self->update();
                }
                return G_SOURCE_REMOVE;
            },
            this);
}

void Prototype::navigateToPage(int number) {
    if (number < 0 || number >= pageCount) {
        return;
    }
    const int previousPage = state.page;
    if (number != state.page) {
        selectedImageIndex.reset();
    }
    resetInput();
    if (canvas) {
        const int width = gtk_widget_get_width(canvas);
        const int height = gtk_widget_get_height(canvas);
        state.scrollToPage(number, width, height);
        fitMode = presentationMode;
        if (presentationMode && width > 0 && height > 0) {
            fitCurrentView(width, height);
            fitted = true;
            lastWidth = width;
            lastHeight = height;
        }
    } else {
        state.selectActivePage(number);
    }
    fitted = false;
    if (canvas) {
        update();
        if (number == previousPage) {
            scrollPageSidebarToCurrent();
        }
    }
}

void Prototype::openGotoPageDialog() {
    if (busy || choosingFile || messageDialog || pageCount < 1) {
        return;
    }
    resetInput(true);
    const auto body = "Enter a page number between 1 and " + std::to_string(pageCount) + ".";
    messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Go to page", body.c_str()));
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), gtk_label_new("Page:"));
    gotoPageSpin = gtk_spin_button_new_with_range(1, std::max(1, pageCount), 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(gotoPageSpin), true);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(gotoPageSpin), state.page + 1);
    gtk_editable_set_width_chars(GTK_EDITABLE(gotoPageSpin), 5);
    gtk_box_append(GTK_BOX(row), gotoPageSpin);
    adw_alert_dialog_set_extra_child(messageDialog, row);
    adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "go", "Go to Page", nullptr);
    adw_alert_dialog_set_response_appearance(messageDialog, "go", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(messageDialog, "cancel");
    adw_alert_dialog_set_default_response(messageDialog, "go");
    g_signal_connect(gotoPageSpin, "activate", G_CALLBACK(+[](GtkSpinButton*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->messageDialog) {
                             self->answerDialog("go");
                         }
                     }),
                     this);
    g_signal_connect(
            messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                const int target = self->gotoPageSpin ?
                                           gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(self->gotoPageSpin)) - 1 :
                                           -1;
                self->messageDialog = nullptr;
                self->gotoPageSpin = nullptr;
                if (response && std::string_view(response) == "go" && target >= 0 && target < self->pageCount) {
                    self->navigateToPage(target);
                }
                if (self->canvas) {
                    gtk_widget_grab_focus(self->canvas);
                }
            }),
            this);
    adw_dialog_present(ADW_DIALOG(messageDialog), window);
}

void Prototype::openLayerRenameDialog(int pageIndex, size_t layerId) {
    if (busy || choosingFile || messageDialog || !nativeSession || pageIndex < 0 ||
        static_cast<size_t>(pageIndex) >= state.pageLayers.size()) {
        return;
    }
    const auto& pageLayers = state.layersForPage(pageIndex);
    if (layerId > pageLayers.layers.size()) {
        return;
    }
    finishTextEditing();
    resetInput(true);

    std::string currentName;
    const bool background = layerId == 0;
    if (background) {
        currentName = pageLayers.backgroundName.value_or("Background");
    } else {
        currentName = pageLayers.layers[layerId - 1].name;
        if (currentName.empty()) {
            currentName = "Layer " + std::to_string(layerId);
        }
    }

    messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(background ? "Rename Background" : "Rename Layer",
                                                          "Change the name shown in the layer list."));
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(row), gtk_label_new("Name:"));
    layerRenameEntry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(layerRenameEntry), true);
    gtk_entry_set_max_length(GTK_ENTRY(layerRenameEntry), 4096);
    gtk_widget_set_hexpand(layerRenameEntry, true);
    gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), currentName.c_str());
    gtk_box_append(GTK_BOX(row), layerRenameEntry);
    adw_alert_dialog_set_extra_child(messageDialog, row);
    adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "rename", "Rename", nullptr);
    adw_alert_dialog_set_response_appearance(messageDialog, "rename", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(messageDialog, "cancel");
    adw_alert_dialog_set_default_response(messageDialog, "rename");
    layerRenamePage = pageIndex;
    layerRenameId = layerId;
    g_signal_connect(layerRenameEntry, "changed", G_CALLBACK(+[](GtkEditable* entry, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto* value = gtk_editable_get_text(entry);
                         adw_alert_dialog_set_response_enabled(self->messageDialog, "rename",
                                                               std::char_traits<char>::length(value) <= 4096);
                     }),
                     this);
    g_signal_connect(layerRenameEntry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->messageDialog) {
                             self->answerDialog("rename");
                         }
                     }),
                     this);
    g_signal_connect(messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const int targetPage = self->layerRenamePage;
                         const size_t targetLayer = self->layerRenameId;
                         const std::string newName =
                                 self->layerRenameEntry ? gtk_editable_get_text(GTK_EDITABLE(self->layerRenameEntry)) :
                                                          "";
                         self->messageDialog = nullptr;
                         self->layerRenameEntry = nullptr;
                         if (response && std::string_view(response) == "rename" &&
                             self->state.renameLayer(targetPage, targetLayer, newName)) {
                             self->rebuildLayerStack();
                             self->update();
                         }
                         if (self->canvas) {
                             gtk_widget_grab_focus(self->canvas);
                         }
                     }),
                     this);
    adw_dialog_present(ADW_DIALOG(messageDialog), window);
    gtk_widget_grab_focus(layerRenameEntry);
    gtk_editable_set_position(GTK_EDITABLE(layerRenameEntry), -1);
}

void Prototype::exportPdfTo(const std::string& path) {
    if (!nativeSession || busy) {
        return;
    }
    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this, path] { exportPdfTo(path); });
        return;
    }
    resetInput(true);
    const auto packets = prototype::bridge::transferInk(state.ink);
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    const auto session = nativeSession;
    const auto worker = options.worker;
    // Exporting to PDF never alters .xopp savedRevision, saveTarget, or active autosave artifacts.
    runDocumentJob(
            "Exporting PDF…",
            [session, worker, path, packets, texts, images, layers, pageSizes] {
                session->exportPdf(worker, path, packets, texts, images, layers, pageSizes);
            },
            [this, path] { exportedPdfPath = path; });
}

void Prototype::startPluginExport(prototype::DocumentExportOptions exportOptions) {
    if (!nativeSession || busy) {
        return;
    }
    const auto path = exportOptions.outputFile;
    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this, exportOptions = std::move(exportOptions)]() mutable {
            startPluginExport(std::move(exportOptions));
        });
        return;
    }
    resetInput(true);
    const auto packets = prototype::bridge::transferInk(state.ink);
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto extension = std::filesystem::path(path).extension().string();
    const auto status = extension == ".pdf" ? "Exporting PDF…" :
                        extension == ".svg" ? "Exporting SVG…" :
                                              "Exporting PNG…";
    runDocumentJob(
            status,
            [session, worker, exportOptions = std::move(exportOptions), packets, texts, images, layers, pageSizes] {
                session->exportDocument(worker, exportOptions, packets, texts, images, layers, pageSizes);
            },
            [this, path, extension] {
                if (extension == ".pdf") {
                    exportedPdfPath = path;
                }
            });
}

bool Prototype::exportDocumentFromPlugin(const prototype::DocumentExportOptions& exportOptions, std::string& error) {
    if (exportOptions.outputFile.empty()) {
        error = "Missing output file!";
        return false;
    }
    if (!nativeSession) {
        error = "Document export requires an open native document";
        return false;
    }
    if (closed || choosingFile || messageDialog || busy) {
        error = "A document operation is already in progress";
        return false;
    }

    const auto extension = std::filesystem::path(exportOptions.outputFile).extension().string();
    if (extension != ".pdf" && extension != ".svg" && extension != ".png") {
        return true;
    }

    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this, exportOptions] { startPluginExport(exportOptions); });
    } else {
        startPluginExport(exportOptions);
    }
    return true;
}

void Prototype::chooseExportPdf() {
    if (busy || choosingFile || messageDialog || !nativeSession) {
        return;
    }
    if (autosaving) {
        waitForAutosave([this] { chooseExportPdf(); });
        return;
    }
    resetInput(true);
    if (!smokeExportChoice.empty()) {
        const auto path = std::exchange(smokeExportChoice, {});
        exportPdfTo(path);
        return;
    }
    choosingFile = true;
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export as PDF");
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF documents");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    const auto name = sourcePath.empty() ? std::string("Notebook.pdf") :
                                           std::filesystem::path(sourcePath).stem().string() + ".pdf";
    gtk_file_dialog_set_initial_name(dialog, name.c_str());
    gtk_file_dialog_save(
            dialog, GTK_WINDOW(window), nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                self->choosingFile = false;
                GError* error = nullptr;
                auto* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
                self->finishFileChoice(file, error, [self](const std::string& path) { self->exportPdfTo(path); });
            },
            this);
    g_object_unref(dialog);
}

void Prototype::performHistoryStep(bool redo) {
    if (busy || choosingFile || messageDialog || closed) {
        return;
    }

    bool pageExistsAfter = false;
    int pageIndex = -1;
    std::string pageSnapshotPath;
    const bool hasPageStructureChange =
            redo ? state.nextRedoPageStructureChange(pageExistsAfter, pageIndex, pageSnapshotPath) :
                   state.nextUndoPageStructureChange(pageExistsAfter, pageIndex, pageSnapshotPath);
    if (!hasPageStructureChange) {
        selectedImageIndex.reset();
        input.cancel();
        if (redo) {
            state.redo();
        } else {
            state.undo();
        }
        update();
        return;
    }
    if (!nativeSession) {
        showError("Page history requires an open native document");
        return;
    }
    if (autosaving) {
        waitForAutosave([this, redo] { performHistoryStep(redo); });
        return;
    }

    selectedImageIndex.reset();
    input.cancel();
    resetInput(true);
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto pageSizes = currentPageSizes(state);
    auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    const bool shouldExistAfter = redo ? pageExistsAfter : !pageExistsAfter;
    runDocumentJob(
            redo ? "Redoing page change…" : "Undoing page change…",
            [session, worker, pageSizes, staged, pageIndex, shouldExistAfter, pageExistsAfter, pageSnapshotPath] {
                if (!shouldExistAfter) {
                    *staged = session->stageDeletePageAt(worker, pageIndex, pageSizes, false);
                } else if (!pageSnapshotPath.empty()) {
                    *staged = session->stageRestorePageAt(worker, pageIndex, pageSnapshotPath, pageSizes);
                } else {
                    *staged = session->stageInsertPageAfter(worker, pageIndex - 1, pageSizes);
                }
            },
            [this, session, staged, redo] {
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
                pageCount = static_cast<int>(sizes.size());
                if (redo) {
                    state.redo();
                } else {
                    state.undo();
                }
                state.setPages(std::move(sizes));
                session->prunePageHistorySnapshots(state.pageHistorySnapshotPaths());
                canvasRenderer.invalidate();
                thumbnailRenderer.invalidate();
                thumbnailWidgets.clear();
                pageList = nullptr;
                adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                navigateToPage(state.page);
            });
}

void Prototype::insertPageAfterCurrent() {
    if (!nativeSession || busy || choosingFile || messageDialog) {
        return;
    }
    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this] { insertPageAfterCurrent(); });
        return;
    }
    resetInput(true);
    const int afterPage = state.page;
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto pageSizes = currentPageSizes(state);
    auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    runDocumentJob(
            "Inserting page…",
            [session, worker, afterPage, pageSizes, staged] {
                *staged = session->stageInsertPageAfter(worker, afterPage, pageSizes);
            },
            [this, session, afterPage, staged] {
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
                    if (layerState.size() != sizes.size()) {
                        throw std::runtime_error("Inserted page layer metadata does not match the document");
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
                pageCount = static_cast<int>(sizes.size());
                state.insertPageAfter(afterPage, sizes.at(static_cast<size_t>(afterPage + 1)), false);
                state.setPages(std::move(sizes));
                state.pageLayers.at(static_cast<size_t>(afterPage + 1)) =
                        layerState.at(static_cast<size_t>(afterPage + 1));
                state.recordPageInsertion(afterPage + 1);
                session->prunePageHistorySnapshots(state.pageHistorySnapshotPaths());
                state.selectActivePage(afterPage + 1);
                canvasRenderer.invalidate();
                thumbnailRenderer.invalidate();
                thumbnailWidgets.clear();
                pageList = nullptr;
                adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                navigateToPage(afterPage + 1);
            });
}

bool Prototype::addEmptyPagesFromPlugin(bool force, std::string& error) {
    if (!nativeSession) {
        error = "Adding note pages requires an open native document";
        return false;
    }
    if (busy || choosingFile || messageDialog || closed) {
        error = "A document operation is already in progress";
        return false;
    }
    if (autosaving) {
        waitForAutosave([this, force] {
            std::string deferredError;
            if (!addEmptyPagesFromPlugin(force, deferredError)) {
                showError(deferredError);
            }
        });
        return true;
    }
    if (nativeSession->pdfPageMap.size() != static_cast<size_t>(pageCount) ||
        state.pages.size() != static_cast<size_t>(pageCount)) {
        error = "The loaded document page metadata is inconsistent";
        return false;
    }

    std::vector<int> afterPages;
    for (int page = pageCount - 1; page >= 0; --page) {
        const auto pageIndex = static_cast<size_t>(page);
        const bool currentHasPdfBackground = nativeSession->pdfPageMap[pageIndex] >= 0;
        const bool nextHasPdfBackground =
                page + 1 < pageCount && nativeSession->pdfPageMap[static_cast<size_t>(page + 1)] >= 0;
        if (force || (currentHasPdfBackground && (page + 1 == pageCount || nextHasPdfBackground))) {
            afterPages.push_back(page);
        }
    }

    if (afterPages.empty()) {
        navigateToPage(0);
        return true;
    }

    finishTextEditing();
    resetInput(true);
    const int finalPage = afterPages.back() == 0 ? 1 : 0;
    const auto sourcePageSizes = state.pages;
    const auto pageSizes = currentPageSizes(state);
    const auto session = nativeSession;
    const auto worker = options.worker;
    auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    runDocumentJob(
            "Adding note pages…",
            [session, worker, afterPages, pageSizes, staged] {
                *staged = session->stageInsertPagesAfter(worker, afterPages, pageSizes);
            },
            [this, session, afterPages, sourcePageSizes, finalPage, staged] {
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
                    if (layerState.size() != sizes.size()) {
                        throw std::runtime_error("Inserted page layer metadata does not match the document");
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
                pageCount = static_cast<int>(sizes.size());
                for (const int afterPage: afterPages) {
                    state.insertPageAfter(afterPage, sourcePageSizes.at(static_cast<size_t>(afterPage)), false);
                }
                state.setPages(std::move(sizes));
                for (const int afterPage: afterPages) {
                    const auto insertedPage = static_cast<size_t>(
                            afterPage + 1 +
                            std::count_if(afterPages.begin(), afterPages.end(),
                                          [afterPage](int pageBefore) { return pageBefore < afterPage; }));
                    state.pageLayers.at(insertedPage) = layerState.at(insertedPage);
                }
                for (const int afterPage: afterPages) {
                    const auto historyPage = afterPage + 1;
                    const auto finalPage = afterPage + 1 +
                                           static_cast<int>(std::count_if(
                                                   afterPages.begin(), afterPages.end(),
                                                   [afterPage](int pageBefore) { return pageBefore < afterPage; }));
                    state.recordPageInsertion(historyPage, finalPage);
                }
                session->prunePageHistorySnapshots(state.pageHistorySnapshotPaths());
                canvasRenderer.invalidate();
                thumbnailRenderer.invalidate();
                thumbnailWidgets.clear();
                pageList = nullptr;
                adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                navigateToPage(finalPage);
            });
    return true;
}

void Prototype::deleteCurrentPage() {
    if (!nativeSession || pageCount <= 1 || busy || choosingFile || messageDialog) {
        return;
    }
    finishTextEditing();
    if (autosaving) {
        waitForAutosave([this] { deleteCurrentPage(); });
        return;
    }
    resetInput(true);
    selectedImageIndex.reset();
    const int removeIndex = state.page;
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto pageSizes = currentPageSizes(state);
    auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
    runDocumentJob(
            "Deleting page…",
            [session, worker, removeIndex, pageSizes, staged] {
                *staged = session->stageDeletePageAt(worker, removeIndex, pageSizes);
            },
            [this, session, removeIndex, staged] {
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
                pageCount = static_cast<int>(sizes.size());
                if (!state.deletePageWithHistory(removeIndex, staged->pageHistorySnapshotPath.string())) {
                    throw std::runtime_error("The deleted page could not be added to undo history");
                }
                state.setPages(std::move(sizes));
                const int nextActive = std::min(removeIndex, pageCount - 1);
                state.selectActivePage(nextActive);
                session->prunePageHistorySnapshots(state.pageHistorySnapshotPaths());
                canvasRenderer.invalidate();
                thumbnailRenderer.invalidate();
                thumbnailWidgets.clear();
                pageList = nullptr;
                adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                navigateToPage(nextActive);
            });
}

}  // namespace prototype::gtk4
