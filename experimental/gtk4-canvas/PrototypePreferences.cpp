#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::openPreferences() {
    PreferencesDialog::Callbacks callbacks;
    callbacks.changePreference = [this](PreferenceKey key, GtkWidget* control) { changePreference(key, control); };
    callbacks.chooseLatexTemplate = [this] { chooseLatexTemplate(); };
    callbacks.applyFloatingToolboxRows = [this] { (void)applyFloatingToolboxRows(); };
    callbacks.applyMainToolRail = [this] { (void)applyMainToolRail(); };
    callbacks.applyLatexPreferences = [this] { return applyLatexPreferences(); };
    callbacks.testLatexSetup = [this] { testLatexSetup(); };
    preferencesDialog.open(GTK_WINDOW(window), importedConfig.settings, std::move(callbacks));
}

GdkRGBA Prototype::unpackArgbColor(std::uint32_t color) {
    return {
            static_cast<float>((color >> 16U) & 0xffU) / 255.0F,
            static_cast<float>((color >> 8U) & 0xffU) / 255.0F,
            static_cast<float>(color & 0xffU) / 255.0F,
            static_cast<float>((color >> 24U) & 0xffU) / 255.0F,
    };
}

std::uint32_t Prototype::packArgbColor(const GdkRGBA& color) {
    const auto channel = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(static_cast<double>(value), 0.0, 1.0) * 255.0));
    };
    return (channel(color.alpha) << 24U) | (channel(color.red) << 16U) | (channel(color.green) << 8U) |
           channel(color.blue);
}


bool Prototype::applyFloatingToolboxRows() {
    static constexpr std::array<PreferenceKey, 4> keys = {
            PreferenceKey::FloatingToolboxRow1,
            PreferenceKey::FloatingToolboxRow2,
            PreferenceKey::FloatingToolboxRow3,
            PreferenceKey::FloatingToolboxRow4,
    };
    const auto previousRows = importedConfig.settings.floatingToolboxRows;
    prototype::FloatingToolboxRows rows;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        auto* entry = preferencesDialog.control(keys[index]);
        const char* raw = gtk_editable_get_text(GTK_EDITABLE(entry));
        auto parsed = prototype::parseFloatingToolboxRow(raw ? raw : "");
        if (!parsed) {
            gtk_widget_add_css_class(entry, "error");
            showAlert("Invalid floating toolbox row",
                      "Use comma-separated toolbar action names. Empty rows are allowed.");
            return false;
        }
        gtk_widget_remove_css_class(entry, "error");
        rows[index] = std::move(*parsed);
    }

    importedConfig.settings.floatingToolboxRows = std::move(rows);
    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        importedConfig.settings.floatingToolboxRows = previousRows;
        showAlert("Could not save toolbar preferences", error);
        return false;
    }
    rebuildFloatingToolboxRows();
    return true;
}

bool Prototype::applyMainToolRail() {
    auto* entry = preferencesDialog.control(PreferenceKey::MainToolRail);
    const char* raw = gtk_editable_get_text(GTK_EDITABLE(entry));
    const auto parsed = prototype::parseMainToolRail(raw ? raw : "");
    if (!parsed) {
        gtk_widget_add_css_class(entry, "error");
        showAlert("Invalid main tool rail layout",
                  "Use comma-separated action names and keep SMART_SELECT in the rail.");
        return false;
    }
    gtk_widget_remove_css_class(entry, "error");

    const auto previous = importedConfig.settings.mainToolRail;
    importedConfig.settings.mainToolRail = *parsed;
    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        importedConfig.settings.mainToolRail = previous;
        showAlert("Could not save toolbar preferences", error);
        return false;
    }
    preferencesDialog.setPendingMainToolRail(*parsed);
    rebuildMainToolRail();
    return true;
}


