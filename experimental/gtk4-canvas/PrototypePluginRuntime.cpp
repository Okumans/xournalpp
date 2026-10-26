#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::initializePlugins() {
    std::vector<std::filesystem::path> searchPaths;
    if (!options.pluginDirectory.empty()) {
        searchPaths.emplace_back(options.pluginDirectory);
    }
#ifdef GTK4_CANVAS_BUNDLED_PLUGIN_DIR
    searchPaths.emplace_back(GTK4_CANVAS_BUNDLED_PLUGIN_DIR);
#endif
    searchPaths.push_back(preferencesConfigDir / "plugins");

    pluginRuntime = std::make_unique<prototype::LuaPluginRuntime>();
    pluginRuntime->discover(searchPaths, importedConfig.settings.pluginsEnabled,
                            importedConfig.settings.pluginsDisabled);

    prototype::PluginRuntimeHost host;
    host.changeToolColor = [this](const auto& tool, std::uint32_t color, bool selection, std::string& error) {
        return changePluginToolColor(tool, color, selection, error);
    };
    host.getActionState = [this](std::string_view action, prototype::PluginActionValue& value, std::string& error) {
        return getPluginActionState(action, value, error);
    };
    host.getToolInfo = [this](std::string_view tool, prototype::PluginToolInfo& info, std::string& error) {
        return getPluginToolInfo(tool, info, error);
    };
    host.changeActionState = [this](std::string_view action, const prototype::PluginActionValue& value,
                                    std::string& error) { return changePluginActionState(action, value, error); };
    host.activateAction = [this](std::string_view action, std::string& error) {
        return activatePluginAction(action, error);
    };
    host.changeCurrentPageBackground = [this](std::string_view format, std::string_view config, std::string& error) {
        return changePluginPageBackground(format, config, error);
    };
    host.addEmptyPages = [this](bool force, std::string& error) { return addEmptyPagesFromPlugin(force, error); };
    host.transformSelectedImages = [this](std::int64_t action, std::string& error) {
        return transformSelectedImagesFromPlugin(action, error);
    };
    host.getDisplayDpi = [this] { return pluginDisplayDpi(); };
    host.getPageLabel = [this](std::size_t pageNumber, std::string& label, std::string& error) {
        return getPluginPageLabel(pageNumber, label, error);
    };
    host.getColorPalette = [this] { return getPluginColorPalette(); };
    host.refreshPage = [this](std::string& error) { return refreshPluginPage(error); };
    host.getFont = [this] { return prototype::PluginFont{state.textFontName, state.textFontSize}; };
    host.setFont = [this](const prototype::PluginFont& font, std::string& error) { return setPluginFont(font, error); };
    host.getScrollPosition = [this] { return getPluginScrollPosition(); };
    host.scrollToPage = [this](int page, bool relative, std::string& error) {
        return scrollPluginToPage(page, relative, error);
    };
    host.scrollToPosition = [this](double x, double y, bool relative, std::string& error) {
        return scrollPluginToPosition(x, y, relative, error);
    };
    host.getZoom = [this] { return state.zoom; };
    host.setZoom = [this](double value, std::string& error) { return setPluginZoom(value, error); };
    host.getStrokes = [this](std::string_view type, std::vector<prototype::PluginStroke>& strokes, std::string& error) {
        return getPluginStrokes(type, strokes, error);
    };
    host.getTexts = [this](std::string_view type, std::vector<prototype::PluginText>& texts, std::string& error) {
        return getPluginTexts(type, texts, error);
    };
    host.getLinks = [this](std::string_view type, std::vector<prototype::PluginLink>& links, std::string& error) {
        return getPluginLinks(type, links, error);
    };
    host.getImages = [this](std::string_view type, std::vector<prototype::PluginImage>& images, std::string& error) {
        return getPluginImages(type, images, error);
    };
    host.addToSelection = [this](const std::vector<std::uintptr_t>& references, std::string& error) {
        return addPluginReferencesToSelection(references, error);
    };
    host.clearSelection = [this] {
        state.clearSelection();
        update();
    };
    host.addStrokes = [this](const std::vector<prototype::PluginStrokeInput>& strokes,
                             prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                             std::string& error) { return addPluginStrokes(strokes, policy, references, error); };
    host.addTexts = [this](const std::vector<prototype::PluginTextInput>& texts,
                           prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                           std::string& error) { return addPluginTexts(texts, policy, references, error); };
    host.addLinks = [this](const std::vector<prototype::PluginLinkInput>& links,
                           prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                           std::string& error) { return addPluginLinks(links, policy, references, error); };
    host.addImages = [this](const std::vector<prototype::PluginImageInput>& images,
                            prototype::PluginStrokeUndoPolicy policy,
                            std::vector<prototype::PluginImageResult>& results,
                            std::string& error) { return addPluginImages(images, policy, results, error); };
    host.setPageSize = [this](double width, double height, bool relative, std::string& error) {
        return setPluginPageSize(width, height, relative, error);
    };
    host.setCurrentPage = [this](std::size_t pageNumber, std::string& error) {
        return setPluginCurrentPage(pageNumber, error);
    };
    host.setCurrentLayer = [this](std::size_t layerNumber, bool updateVisibility, std::string& error) {
        return setPluginCurrentLayer(layerNumber, updateVisibility, error);
    };
    host.setLayerVisibility = [this](bool visible, std::string& error) {
        return setPluginLayerVisibility(visible, error);
    };
    host.setCurrentLayerName = [this](std::string_view name, std::string& error) {
        return setPluginCurrentLayerName(name, error);
    };
    host.setBackgroundName = [this](std::string_view name, std::string& error) {
        return setPluginBackgroundName(name, error);
    };
    host.exportDocument = [this](const prototype::DocumentExportOptions& exportOptions, std::string& error) {
        return exportDocumentFromPlugin(exportOptions, error);
    };
    host.quickScreenshot = [this](std::string_view plugin, std::string& error) {
        return startQuickScreenshot(plugin, error);
    };
    host.documentFilename = [this] { return saveTarget; };
    host.documentStructure = [this] { return pluginDocumentStructure(); };
    host.openDialog = [this](std::string_view plugin, std::string_view message,
                             const std::vector<prototype::PluginDialogButton>& buttons, bool isError,
                             std::function<void(int)> callback, std::string& error) {
        return showPluginDialog(plugin, message, buttons, isError, std::move(callback), error);
    };
    host.reportError = [this](std::string_view plugin, std::string_view message) {
        if (window) {
            showAlert("Plugin error", std::string(plugin) + ": " + std::string(message));
        } else {
            std::cerr << "xournalpp-gtk4-canvas: plugin " << plugin << ": " << message << '\n';
        }
    };
    host.rebuildFloatingToolboxRows = [this] { rebuildFloatingToolboxRows(); };
    host.updateToolbarPlaceholder = [this](std::string_view id, std::string_view value) {
        if (!floatingToolboxRowsContainer) {
            return;
        }
        const std::string fullId(id);
        if (auto* label = findWidgetByData(floatingToolboxRowsContainer, "gtk4-plugin-placeholder-id", fullId.c_str());
            label && GTK_IS_LABEL(label)) {
            const std::string text(value);
            gtk_label_set_text(GTK_LABEL(label), text.c_str());
            gtk_accessible_update_property(GTK_ACCESSIBLE(label), GTK_ACCESSIBLE_PROPERTY_LABEL, text.c_str(), -1);
        }
    };
    pluginRuntime->loadEnabled(std::move(host));
}

