#include "Prototype.h"

namespace prototype::gtk4 {

Prototype::Prototype(prototype::ApplicationOptions options):
        options(std::move(options)),
        searchController(*this),
        thumbnailRenderer(this->options.rasterExecutable),
        canvasRenderer(this->options.rasterExecutable, 4, 1, 4) {
    preferencesConfigDir = !this->options.configDirectory.empty() ?
                                   std::filesystem::path(this->options.configDirectory) :
                                   (configRoot() / "xournalpp");
    importedConfig = prototype::importSettingsFromConfigDir(preferencesConfigDir);
    if (!importedConfig.warning.empty()) {
        std::cerr << "xournalpp-gtk4-canvas: " << importedConfig.warning << '\n';
    }
    const auto preferenceLoad = prototype::loadGtk4Preferences(preferencesConfigDir, importedConfig.settings);
    if (!preferenceLoad.warning.empty()) {
        std::cerr << "xournalpp-gtk4-canvas: " << preferenceLoad.warning << '\n';
    }
    applyCanvasCacheLimits();
    presentationMode = importedConfig.settings.pageLayout.presentationMode;
    prototype::applyImportedSettingsToCanvas(state, importedConfig.settings);
    state.backgroundChangedCallback = [this](int page) { refreshNativeDocumentRevision(page); };
    state.historyChangedCallback = [this](const std::vector<std::string>& snapshots) {
        if (nativeSession) {
            nativeSession->prunePageHistorySnapshots(snapshots);
        }
    };
    input.setStabilizerConfig(importedConfig.settings.stabilizer);
    penBrush = {Tool::Pen, importedConfig.settings.penColor, importedConfig.settings.penWidth,
                importedConfig.settings.penFillEnabled, importedConfig.settings.penFillOpacity};
    penBrush.lineDashes =
            prototype::parseLineStyle(importedConfig.settings.penLineStyle).value_or(prototype::LineDashPattern{});
    latexBrush = {Tool::Latex, importedConfig.settings.latexColor, importedConfig.settings.penWidth};
    latexTemplatePath = importedConfig.settings.latexTemplatePath;
    if (latexTemplatePath.empty()) {
#ifdef GTK4_CANVAS_RESOURCE_DIR
        latexTemplatePath = std::filesystem::path(GTK4_CANVAS_RESOURCE_DIR) / "default_template.tex";
#endif
    }
    highlightBrush = {Tool::Highlighter, importedConfig.settings.highlighterColor,
                      importedConfig.settings.highlighterWidth, importedConfig.settings.highlighterFillEnabled,
                      importedConfig.settings.highlighterFillOpacity};
    laserPenBrush = {Tool::LaserPointerPen, importedConfig.settings.laserPenColor,
                     importedConfig.settings.laserPenWidth};
    laserHighlighterBrush = {Tool::LaserPointerHighlighter, importedConfig.settings.laserHighlighterColor,
                             importedConfig.settings.laserHighlighterWidth};
    eraserBrush = {Tool::Eraser, prototype::behavior::whiteoutColor, importedConfig.settings.eraserWidth};
    state.brush = importedConfig.settings.activeTool == Tool::Highlighter             ? highlightBrush :
                  importedConfig.settings.activeTool == Tool::Eraser                  ? eraserBrush :
                  importedConfig.settings.activeTool == Tool::LaserPointerPen         ? laserPenBrush :
                  importedConfig.settings.activeTool == Tool::LaserPointerHighlighter ? laserHighlighterBrush :
                  importedConfig.settings.activeTool == Tool::Latex                   ? latexBrush :
                                                                                        penBrush;
    effectiveAutosaveSeconds =
            this->options.autosaveSecondsOverride.value_or(importedConfig.settings.effectiveAutosaveSeconds());

    sourcePath = this->options.nativeDocument.empty() ? this->options.pdf : this->options.nativeDocument;
    if (!this->options.pdf.empty()) {
        auto [loadedDoc, sizes] = openPreviewPdf(this->options.pdf);
        document = loadedDoc;
        pageCount = static_cast<int>(sizes.size());
        state.setPages(std::move(sizes));
        state.selectActivePage(0);
    } else {
        state.setPages({{595, 842}});
        state.selectActivePage(0);
    }
    initializePlugins();
}

Prototype::~Prototype() {
    cancelScrollMomentum();
    for (const auto& operation: quickScreenshotOperations) {
        operation->self = nullptr;
        if (operation->process) {
            g_subprocess_force_exit(operation->process);
            g_clear_object(&operation->process);
        }
        removeQuickScreenshotDirectory(*operation);
    }
    quickScreenshotOperations.clear();
    for (guint* source: {&heartbeatSource, &autosaveSource, &crashHelperSource, &recoveryVerifySource,
                         &selectionEdgePanSource, &zoomPreviewSource, &canvasPreloadSource}) {
        if (*source) {
            g_source_remove(*source);
            *source = 0;
        }
    }
    if (laserFadeSource) {
        g_source_remove(laserFadeSource);
        laserFadeSource = 0;
    }
    if (floatingToolboxHideSource) {
        g_source_remove(floatingToolboxHideSource);
        floatingToolboxHideSource = 0;
    }
    if (floatingToolboxSmokeSource) {
        g_source_remove(floatingToolboxSmokeSource);
        floatingToolboxSmokeSource = 0;
    }
    if (floatingPageSpinTimeout) {
        g_source_remove(floatingPageSpinTimeout);
        floatingPageSpinTimeout = 0;
    }
    pendingFloatingPageSpin = nullptr;
    if (floatingLayerRefreshSource) {
        g_source_remove(floatingLayerRefreshSource);
        floatingLayerRefreshSource = 0;
    }
    if (floatingToolboxDismissSmokeSource) {
        g_source_remove(floatingToolboxDismissSmokeSource);
        floatingToolboxDismissSmokeSource = 0;
    }
    if (closed) {
        cleanupActiveAutosave();
    }
    if (pluginManagerWindow) {
        gtk_window_destroy(pluginManagerWindow);
        g_clear_object(&pluginManagerWindow);
    }
    preferencesDialog.clear();
    if (latexEditor) {
        auto editor = std::move(latexEditor);
        editor->cancel();
    }
    audioPlayback.reset();
    if (paintClock) {
        g_signal_handlers_disconnect_by_data(paintClock, this);
        g_clear_object(&paintClock);
    }
    g_clear_object(&windowPaintable);
    if (document) {
        g_object_unref(document);
    }
    g_clear_object(&pdfTextDocument);
    releaseLayerPreviewPdfs(layerDocuments);
    releaseLayerPreviewPdfs(audioLayerDocuments);
}

int Prototype::run() {
    auto* app = adw_application_new("com.github.xournalpp.Gtk4CanvasPrototype", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer data) {
                         static_cast<Prototype*>(data)->activate(app);
                     }),
                     this);
    const auto result = g_application_run(G_APPLICATION(app), 0, nullptr);
    g_object_unref(app);
    return failed ? 1 : result;
}