void Prototype::changePreference(PreferenceKey key, GtkWidget* control) {
    if (preferencesDialog.syncingControls()) {
        return;
    }
    auto& settings = importedConfig.settings;
    const bool layoutPreference = key == PreferenceKey::LayoutFlow || key == PreferenceKey::LayoutGridDimension ||
                                  key == PreferenceKey::LayoutGridCount || key == PreferenceKey::LayoutPairedPages ||
                                  key == PreferenceKey::LayoutPairedPageOffset ||
                                  key == PreferenceKey::LayoutHorizontalDirection ||
                                  key == PreferenceKey::LayoutVerticalDirection;
    const bool viewModePreference =
            key == PreferenceKey::FullscreenMenubar || key == PreferenceKey::FullscreenToolbar ||
            key == PreferenceKey::FullscreenSidebar || key == PreferenceKey::PresentationGoFullscreen ||
            key == PreferenceKey::PresentationMenubar || key == PreferenceKey::PresentationToolbar ||
            key == PreferenceKey::PresentationSidebar;
    const bool preloadPreference = key == PreferenceKey::PreloadPagesBefore ||
                                   key == PreferenceKey::PreloadPagesAfter || key == PreferenceKey::PreloadDetail;
    const bool canvasCachePreference =
            key == PreferenceKey::FullDetailCachePages || key == PreferenceKey::MediumDetailCachePages ||
            key == PreferenceKey::MediumDetailCacheMegabytes || key == PreferenceKey::LowDetailCachePages ||
            key == PreferenceKey::LowDetailCacheMegabytes;
    switch (key) {
        case PreferenceKey::Theme:
            switch (adw_combo_row_get_selected(ADW_COMBO_ROW(control))) {
                case 1:
                    settings.themeVariant = prototype::ThemeVariant::ForceLight;
                    break;
                case 2:
                    settings.themeVariant = prototype::ThemeVariant::ForceDark;
                    break;
                default:
                    settings.themeVariant = prototype::ThemeVariant::UseSystem;
                    break;
            }
            break;
        case PreferenceKey::Sidebar:
            settings.showSidebar = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Default)].showSidebar =
                    settings.showSidebar;
            break;
        case PreferenceKey::FullscreenMenubar:
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Fullscreen)].showMenubar =
                    adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::FullscreenToolbar:
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Fullscreen)].showToolbar =
                    adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::FullscreenSidebar:
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Fullscreen)].showSidebar =
                    adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::PresentationGoFullscreen:
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].goFullscreen =
                    adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::PresentationMenubar:
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].showMenubar =
                    adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::PresentationToolbar:
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].showToolbar =
                    adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::PresentationSidebar:
            settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].showSidebar =
                    adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LayoutFlow:
            settings.pageLayout.vertical = adw_combo_row_get_selected(ADW_COMBO_ROW(control)) == 0;
            break;
        case PreferenceKey::LayoutGridDimension: {
            const bool fixedRows = adw_combo_row_get_selected(ADW_COMBO_ROW(control)) == 1;
            if (settings.pageLayout.fixedRows != fixedRows) {
                auto* countControl = preferencesDialog.control(PreferenceKey::LayoutGridCount);
                const int count = countControl ? gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(countControl)) : 1;
                if (fixedRows) {
                    settings.pageLayout.rows = static_cast<std::size_t>(std::clamp(count, 1, 8));
                } else {
                    settings.pageLayout.columns = static_cast<std::size_t>(std::clamp(count, 1, 8));
                }
                settings.pageLayout.fixedRows = fixedRows;
            }
            break;
        }
        case PreferenceKey::LayoutGridCount: {
            const auto count = static_cast<std::size_t>(
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 1, 8));
            if (settings.pageLayout.fixedRows) {
                settings.pageLayout.rows = count;
            } else {
                settings.pageLayout.columns = count;
            }
            break;
        }
        case PreferenceKey::LayoutPairedPages:
            settings.pageLayout.pairedPages = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LayoutPairedPageOffset:
            settings.pageLayout.pairedPageOffset =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 0, 100);
            break;
        case PreferenceKey::LayoutHorizontalDirection:
            settings.pageLayout.rightToLeft = adw_combo_row_get_selected(ADW_COMBO_ROW(control)) == 1;
            break;
        case PreferenceKey::LayoutVerticalDirection:
            settings.pageLayout.bottomToTop = adw_combo_row_get_selected(ADW_COMBO_ROW(control)) == 1;
            break;
        case PreferenceKey::PressureSensitivity:
            settings.pressureSensitivity = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::PositionHighlight:
            settings.highlightPosition = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::CursorHighlightColor:
            settings.cursorHighlightColor =
                    packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(control)));
            break;
        case PreferenceKey::CursorHighlightRadius:
            settings.cursorHighlightRadius = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::CursorHighlightBorderColor:
            settings.cursorHighlightBorderColor =
                    packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(control)));
            break;
        case PreferenceKey::CursorHighlightBorderWidth:
            settings.cursorHighlightBorderWidth = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::LatexAutoCheckDependencies:
            settings.latexAutoCheckDependencies = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LatexSourceViewTheme: {
            const auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(control));
            if (selected >= preferencesDialog.latexSourceViewThemeIds().size()) {
                return;
            }
            settings.latexSourceViewThemeId = preferencesDialog.latexSourceViewThemeIds()[selected];
            break;
        }
        case PreferenceKey::LatexSourceViewAutoIndent:
            settings.latexSourceViewAutoIndent = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LatexSourceViewSyntaxHighlight:
            settings.latexSourceViewSyntaxHighlight = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LatexSourceViewShowLineNumbers:
            settings.latexSourceViewShowLineNumbers = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LatexEditorFont: {
            PangoFontDescription* description = gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(control));
            if (description) {
                gchar* value = pango_font_description_to_string(description);
                settings.latexEditorFont = value ? value : "Monospace 12";
                g_free(value);
                pango_font_description_free(description);
            }
            break;
        }
        case PreferenceKey::LatexUseCustomEditorFont:
            settings.latexUseCustomEditorFont = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            preferencesDialog.updateLatexPreferenceSensitivity(settings);
            break;
        case PreferenceKey::LatexEditorWordWrap:
            settings.latexEditorWordWrap = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LatexUseExternalEditor:
            settings.latexUseExternalEditor = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            preferencesDialog.updateLatexPreferenceSensitivity(settings);
            break;
        case PreferenceKey::LatexExternalEditorAutoConfirm:
            settings.latexExternalEditorAutoConfirm = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::MinimumPressure:
            settings.minimumPressure = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::PressureMultiplier:
            settings.pressureMultiplier = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::StylusButtonOne:
            settings.stylusButtonOne =
                    static_cast<prototype::ButtonToolAction>(adw_combo_row_get_selected(ADW_COMBO_ROW(control)));
            break;
        case PreferenceKey::StylusButtonTwo:
            settings.stylusButtonTwo =
                    static_cast<prototype::ButtonToolAction>(adw_combo_row_get_selected(ADW_COMBO_ROW(control)));
            break;
        case PreferenceKey::AutosaveEnabled:
            settings.autosaveEnabled = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::AutosaveTimeout:
            settings.autosaveTimeoutMinutes =
                    static_cast<int>(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)));
            break;
        case PreferenceKey::ZoomStep:
            settings.zoomStep = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control)) / 100.0;
            break;
        case PreferenceKey::ScrollZoomStep:
            settings.scrollZoomStep = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control)) / 100.0;
            break;
        case PreferenceKey::PreloadPagesBefore:
            settings.preloadPagesBefore = static_cast<unsigned int>(
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 0, 99));
            break;
        case PreferenceKey::PreloadPagesAfter:
            settings.preloadPagesAfter = static_cast<unsigned int>(
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 0, 99));
            break;
        case PreferenceKey::FullDetailCachePages:
            settings.fullDetailCachePages =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 1, 8);
            break;
        case PreferenceKey::MediumDetailCachePages:
            settings.mediumDetailCachePages =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 1, 64);
            break;
        case PreferenceKey::MediumDetailCacheMegabytes:
            settings.mediumDetailCacheMegabytes =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 1, 512);
            break;
        case PreferenceKey::LowDetailCachePages:
            settings.lowDetailCachePages =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 1, 512);
            break;
        case PreferenceKey::LowDetailCacheMegabytes:
            settings.lowDetailCacheMegabytes =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 1, 512);
            break;
        case PreferenceKey::OverviewZoomThreshold:
            settings.overviewZoomThresholdPercent =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 5, 100);
            break;
        case PreferenceKey::LowDetailZoomThreshold:
            settings.lowDetailZoomThresholdPercent =
                    std::clamp(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)), 5, 50);
            break;
        case PreferenceKey::PreloadDetail:
            settings.preloadDetail =
                    static_cast<prototype::CanvasPreloadDetail>(adw_combo_row_get_selected(ADW_COMBO_ROW(control)));
            break;
        case PreferenceKey::EdgePanSpeed:
            settings.edgePanSpeed = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::EdgePanMaxMultiplier:
            settings.edgePanMaxMultiplier = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::RotationSnapTolerance:
            settings.snapRotationTolerance = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::SnapRotation:
            settings.snapRotation = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::SnapGrid:
            settings.snapGrid = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::SnapGridSize:
            settings.snapGridSize = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::SnapGridTolerance:
            settings.snapGridTolerance = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
            break;
        case PreferenceKey::FloatingToolboxRow1:
        case PreferenceKey::FloatingToolboxRow2:
        case PreferenceKey::FloatingToolboxRow3:
        case PreferenceKey::FloatingToolboxRow4:
        case PreferenceKey::MainToolRail:
        case PreferenceKey::LatexDefaultText:
        case PreferenceKey::LatexTemplatePath:
        case PreferenceKey::LatexGeneratorCommand:
        case PreferenceKey::LatexExternalEditorCommand:
        case PreferenceKey::LatexTemporaryFileExtension:
        case PreferenceKey::Count:
            return;
    }

    if (preloadPreference) {
        scheduleCanvasPagePreload();
    }
    if (canvasCachePreference) {
        applyCanvasCacheLimits();
    }
    prototype::applyImportedSettingsToCanvas(state, settings);
    if (layoutPreference) {
        syncPageLayoutActions();
        if (canvas) {
            state.scrollToPage(state.page, gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
        }
    }
    input.setStabilizerConfig(settings.stabilizer);
    effectiveAutosaveSeconds = settings.effectiveAutosaveSeconds();
    scheduleAutosave(static_cast<guint>(std::max(0, effectiveAutosaveSeconds)));
    if (splitView) {
        adw_overlay_split_view_set_show_sidebar(splitView, settings.showSidebar);
    }
    if (!options.explicitDark) {
        const auto scheme = settings.themeVariant == prototype::ThemeVariant::ForceLight ?
                                    ADW_COLOR_SCHEME_FORCE_LIGHT :
                            settings.themeVariant == prototype::ThemeVariant::ForceDark ? ADW_COLOR_SCHEME_FORCE_DARK :
                                                                                          ADW_COLOR_SCHEME_DEFAULT;
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), scheme);
    }
    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, settings, error)) {
        showAlert("Could not save preferences", error);
    }
    if (viewModePreference) {
        applyCurrentViewMode();
    }
    update();
}