void Prototype::removeQuickScreenshotDirectory(const QuickScreenshotContext& context) {
    if (!context.directory.empty()) {
        std::error_code error;
        std::filesystem::remove_all(context.directory, error);
    }
}

void Prototype::finishQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context) {
    if (context->process) {
        g_clear_object(&context->process);
    }
    if (context->self) {
        context->self->choosingFile = false;
        auto& operations = context->self->quickScreenshotOperations;
        operations.erase(std::remove(operations.begin(), operations.end(), context), operations.end());
        context->self = nullptr;
    }
    removeQuickScreenshotDirectory(*context);
}

void Prototype::failQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context, std::string message) {
    if (context->self) {
        auto* self = context->self;
        if (self->options.verifyPlugins) {
            self->quickScreenshotVerifyError = std::move(message);
            self->quickScreenshotVerifyDone = true;
        } else if (self->window) {
            self->showAlert("Quick Screenshot failed", message);
        }
    }
    finishQuickScreenshot(context);
}

void Prototype::cancelQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context) {
    if (context->self && context->self->options.verifyPlugins) {
        context->self->quickScreenshotVerifyError = "Region capture was cancelled";
        context->self->quickScreenshotVerifyDone = true;
    }
    finishQuickScreenshot(context);
}

bool Prototype::launchQuickScreenshotProcess(const std::shared_ptr<QuickScreenshotContext>& context,
                                             const std::vector<std::string>& arguments, bool captureOutput,
                                             std::string& error) {
    std::vector<const gchar*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument: arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);

    GError* spawnError = nullptr;
    const auto flags =
            captureOutput ?
                    static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE) :
                    static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                  G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    context->process = g_subprocess_newv(argv.data(), flags, &spawnError);
    if (!context->process) {
        error = spawnError ? spawnError->message : "Could not start the screenshot utility";
        g_clear_error(&spawnError);
        return false;
    }

    auto* callbackContext = new std::shared_ptr<QuickScreenshotContext>(context);
    if (captureOutput) {
        g_subprocess_communicate_utf8_async(
                context->process, nullptr, nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                            static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                    const auto operation = *boxed;
                    gchar* output = nullptr;
                    gchar* standardError = nullptr;
                    GError* error = nullptr;
                    const bool completed = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output,
                                                                                &standardError, &error);
                    std::string geometry = output ? output : "";
                    g_strstrip(geometry.data());
                    g_free(output);
                    g_free(standardError);
                    g_clear_error(&error);
                    g_clear_object(&operation->process);

                    if (!operation->self) {
                        removeQuickScreenshotDirectory(*operation);
                        return;
                    }
                    if (!completed || geometry.empty()) {
                        operation->self->cancelQuickScreenshot(operation);
                        return;
                    }

                    gchar* grimPath = g_find_program_in_path("grim");
                    std::string launchError;
                    const bool launched =
                            grimPath &&
                            operation->self->launchQuickScreenshotProcess(
                                    operation, {grimPath, "-g", geometry, operation->imagePath}, false, launchError);
                    g_free(grimPath);
                    if (!launched) {
                        operation->self->failQuickScreenshot(
                                operation,
                                launchError.empty() ? "The grim screenshot utility is unavailable" : launchError);
                    }
                },
                callbackContext);
    } else {
        g_subprocess_wait_check_async(
                context->process, nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                            static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                    const auto operation = *boxed;
                    GError* error = nullptr;
                    const bool completed = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
                    std::error_code fileError;
                    const auto imageStatus = std::filesystem::symlink_status(operation->imagePath, fileError);
                    const bool imageExists = !fileError && std::filesystem::is_regular_file(imageStatus);
                    auto imageSize = imageExists ? std::filesystem::file_size(operation->imagePath, fileError) : 0;
                    if (fileError) {
                        imageSize = 0;
                    }
                    g_clear_error(&error);
                    g_clear_object(&operation->process);

                    if (!operation->self) {
                        removeQuickScreenshotDirectory(*operation);
                        return;
                    }
                    if (!completed && !imageExists) {
                        operation->self->cancelQuickScreenshot(operation);
                        return;
                    }
                    if (!completed || imageSize == 0) {
                        operation->self->failQuickScreenshot(operation,
                                                             "The screenshot utility did not create a PNG image");
                        return;
                    }
                    operation->self->quickScreenshotReady(operation);
                },
                callbackContext);
    }
    return true;
}