void Prototype::fail(GError* error) {
    const std::string message = error ? error->message : "Unknown PDF error";
    g_clear_error(&error);
    throw std::runtime_error(message);
}

std::pair<PopplerDocument*, std::vector<prototype::PageSize>> Prototype::openPreviewPdf(const std::string& path) {
    GError* error = nullptr;
    auto* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
    if (!uri) {
        fail(error);
    }
    auto* candidate = poppler_document_new_from_file(uri, nullptr, &error);
    g_free(uri);
    if (!candidate) {
        fail(error);
    }
    const int count = poppler_document_get_n_pages(candidate);
    if (count <= 0) {
        g_object_unref(candidate);
        throw std::runtime_error("Document has no pages");
    }
    std::vector<prototype::PageSize> sizes;
    sizes.reserve(static_cast<size_t>(count));
    try {
        for (int i = 0; i < count; ++i) {
            auto* pdfPage = poppler_document_get_page(candidate, i);
            if (!pdfPage) {
                throw std::runtime_error("Cannot open rendered PDF page");
            }
            double w = 595;
            double h = 842;
            poppler_page_get_size(pdfPage, &w, &h);
            g_object_unref(pdfPage);
            sizes.push_back({w, h});
        }
    } catch (...) {
        g_object_unref(candidate);
        throw;
    }
    return {candidate, std::move(sizes)};
}