void Prototype::chooseLatexTemplate() {
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose a TeX template");
    const char* currentPath = gtk_editable_get_text(GTK_EDITABLE(preferencesDialog.latexTemplatePathEntry()));
    if (currentPath && *currentPath) {
        GFile* currentFile = g_file_new_for_path(currentPath);
        gtk_file_dialog_set_initial_file(dialog, currentFile);
        g_object_unref(currentFile);
    }

    using DialogContext = std::pair<Prototype*, GtkFileDialog*>;
    auto* context = new DialogContext(this, dialog);
    gtk_file_dialog_open(
            dialog, GTK_WINDOW(window), nullptr,
            +[](GObject*, GAsyncResult* result, gpointer data) {
                std::unique_ptr<DialogContext> context(static_cast<DialogContext*>(data));
                GError* error = nullptr;
                GFile* file = gtk_file_dialog_open_finish(context->second, result, &error);
                if (file) {
                    gchar* path = g_file_get_path(file);
                    if (path) {
                        gtk_editable_set_text(GTK_EDITABLE(context->first->preferencesDialog.latexTemplatePathEntry()),
                                              path);
                        g_free(path);
                    } else {
                        context->first->showAlert("Template must be a local file",
                                                  "Choose a template stored on this device.");
                    }
                    g_object_unref(file);
                }
                g_clear_error(&error);
                g_object_unref(context->second);
            },
            context);
}