bool Prototype::startQuickScreenshot(std::string_view plugin, std::string& error) {
    if (!window || closed || busy || choosingFile || messageDialog) {
        error = "Wait for the current dialog or document operation to finish";
        return false;
    }

    gchar* scrot = nullptr;
    gchar* maim = nullptr;
    gchar* xclip = nullptr;
    gchar* grim = nullptr;
    gchar* slurp = nullptr;
    bool wayland = g_getenv("WAYLAND_DISPLAY") != nullptr;
#ifdef GDK_WINDOWING_WAYLAND
    if (auto* display = gdk_display_get_default()) {
        wayland = GDK_IS_WAYLAND_DISPLAY(display);
    }
#endif
    if (options.verifyPlugins && g_getenv("XOURNALPP_TEST_QUICK_SCREENSHOT_X11")) {
        wayland = false;
    }
    if (wayland) {
        grim = g_find_program_in_path("grim");
        slurp = g_find_program_in_path("slurp");
    }
    if (!wayland || !grim || !slurp) {
        maim = g_find_program_in_path("maim");
        if (!wayland) {
            xclip = g_find_program_in_path("xclip");
        }
    }
    const bool clipboardCapture = !wayland && maim && xclip;
    if (!clipboardCapture && (!grim || !slurp)) {
        g_clear_pointer(&grim, g_free);
        g_clear_pointer(&slurp, g_free);
        scrot = g_find_program_in_path("scrot");
        if (!scrot && !maim) {
            maim = g_find_program_in_path("maim");
        }
    }
    if (!clipboardCapture && !grim && !scrot && !maim) {
        grim = g_find_program_in_path("grim");
        slurp = g_find_program_in_path("slurp");
    }
    if (!clipboardCapture && ((!grim || !slurp) && !scrot && !maim)) {
        g_free(grim);
        g_free(slurp);
        g_free(xclip);
        g_free(maim);
        error = "Quick Screenshot needs scrot or maim on X11, or grim and slurp on Wayland";
        return false;
    }

    GError* directoryError = nullptr;
    gchar* directory = g_dir_make_tmp("xournalpp-screenshot-XXXXXX", &directoryError);
    if (!directory) {
        error = directoryError ? directoryError->message : "Could not create temporary screenshot storage";
        g_clear_error(&directoryError);
        g_free(grim);
        g_free(slurp);
        g_free(scrot);
        g_free(maim);
        g_free(xclip);
        return false;
    }

    auto operation = std::make_shared<QuickScreenshotContext>();
    operation->self = this;
    operation->plugin = plugin;
    operation->directory = directory;
    operation->imagePath = (std::filesystem::path(directory) / "region.png").string();
    operation->copyToClipboard = clipboardCapture;
    g_free(directory);
    quickScreenshotOperations.push_back(operation);
    choosingFile = true;
    resetInput(true);

    bool launched = false;
    if (grim && slurp) {
        launched = launchQuickScreenshotProcess(operation, {slurp}, true, error);
    } else if (scrot) {
        launched = launchQuickScreenshotProcess(
                operation, {scrot, "--overwrite", "--select", "--silent", operation->imagePath}, false, error);
    } else if (maim) {
        launched = launchQuickScreenshotProcess(operation, {maim, "-s", operation->imagePath}, false, error);
    }
    g_free(grim);
    g_free(slurp);
    g_free(scrot);
    g_free(maim);
    g_free(xclip);
    if (!launched) {
        finishQuickScreenshot(operation);
        return false;
    }
    return true;
}