std::vector<PopplerDocument*> Prototype::openLayerPreviewPdfs(const std::vector<std::filesystem::path>& paths,
                                                              int expectedPageCount) {
    std::vector<PopplerDocument*> documents;
    documents.reserve(paths.size());
    try {
        for (const auto& path: paths) {
            auto [document, sizes] = openPreviewPdf(path.string());
            if (static_cast<int>(sizes.size()) != expectedPageCount) {
                g_object_unref(document);
                throw std::runtime_error("A native layer preview has the wrong page count");
            }
            documents.push_back(document);
        }
    } catch (...) {
        for (auto* document: documents) {
            g_object_unref(document);
        }
        throw;
    }
    return documents;
}

std::vector<PopplerDocument*> Prototype::openAudioLayerPreviewPdfs(const std::vector<std::filesystem::path>& paths,
                                                                   int expectedPageCount) {
    return openLayerPreviewPdfs(paths, expectedPageCount);
}

void Prototype::releaseLayerPreviewPdfs(std::vector<PopplerDocument*>& documents) {
    for (auto* document: documents) {
        g_object_unref(document);
    }
    documents.clear();
}

bool Prototype::modified() const { return savedRevision != state.revision(); }

std::filesystem::path Prototype::cacheRoot() {
    const char* dir = g_get_user_cache_dir();
    return dir ? std::filesystem::path(dir) : std::filesystem::path{};
}

std::filesystem::path Prototype::configRoot() {
    const char* dir = g_get_user_config_dir();
    return dir ? std::filesystem::path(dir) : std::filesystem::path{};
}

std::string Prototype::currentAutosaveTarget() const {
    return prototype::bridge::autosavePathForTarget(saveTarget, cacheRoot(), static_cast<long long>(::getpid()))
            .string();
}

void Prototype::cleanupActiveAutosave() {
    if (!activeAutosavePath.empty()) {
        prototype::bridge::removeAutosaveFiles(activeAutosavePath);
        activeAutosavePath.clear();
    }
}

void Prototype::scheduleAutosave(guint seconds) {
    if (autosaveSource) {
        g_source_remove(autosaveSource);
        autosaveSource = 0;
    }
    if (seconds > 0) {
        autosaveSource = g_timeout_add_seconds(
                seconds,
                +[](gpointer data) -> gboolean {
                    static_cast<Prototype*>(data)->startAutosave();
                    return G_SOURCE_CONTINUE;
                },
                this);
    }
}

void Prototype::waitForAutosave(std::function<void()> action) {
    resetInput(true);
    gtk_widget_set_sensitive(contentView, false);
    deferredAction = std::move(action);
    updateTitle();
}

void Prototype::startAutosave() {
    if (!nativeSession || closed || busy || autosaving || choosingFile || messageDialog) {
        return;
    }
    finishTextEditing();
    if (!modified()) {
        if (!activeAutosavePath.empty()) {
            cleanupActiveAutosave();
            updateTitle();
        }
        autosavedRevision = state.revision();
        return;
    }
    const auto target = currentAutosaveTarget();
    std::error_code statusError;
    if (autosavedRevision == state.revision() && activeAutosavePath == target &&
        std::filesystem::is_regular_file(std::filesystem::symlink_status(target, statusError)) && !statusError) {
        return;
    }
    const auto revision = state.revision();
    const auto packets = prototype::bridge::transferInk(state.committedInk());
    const auto texts = state.texts;
    const auto images = prototype::bridge::transferImages(state.images);
    const auto layers = state.pageLayers;
    const auto pageSizes = currentPageSizes(state);
    const auto session = nativeSession;
    const auto worker = options.worker;
    const auto previous = activeAutosavePath;
    autosaving = true;
    updateTitle();
    auto* application = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
    g_application_hold(application);
    prototype::bridge::documentJob(
            [session, worker, target, packets, texts, images, layers, pageSizes] {
                session->autosave(worker, target, packets, texts, images, layers, pageSizes);
            },
            [this, application, session, revision, target, previous](const std::string& error) {
                autosaving = false;
                if (error.empty() && nativeSession == session && !closed) {
                    if (!previous.empty() && previous != target) {
                        prototype::bridge::removeAutosaveFiles(previous);
                    }
                    if (modified()) {
                        activeAutosavePath = target;
                        autosavedRevision = revision;
                    } else {
                        prototype::bridge::removeAutosaveFiles(target);
                        activeAutosavePath.clear();
                        autosavedRevision = state.revision();
                    }
                } else if (!error.empty()) {
                    ++autosaveErrors;
                }
                if (deferredAction) {
                    gtk_widget_set_sensitive(contentView, true);
                    auto next = std::move(deferredAction);
                    deferredAction = nullptr;
                    if (!closed) {
                        next();
                    }
                }
                if (!closed) {
                    update();
                }
                g_application_release(application);
            });
}