bool Prototype::applyLatexPreferences() {
    auto entryText = [this](PreferenceKey key) {
        auto* entry = preferencesDialog.control(key);
        const char* text = entry ? gtk_editable_get_text(GTK_EDITABLE(entry)) : "";
        return std::string(text ? text : "");
    };
    const auto defaultText = entryText(PreferenceKey::LatexDefaultText);
    const auto templatePathText = entryText(PreferenceKey::LatexTemplatePath);
    const auto generatorCommand = entryText(PreferenceKey::LatexGeneratorCommand);
    const auto externalEditorCommand = entryText(PreferenceKey::LatexExternalEditorCommand);
    const auto temporaryFileExtension = entryText(PreferenceKey::LatexTemporaryFileExtension);
    if (defaultText.size() > prototype::maxCanvasLatexSourceBytes || templatePathText.size() > 4096 ||
        generatorCommand.size() > 16384 || externalEditorCommand.size() > 16384 || temporaryFileExtension.size() > 64 ||
        temporaryFileExtension.find('/') != std::string::npos ||
        temporaryFileExtension.find('\\') != std::string::npos ||
        std::any_of(temporaryFileExtension.begin(), temporaryFileExtension.end(),
                    [](unsigned char value) { return value < 0x20; })) {
        showAlert("LaTeX settings could not be applied",
                  "Check the formula size, command lengths, template path, and temporary file suffix.");
        return false;
    }

    auto& settings = importedConfig.settings;
    const auto oldDefaultText = settings.latexDefaultText;
    const auto oldTemplatePath = settings.latexTemplatePath;
    const auto oldGeneratorCommand = settings.latexGeneratorCommand;
    const auto oldExternalEditorCommand = settings.latexExternalEditorCommand;
    const auto oldTemporaryFileExtension = settings.latexTemporaryFileExtension;
    settings.latexDefaultText = defaultText;
    settings.latexTemplatePath = templatePathText.empty() ?
                                         std::filesystem::path{} :
                                         prototype::preferences_detail::pathFromUtf8(templatePathText);
    settings.latexGeneratorCommand = generatorCommand;
    settings.latexExternalEditorCommand = externalEditorCommand;
    settings.latexTemporaryFileExtension = temporaryFileExtension;

    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, settings, error)) {
        settings.latexDefaultText = oldDefaultText;
        settings.latexTemplatePath = oldTemplatePath;
        settings.latexGeneratorCommand = oldGeneratorCommand;
        settings.latexExternalEditorCommand = oldExternalEditorCommand;
        settings.latexTemporaryFileExtension = oldTemporaryFileExtension;
        showAlert("Could not save LaTeX preferences", error);
        return false;
    }

    latexTemplatePath = settings.latexTemplatePath;
    if (latexTemplatePath.empty()) {
#ifdef GTK4_CANVAS_RESOURCE_DIR
        latexTemplatePath = std::filesystem::path(GTK4_CANVAS_RESOURCE_DIR) / "default_template.tex";
#endif
    }
    preferencesDialog.updateLatexPreferenceSensitivity(settings);
    preferencesDialog.setLatexSettingsSaved();
    return true;
}


void Prototype::testLatexSetup() {
    if (!applyLatexPreferences()) {
        return;
    }
    if (latexTemplatePath.empty()) {
        showAlert("Choose a TeX template", "Select a template file before checking the LaTeX setup.");
        return;
    }
    preferencesDialog.setLatexSetupRunning(true);
    const auto& settings = importedConfig.settings;
    prototype::LatexEditor::testConfiguration(
            latexTemplatePath, settings.latexGeneratorCommand, settings.latexDefaultText, latexBrush.color,
            [this](bool success, std::string message) {
                preferencesDialog.setLatexSetupResult(success);
                showAlert(success ? "LaTeX setup works" : "LaTeX setup failed", message);
            });
}


}  // namespace prototype::gtk4