void Prototype::quickScreenshotReady(const std::shared_ptr<QuickScreenshotContext>& context) {
    if (!context->self) {
        removeQuickScreenshotDirectory(*context);
        return;
    }
    auto* self = context->self;
    auto* capturedImage = cairo_image_surface_create_from_png(context->imagePath.c_str());
    const auto imageStatus = cairo_surface_status(capturedImage);
    cairo_surface_destroy(capturedImage);
    if (imageStatus != CAIRO_STATUS_SUCCESS) {
        self->failQuickScreenshot(context, "The screenshot utility did not produce a readable PNG image");
        return;
    }
    if (context->copyToClipboard) {
        gchar* contents = nullptr;
        gsize length = 0;
        GError* error = nullptr;
        if (!g_file_get_contents(context->imagePath.c_str(), &contents, &length, &error)) {
            const std::string message = error ? error->message : "Could not read the captured PNG image";
            g_clear_error(&error);
            self->failQuickScreenshot(context, message);
            return;
        }
        auto* bytes = g_bytes_new_take(contents, length);
        auto* provider = gdk_content_provider_new_for_bytes("image/png", bytes);
        g_bytes_unref(bytes);
        const bool copied = gdk_clipboard_set_content(gtk_widget_get_clipboard(self->canvas), provider);
        g_object_unref(provider);
        if (!copied) {
            self->failQuickScreenshot(context, "Could not place the screenshot on the clipboard");
            return;
        }
        if (self->options.verifyPlugins) {
            self->quickScreenshotVerifyError.clear();
            self->quickScreenshotVerifyDone = true;
        }
        self->finishQuickScreenshot(context);
        return;
    }
    if (self->options.verifyPlugins) {
        const auto destination = std::filesystem::path(self->options.smokeDirectory) / "quick-screenshot.png";
        auto* file = g_file_new_for_path(destination.c_str());
        self->copyQuickScreenshot(context, file);
        g_object_unref(file);
        return;
    }

    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Quick Screenshot");
    gtk_file_dialog_set_initial_name(dialog, "Untitled.png");
    auto* callbackContext = new std::shared_ptr<QuickScreenshotContext>(context);
    gtk_file_dialog_save(
            dialog, GTK_WINDOW(self->window), nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                        static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                const auto operation = *boxed;
                if (!operation->self) {
                    removeQuickScreenshotDirectory(*operation);
                    return;
                }
                GError* error = nullptr;
                auto* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
                if (!file) {
                    const bool cancelled =
                            error && (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
                                      g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
                                      g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED));
                    const std::string message = error ? error->message : "Could not choose a file";
                    g_clear_error(&error);
                    if (cancelled) {
                        operation->self->cancelQuickScreenshot(operation);
                    } else {
                        operation->self->failQuickScreenshot(operation, message);
                    }
                    return;
                }
                operation->self->copyQuickScreenshot(operation, file);
                g_object_unref(file);
            },
            callbackContext);
    g_object_unref(dialog);
}