void Prototype::closeWindow() {
    cleanupActiveAutosave();
    closed = true;
    gtk_window_destroy(GTK_WINDOW(window));
}

void Prototype::updateTitle() {
    if (!heading) {
        return;
    }
    const auto name =
            sourcePath.empty() ? std::string("Blank notebook") : std::filesystem::path(sourcePath).filename().string();
    adw_window_title_set_title(ADW_WINDOW_TITLE(heading), (modified() ? "• " + name : name).c_str());
    std::string status;
    if (busy) {
        status = busyText;
    } else if (autosaving) {
        status = "Autosaving…";
    } else if (modified()) {
        if (recoveredChanges && state.revision() == 0) {
            status = "Recovered unsaved changes";
        } else if (autosavedRevision == state.revision() && !activeAutosavePath.empty()) {
            status = "Unsaved changes · Autosaved";
        } else {
            status = "Unsaved changes";
        }
    } else if (!savedCopyPath.empty()) {
        status = "Copy saved: " + std::filesystem::path(savedCopyPath).filename().string();
    } else if (savedNormally) {
        status = "Saved";
    } else if (nativeSession) {
        status = "Document loaded";
    } else {
        status = "Temporary notebook";
    }
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(heading), status.c_str());
}

void Prototype::showAlert(const char* title, const std::string& message) {
    ++ioErrors;
    messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, message.c_str()));
    adw_alert_dialog_add_response(messageDialog, "close", "Close");
    adw_alert_dialog_set_close_response(messageDialog, "close");
    g_signal_connect(messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
                         static_cast<Prototype*>(data)->messageDialog = nullptr;
                     }),
                     this);
    adw_dialog_present(ADW_DIALOG(messageDialog), window);
}

void Prototype::showError(const std::string& message) { showAlert("Document operation failed", message); }