void Prototype::copyQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context, GFile* destination) {
    auto* source = g_file_new_for_path(context->imagePath.c_str());
    auto* callbackContext = new std::shared_ptr<QuickScreenshotContext>(context);
    g_file_copy_async(
            source, destination, G_FILE_COPY_OVERWRITE, G_PRIORITY_DEFAULT, nullptr, nullptr, nullptr,
            +[](GObject* sourceFile, GAsyncResult* result, gpointer data) {
                std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                        static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                const auto operation = *boxed;
                GError* error = nullptr;
                const bool copied = g_file_copy_finish(G_FILE(sourceFile), result, &error);
                const std::string message = error ? error->message : "Could not save the screenshot";
                g_clear_error(&error);
                if (!operation->self) {
                    removeQuickScreenshotDirectory(*operation);
                    return;
                }
                auto* self = operation->self;
                if (self->options.verifyPlugins) {
                    self->quickScreenshotVerifyError = copied ? "" : message;
                    self->quickScreenshotVerifyDone = true;
                } else if (!copied && self->window) {
                    self->showAlert("Could not save Quick Screenshot", message);
                }
                self->finishQuickScreenshot(operation);
            },
            callbackContext);
    g_object_unref(source);
}

bool Prototype::showPluginDialog(std::string_view pluginName, std::string_view message,
                                 const std::vector<prototype::PluginDialogButton>& buttons, bool isError,
                                 std::function<void(int)> callback, std::string& error) {
    if (!window) {
        error = "The main window is not available";
        return false;
    }
    if (messageDialog) {
        error = "Another dialog is already open";
        return false;
    }

    const std::string title =
            (isError ? "Error in Xournal++ Plugin «" : "Xournal++ Plugin «") + std::string(pluginName) + "»";
    const std::string body(message);
    auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title.c_str(), body.c_str()));
    auto* context = new PluginDialogContext{this, {}, std::move(callback)};
    for (const auto& button: buttons) {
        const auto response = std::to_string(button.response);
        const auto responseId = "plugin-response-" + response;
        context->responses.emplace(responseId, button.response);
        adw_alert_dialog_add_response(dialog, responseId.c_str(), button.label.c_str());
    }
    constexpr const char* dismissResponse = "plugin-dismiss";
    adw_alert_dialog_set_close_response(dialog, dismissResponse);
    if (!buttons.empty()) {
        const auto firstResponse = "plugin-response-" + std::to_string(buttons.front().response);
        adw_alert_dialog_set_default_response(dialog, firstResponse.c_str());
    }
    g_signal_connect_data(
            dialog, "response", G_CALLBACK(+[](AdwAlertDialog* dialog, const char* response, gpointer data) {
                auto* context = static_cast<PluginDialogContext*>(data);
                if (context->prototype->messageDialog == dialog) {
                    context->prototype->messageDialog = nullptr;
                }
                const auto selected = context->responses.find(response ? response : "");
                if (selected != context->responses.end() && context->callback) {
                    context->callback(selected->second);
                }
            }),
            context, +[](gpointer data, GClosure*) { delete static_cast<PluginDialogContext*>(data); },
            static_cast<GConnectFlags>(0));
    messageDialog = dialog;
    adw_dialog_present(ADW_DIALOG(dialog), window);
    return true;
}

void Prototype::openPluginManager() {
    if (!pluginRuntime) {
        showAlert("Plugins unavailable", "The Lua plugin runtime did not initialize.");
        return;
    }
    pluginRuntime->resetPendingStates();
    if (!pluginManagerWindow) {
        buildPluginManager();
    }
    rebuildPluginManagerRows();
    gtk_window_present(pluginManagerWindow);
}

void Prototype::buildPluginManager() {
    pluginManagerWindow = GTK_WINDOW(g_object_ref_sink(gtk_window_new()));
    gtk_window_set_title(pluginManagerWindow, "Manage Plugins");
    gtk_window_set_default_size(pluginManagerWindow, 620, 640);
    gtk_window_set_transient_for(pluginManagerWindow, GTK_WINDOW(window));
    gtk_window_set_modal(pluginManagerWindow, true);
    g_signal_connect(pluginManagerWindow, "close-request", G_CALLBACK(+[](GtkWindow* dialog, gpointer) -> gboolean {
                         gtk_widget_set_visible(GTK_WIDGET(dialog), false);
                         return true;
                     }),
                     nullptr);

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto* header = adw_header_bar_new();
    auto* title = gtk_label_new("Plugins");
    gtk_widget_add_css_class(title, "title");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
    gtk_window_set_titlebar(pluginManagerWindow, header);

    auto* explanation =
            gtk_label_new("Choose which plugins to load. Changes take effect the next time Xournal++ starts.");
    gtk_label_set_wrap(GTK_LABEL(explanation), true);
    gtk_widget_set_halign(explanation, GTK_ALIGN_START);
    gtk_widget_set_margin_start(explanation, 18);
    gtk_widget_set_margin_end(explanation, 18);
    gtk_widget_set_margin_top(explanation, 12);
    gtk_widget_set_margin_bottom(explanation, 12);
    gtk_box_append(GTK_BOX(root), explanation);

    auto* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, true);
    pluginManagerList = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(pluginManagerList), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(pluginManagerList, "boxed-list");
    gtk_widget_set_margin_start(pluginManagerList, 18);
    gtk_widget_set_margin_end(pluginManagerList, 18);
    gtk_widget_set_margin_bottom(pluginManagerList, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), pluginManagerList);
    gtk_box_append(GTK_BOX(root), scroller);

    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    gtk_widget_set_margin_start(actions, 18);
    gtk_widget_set_margin_end(actions, 18);
    gtk_widget_set_margin_bottom(actions, 18);
    auto* cancel = gtk_button_new_with_label("Cancel");
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         gtk_widget_set_visible(GTK_WIDGET(self->pluginManagerWindow), false);
                     }),
                     this);
    auto* apply = gtk_button_new_with_label("Apply");
    pluginManagerApplyButton = apply;
    gtk_widget_add_css_class(apply, "suggested-action");
    g_signal_connect(apply, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Prototype*>(data)->applyPluginManagerChanges();
                     }),
                     this);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), apply);
    gtk_box_append(GTK_BOX(root), actions);
    gtk_window_set_child(pluginManagerWindow, root);
}