void Prototype::activatePageLayoutAction(GSimpleAction* action, GVariant* parameter) {
    const std::string_view name = g_action_get_name(G_ACTION(action));
    if (name == "presentation-mode") {
        const bool enabled = parameter ? g_variant_get_boolean(parameter) : !presentationMode;
        setPresentationMode(enabled);
        g_simple_action_set_state(action, g_variant_new_boolean(presentationMode));
        return;
    }
    if (name == "fullscreen") {
        const bool current = presentationMode ? windowFullscreen : fullscreenModeActive;
        const bool enabled = parameter ? g_variant_get_boolean(parameter) : !current;
        setFullscreenMode(enabled);
        g_simple_action_set_state(action,
                                  g_variant_new_boolean(presentationMode ? windowFullscreen : fullscreenModeActive));
        return;
    }
    if (presentationMode) {
        return;
    }
    auto& settings = importedConfig.settings.pageLayout;
    bool handled = true;
    if (name == "paired-pages-mode") {
        settings.pairedPages = parameter ? g_variant_get_boolean(parameter) : !settings.pairedPages;
        g_simple_action_set_state(action, g_variant_new_boolean(settings.pairedPages));
    } else if (name == "set-layout-vertical") {
        settings.vertical = g_variant_get_boolean(parameter);
        g_simple_action_set_state(action, g_variant_new_boolean(settings.vertical));
    } else if (name == "set-layout-right-to-left") {
        settings.rightToLeft = g_variant_get_boolean(parameter);
        g_simple_action_set_state(action, g_variant_new_boolean(settings.rightToLeft));
    } else if (name == "set-layout-bottom-to-top") {
        settings.bottomToTop = g_variant_get_boolean(parameter);
        g_simple_action_set_state(action, g_variant_new_boolean(settings.bottomToTop));
    } else if (name == "set-columns-or-rows") {
        const int selection = g_variant_get_int32(parameter);
        if (selection > 0) {
            settings.columns = static_cast<std::size_t>(selection);
            settings.fixedRows = false;
        } else if (selection < 0) {
            settings.rows = static_cast<std::size_t>(-selection);
            settings.fixedRows = true;
        }
        const int current = settings.fixedRows ? -static_cast<int>(settings.rows) : static_cast<int>(settings.columns);
        g_simple_action_set_state(action, g_variant_new_int32(current));
    } else if (name == "paired-pages-offset") {
        settings.pairedPageOffset = std::clamp(g_variant_get_int32(parameter), 0, 100);
        g_simple_action_set_state(action, g_variant_new_int32(settings.pairedPageOffset));
    } else {
        handled = false;
    }
    if (!handled) {
        return;
    }
    const int currentPage = state.page;
    state.setLayoutSettings(settings);
    if (canvas) {
        state.scrollToPage(currentPage, gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
    }
    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        showAlert("Could not save preferences", error);
    }
    update();
}

const prototype::ViewModeSettings& Prototype::activeViewModeSettings() const {
    const auto preset = presentationMode     ? prototype::ViewModePreset::Presentation :
                        fullscreenModeActive ? prototype::ViewModePreset::Fullscreen :
                                               prototype::ViewModePreset::Default;
    return importedConfig.settings.viewModes[static_cast<std::size_t>(preset)];
}

void Prototype::setPageLayoutActionsEnabled(bool enabled) {
    for (const char* name: {"paired-pages-mode", "set-layout-vertical", "set-layout-right-to-left",
                            "set-layout-bottom-to-top", "set-columns-or-rows", "paired-pages-offset"}) {
        if (auto* action = g_action_map_lookup_action(G_ACTION_MAP(window), name);
            action && G_IS_SIMPLE_ACTION(action)) {
            g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
        }
    }
}

void Prototype::applyCurrentViewMode() {
    const auto& mode = activeViewModeSettings();
    if (headerBarWidget) {
        gtk_widget_set_visible(headerBarWidget, mode.showMenubar);
    }
    if (toolbarWidget) {
        gtk_widget_set_visible(toolbarWidget, mode.showToolbar);
    }
    if (mainToolRailWidget) {
        gtk_widget_set_visible(mainToolRailWidget, mode.showToolbar);
    }
    applyingViewMode = true;
    if (splitView) {
        adw_overlay_split_view_set_show_sidebar(splitView, mode.showSidebar);
    }
    applyingViewMode = false;

    const bool usePresetFullscreen = mode.goFullscreen;
    const bool targetFullscreen = presentationMode && presentationFullscreenOverride.has_value() ?
                                          *presentationFullscreenOverride :
                                          usePresetFullscreen;
    if (window && targetFullscreen != windowFullscreen) {
        if (targetFullscreen) {
            gtk_window_fullscreen(GTK_WINDOW(window));
        } else {
            gtk_window_unfullscreen(GTK_WINDOW(window));
        }
        windowFullscreen = targetFullscreen;
    }
    if (fitPageButton) {
        gtk_widget_set_sensitive(fitPageButton, !presentationMode);
    }
    for (auto* button: floatingZoomButtons) {
        gtk_widget_set_sensitive(button, !presentationMode);
    }
    setPageLayoutActionsEnabled(!presentationMode);

    if (auto* action = g_action_map_lookup_action(G_ACTION_MAP(window), "presentation-mode");
        action && G_IS_SIMPLE_ACTION(action)) {
        g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(presentationMode));
    }
    if (auto* action = g_action_map_lookup_action(G_ACTION_MAP(window), "fullscreen");
        action && G_IS_SIMPLE_ACTION(action)) {
        g_simple_action_set_state(G_SIMPLE_ACTION(action),
                                  g_variant_new_boolean(presentationMode ? windowFullscreen : fullscreenModeActive));
    }
}