void Prototype::rebuildPluginManagerRows() {
    pluginManagerSwitches.clear();
    while (auto* row = gtk_widget_get_first_child(pluginManagerList)) {
        gtk_list_box_remove(GTK_LIST_BOX(pluginManagerList), row);
    }
    for (const auto& plugin: pluginRuntime->plugins()) {
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
        gtk_widget_set_margin_start(row, 14);
        gtk_widget_set_margin_end(row, 14);
        gtk_widget_set_margin_top(row, 10);
        gtk_widget_set_margin_bottom(row, 10);

        auto* details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
        gtk_widget_set_hexpand(details, true);
        auto* name = gtk_label_new(plugin.name.c_str());
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_widget_add_css_class(name, "heading");
        gtk_box_append(GTK_BOX(details), name);
        std::string summary = plugin.description;
        if (!plugin.author.empty()) {
            if (!summary.empty())
                summary += " · ";
            summary += plugin.author;
        }
        if (!plugin.version.empty() && plugin.version != "<xournalpp>") {
            if (!summary.empty())
                summary += " · ";
            summary += "v" + plugin.version;
        }
        if (!plugin.error.empty()) {
            if (!summary.empty())
                summary += "\n";
            summary += plugin.error;
        }
        auto* description = gtk_label_new(summary.c_str());
        gtk_label_set_xalign(GTK_LABEL(description), 0);
        gtk_label_set_wrap(GTK_LABEL(description), true);
        gtk_widget_add_css_class(description, plugin.error.empty() ? "dim-label" : "error");
        gtk_box_append(GTK_BOX(details), description);
        gtk_box_append(GTK_BOX(row), details);

        auto* toggle = gtk_switch_new();
        gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
        gtk_widget_set_sensitive(toggle, plugin.valid);
        gtk_accessible_update_property(GTK_ACCESSIBLE(toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, plugin.name.c_str(), -1);
        gtk_switch_set_active(GTK_SWITCH(toggle), plugin.pendingEnabled);
        g_object_set_data_full(G_OBJECT(toggle), "plugin-name", g_strdup(plugin.name.c_str()), g_free);
        g_signal_connect(toggle, "notify::active", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto* name = static_cast<const char*>(g_object_get_data(object, "plugin-name"));
                             if (name && self->pluginRuntime) {
                                 self->pluginRuntime->setPendingEnabled(name,
                                                                        gtk_switch_get_active(GTK_SWITCH(object)));
                             }
                         }),
                         this);
        pluginManagerSwitches.emplace(plugin.name, toggle);
        gtk_box_append(GTK_BOX(row), toggle);
        gtk_list_box_append(GTK_LIST_BOX(pluginManagerList), row);
    }
}

void Prototype::applyPluginManagerChanges() {
    const auto [enabled, disabled] = pluginRuntime->pendingPluginPaths();
    importedConfig.settings.pluginsEnabled = enabled;
    importedConfig.settings.pluginsDisabled = disabled;
    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        showAlert("Cannot save plugin settings", error);
        return;
    }
    const bool changed = pluginRuntime->applyPendingStates();
    gtk_widget_set_visible(GTK_WIDGET(pluginManagerWindow), false);
    if (changed) {
        showAlert("Restart required", "Plugin changes are saved. Restart Xournal++ to load the updated plugins.");
    }
}

}  // namespace prototype::gtk4