void Prototype::fitCurrentView(double width, double height) {
    if (presentationMode) {
        state.fitPresentation(width, height);
        presentationFitPage = state.page;
    } else {
        state.fit(width, height);
        presentationFitPage = -1;
    }
}

void Prototype::setPresentationMode(bool enabled) {
    if (presentationMode == enabled) {
        return;
    }
    finishTextEditing();
    resetInput(true);
    presentationMode = enabled;
    fullscreenModeActive = false;
    presentationFullscreenOverride.reset();
    importedConfig.settings.pageLayout.presentationMode = enabled;
    state.setLayoutSettings(importedConfig.settings.pageLayout);
    applyCurrentViewMode();

    const int width = canvas ? gtk_widget_get_width(canvas) : 0;
    const int height = canvas ? gtk_widget_get_height(canvas) : 0;
    if (width > 0 && height > 0) {
        if (enabled) {
            fitCurrentView(width, height);
            fitMode = true;
        } else {
            state.scrollToPage(state.page, width, height);
            presentationFitPage = -1;
            fitMode = false;
        }
        fitted = true;
        lastWidth = width;
        lastHeight = height;
    } else {
        fitted = false;
        fitMode = enabled;
    }

    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        showAlert("Could not save Presentation Mode", error);
    }
    update();
}

void Prototype::setFullscreenMode(bool enabled) {
    if (presentationMode) {
        presentationFullscreenOverride = enabled;
    } else {
        fullscreenModeActive = enabled;
        presentationFullscreenOverride.reset();
    }
    applyCurrentViewMode();
    update();
}

void Prototype::syncPageLayoutActions() {
    const auto& settings = importedConfig.settings.pageLayout;
    const auto setState = [this](const char* name, GVariant* value) {
        auto* action = g_action_map_lookup_action(G_ACTION_MAP(window), name);
        if (action && G_IS_SIMPLE_ACTION(action)) {
            g_simple_action_set_state(G_SIMPLE_ACTION(action), value);
        }
    };
    setState("paired-pages-mode", g_variant_new_boolean(settings.pairedPages));
    setState("set-layout-vertical", g_variant_new_boolean(settings.vertical));
    setState("set-layout-right-to-left", g_variant_new_boolean(settings.rightToLeft));
    setState("set-layout-bottom-to-top", g_variant_new_boolean(settings.bottomToTop));
    const int gridState = settings.fixedRows ? -static_cast<int>(settings.rows) : static_cast<int>(settings.columns);
    setState("set-columns-or-rows", g_variant_new_int32(gridState));
    setState("paired-pages-offset", g_variant_new_int32(settings.pairedPageOffset));
}

void Prototype::buildPageLayoutMenu(GtkWidget* header) {
    const auto addAction = [this](const char* name, const GVariantType* parameterType, GVariant* initialState) {
        auto* action = g_simple_action_new_stateful(name, parameterType, initialState);
        g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction* action, GVariant* parameter, gpointer data) {
                             static_cast<Prototype*>(data)->activatePageLayoutAction(action, parameter);
                         }),
                         this);
        g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(action));
        g_object_unref(action);
    };
    const auto addTargetItem = [](GMenu* menu, const char* label, const char* action, GVariant* target) {
        auto* item = g_menu_item_new(label, action);
        g_menu_item_set_attribute_value(item, "target", target);
        g_menu_append_item(menu, item);
        g_object_unref(item);
    };

    const auto& settings = importedConfig.settings.pageLayout;
    addAction("paired-pages-mode", nullptr, g_variant_new_boolean(settings.pairedPages));
    addAction("presentation-mode", G_VARIANT_TYPE_BOOLEAN, g_variant_new_boolean(presentationMode));
    addAction("fullscreen", G_VARIANT_TYPE_BOOLEAN, g_variant_new_boolean(fullscreenModeActive));
    addAction("set-layout-vertical", G_VARIANT_TYPE_BOOLEAN, g_variant_new_boolean(settings.vertical));
    addAction("set-layout-right-to-left", G_VARIANT_TYPE_BOOLEAN, g_variant_new_boolean(settings.rightToLeft));
    addAction("set-layout-bottom-to-top", G_VARIANT_TYPE_BOOLEAN, g_variant_new_boolean(settings.bottomToTop));
    const int gridState = settings.fixedRows ? -static_cast<int>(settings.rows) : static_cast<int>(settings.columns);
    addAction("set-columns-or-rows", G_VARIANT_TYPE_INT32, g_variant_new_int32(gridState));
    addAction("paired-pages-offset", G_VARIANT_TYPE_INT32, g_variant_new_int32(settings.pairedPageOffset));

    auto* menu = g_menu_new();
    g_menu_append(menu, "Presentation Mode (F5)", "win.presentation-mode");
    g_menu_append(menu, "Fullscreen (F11)", "win.fullscreen");
    g_menu_append(menu, "Pair Pages", "win.paired-pages-mode");

    auto* orientation = g_menu_new();
    addTargetItem(orientation, "Horizontal", "win.set-layout-vertical", g_variant_new_boolean(false));
    addTargetItem(orientation, "Vertical", "win.set-layout-vertical", g_variant_new_boolean(true));
    g_menu_append_submenu(menu, "Page Flow", G_MENU_MODEL(orientation));
    g_object_unref(orientation);

    auto* horizontalDirection = g_menu_new();
    addTargetItem(horizontalDirection, "Left to Right", "win.set-layout-right-to-left", g_variant_new_boolean(false));
    addTargetItem(horizontalDirection, "Right to Left", "win.set-layout-right-to-left", g_variant_new_boolean(true));
    g_menu_append_submenu(menu, "Horizontal Reading", G_MENU_MODEL(horizontalDirection));
    g_object_unref(horizontalDirection);

    auto* verticalDirection = g_menu_new();
    addTargetItem(verticalDirection, "Top to Bottom", "win.set-layout-bottom-to-top", g_variant_new_boolean(false));
    addTargetItem(verticalDirection, "Bottom to Top", "win.set-layout-bottom-to-top", g_variant_new_boolean(true));
    g_menu_append_submenu(menu, "Vertical Reading", G_MENU_MODEL(verticalDirection));
    g_object_unref(verticalDirection);

    auto* grid = g_menu_new();
    auto* columns = g_menu_new();
    auto* rows = g_menu_new();
    for (int count = 1; count <= 8; ++count) {
        const auto columnLabel = std::to_string(count) + (count == 1 ? " Column" : " Columns");
        const auto rowLabel = std::to_string(count) + (count == 1 ? " Row" : " Rows");
        addTargetItem(columns, columnLabel.c_str(), "win.set-columns-or-rows", g_variant_new_int32(count));
        addTargetItem(rows, rowLabel.c_str(), "win.set-columns-or-rows", g_variant_new_int32(-count));
    }
    g_menu_append_submenu(grid, "Set Columns", G_MENU_MODEL(columns));
    g_menu_append_submenu(grid, "Set Rows", G_MENU_MODEL(rows));
    g_menu_append_submenu(menu, "Page Grid", G_MENU_MODEL(grid));
    g_object_unref(columns);
    g_object_unref(rows);
    g_object_unref(grid);

    auto* pairOffset = g_menu_new();
    addTargetItem(pairOffset, "0 pages", "win.paired-pages-offset", g_variant_new_int32(0));
    addTargetItem(pairOffset, "1 page", "win.paired-pages-offset", g_variant_new_int32(1));
    g_menu_append_submenu(menu, "First Spread Offset", G_MENU_MODEL(pairOffset));
    g_object_unref(pairOffset);

    pageLayoutMenuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pageLayoutMenuButton), "view-grid-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(pageLayoutMenuButton), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(pageLayoutMenuButton, "Page layout, reading direction, and view modes");
    gtk_accessible_update_property(GTK_ACCESSIBLE(pageLayoutMenuButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Page layout",
                                   -1);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), pageLayoutMenuButton);
    g_object_unref(menu);
}

}  // namespace prototype::gtk4
