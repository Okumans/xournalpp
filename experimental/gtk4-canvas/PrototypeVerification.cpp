#include <algorithm>
#include <memory>

#include "Prototype.h"

namespace prototype::gtk4 {

namespace {

class SearchEventLoopHeartbeat {
public:
    explicit SearchEventLoopHeartbeat(Gtk4SearchController& searchController):
            searchController(searchController), lastTick(g_get_monotonic_time()) {
        source = g_timeout_add_full(G_PRIORITY_DEFAULT, 16, &SearchEventLoopHeartbeat::tick, this, nullptr);
    }

    ~SearchEventLoopHeartbeat() { stop(); }

    void stop() {
        if (source) {
            g_source_remove(source);
            source = 0;
        }
    }

    gint64 maximumGapMicroseconds = 0;
    size_t ticks = 0;
    int pagesScannedAtMaximumGap = 0;

private:
    static gboolean tick(gpointer data) {
        auto* self = static_cast<SearchEventLoopHeartbeat*>(data);
        const auto now = g_get_monotonic_time();
        const gint64 gap = now - self->lastTick;
        if (gap > self->maximumGapMicroseconds) {
            self->maximumGapMicroseconds = gap;
            self->pagesScannedAtMaximumGap = self->searchController.pagesScanned();
        }
        self->lastTick = now;
        ++self->ticks;
        return G_SOURCE_CONTINUE;
    }

    Gtk4SearchController& searchController;
    gint64 lastTick = 0;
    guint source = 0;
};

}  // namespace

void Prototype::captureTopLevel(const std::string& name, GtkWidget* topLevel) {
    const auto deadline = g_get_monotonic_time() + 750000;
    while (!gtk_widget_get_mapped(topLevel) && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    auto* frameClock = gtk_widget_get_frame_clock(topLevel);
    require(frameClock != nullptr, "Plugin manager has no frame clock");
    const auto previousFrame = gdk_frame_clock_get_frame_counter(frameClock);
    gtk_widget_queue_draw(topLevel);
    while (gdk_frame_clock_get_frame_counter(frameClock) <= previousFrame && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    auto* paintable = gtk_widget_paintable_new(topLevel);
    auto* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(paintable), GDK_SNAPSHOT(snapshot), gtk_widget_get_width(topLevel),
                           gtk_widget_get_height(topLevel));
    auto* node = gtk_snapshot_free_to_node(snapshot);
    if (!node) {
        g_object_unref(paintable);
        throw SnapshotPending{};
    }
    auto* renderer = gtk_native_get_renderer(GTK_NATIVE(topLevel));
    auto* texture = gsk_renderer_render_texture(renderer, node, nullptr);
    const auto path = options.smokeDirectory + "/" + name + ".png";
    const bool saved = gdk_texture_save_to_png(texture, path.c_str());
    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    if (!saved) {
        throw std::runtime_error("Cannot write plugin manager screenshot");
    }
}

void Prototype::verifyQuickScreenshotPlugin() {
    const auto plugin = pluginRuntime->findPlugin("QuickScreenshot");
    require(plugin && plugin->loaded && plugin->menuItems == 1,
            "Bundled QuickScreenshot did not register its GTK4 menu callback");
    quickScreenshotVerifyDone = false;
    quickScreenshotVerifyError.clear();
    require(pluginRuntime->invokeMenu("QuickScreenshot", 0), "QuickScreenshot did not dispatch its capture action");

    const auto deadline = g_get_monotonic_time() + 10000000;
    while (!quickScreenshotVerifyDone && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(quickScreenshotVerifyDone,
            "QuickScreenshot did not finish capture and saving before the verification timeout");
    require(quickScreenshotVerifyError.empty(), quickScreenshotVerifyError.c_str());

    const auto path = std::filesystem::path(options.smokeDirectory) / "quick-screenshot.png";
    auto* surface = cairo_image_surface_create_from_png(path.c_str());
    const auto status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    require(status == CAIRO_STATUS_SUCCESS, "QuickScreenshot did not save a valid PNG file");

    g_setenv("XOURNALPP_TEST_QUICK_SCREENSHOT_X11", "1", true);
    quickScreenshotVerifyDone = false;
    quickScreenshotVerifyError.clear();
    const bool clipboardCaptureStarted = pluginRuntime->invokeMenu("QuickScreenshot", 0);
    g_unsetenv("XOURNALPP_TEST_QUICK_SCREENSHOT_X11");
    const auto clipboardDeadline = g_get_monotonic_time() + 10000000;
    while (clipboardCaptureStarted && !quickScreenshotVerifyDone && g_get_monotonic_time() < clipboardDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(clipboardCaptureStarted && quickScreenshotVerifyDone && quickScreenshotVerifyError.empty(),
            quickScreenshotVerifyError.empty() ? "QuickScreenshot did not finish the X11 clipboard capture path" :
                                                 quickScreenshotVerifyError.c_str());
    const auto* clipboardFormats = gdk_clipboard_get_formats(gtk_widget_get_clipboard(canvas));
    require(gdk_content_formats_contain_mime_type(clipboardFormats, "image/png"),
            "QuickScreenshot did not publish PNG data to the GTK clipboard");
}

void Prototype::verifyFloatingToolboxTextFormats() {
    using Format = prototype::TextEditorOverlay::Format;
    const std::pair<const char*, Format> formats[] = {
            {"FORMAT_BOLD", Format::Bold},
            {"FORMAT_ITALIC", Format::Italic},
            {"FORMAT_UNDERLINE", Format::Underline},
            {"FORMAT_STRIKETHROUGH", Format::Strikethrough},
            {"FORMAT_SUPERSCRIPT", Format::Superscript},
            {"FORMAT_SUBSCRIPT", Format::Subscript},
    };

    for (const auto& [token, format]: formats) {
        auto* button = findWidgetByData(floatingToolboxRowsContainer, "gtk4-floating-text-format", token);
        require(button && GTK_IS_BUTTON(button), "Configured floating text-format action did not render");

        prototype::TextItem draft;
        draft.text = "Floating toolbar format";
        textEditor->begin(std::move(draft), std::nullopt, state.zoom);
        auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gtk_text_buffer_select_range(buffer, &end, &start);
        g_signal_emit_by_name(button, "clicked");
        const auto completion = textEditor->finish();
        updateTextToolbarVisibility();
        require(completion && completion->hasContent,
                "Floating text-format action ended the inline edit without its text");

        const auto run = std::find_if(
                completion->item.styleRuns.begin(), completion->item.styleRuns.end(),
                [](const prototype::TextStyleRun& candidate) { return candidate.start == 0 && candidate.end > 0; });
        require(run != completion->item.styleRuns.end(), "Floating text-format action produced no style run");

        bool applied = false;
        if (format == Format::Bold || format == Format::Italic) {
            auto* font = pango_font_description_from_string(run->fontName.c_str());
            applied = format == Format::Bold ? pango_font_description_get_weight(font) >= PANGO_WEIGHT_SEMIBOLD :
                                               pango_font_description_get_style(font) != PANGO_STYLE_NORMAL;
            pango_font_description_free(font);
        } else {
            const std::uint32_t attribute = format == Format::Underline     ? 1U :
                                            format == Format::Strikethrough ? 2U :
                                            format == Format::Superscript   ? 4U :
                                                                              8U;
            applied = (run->attributes & attribute) != 0;
        }
        require(applied, "Floating text-format action did not apply its registered format");
    }
    std::cout << "PASS: GTK4 floating toolbox rich-text formatting actions\n";
}

void Prototype::verifyFloatingToolboxTextAlignment() {
    const std::pair<const char*, int> alignments[] = {
            {"FORMAT_ALIGN_LEFT", 0},
            {"FORMAT_ALIGN_CENTER", 1},
            {"FORMAT_ALIGN_RIGHT", 2},
    };

    for (const auto& [token, alignment]: alignments) {
        auto* button = findWidgetByData(floatingToolboxRowsContainer, "gtk4-floating-text-layout", token);
        require(button && GTK_IS_BUTTON(button), "Configured floating text-alignment action did not render");

        prototype::TextItem draft;
        draft.text = "Floating toolbar alignment";
        draft.alignment = state.textAlignment;
        draft.justify = state.textJustify;
        textEditor->begin(std::move(draft), std::nullopt, state.zoom);
        g_signal_emit_by_name(button, "clicked");
        require(textEditor->item() && textEditor->item()->alignment == alignment,
                "Floating text-alignment action did not update the active draft");
        const auto completion = textEditor->finish();
        updateTextToolbarVisibility();
        require(completion && completion->item.alignment == alignment,
                "Floating text-alignment action was not retained in the text item");
    }

    auto* justifyButton = findWidgetByData(floatingToolboxRowsContainer, "gtk4-floating-text-layout", "FORMAT_JUSTIFY");
    require(justifyButton && GTK_IS_BUTTON(justifyButton), "Configured floating text-justify action did not render");
    prototype::TextItem draft;
    draft.text = "Floating toolbar justification";
    draft.alignment = state.textAlignment;
    draft.justify = state.textJustify;
    textEditor->begin(std::move(draft), std::nullopt, state.zoom);
    const bool expectedJustify = !state.textJustify;
    g_signal_emit_by_name(justifyButton, "clicked");
    const auto expectedLayout = expectedJustify          ? GTK_JUSTIFY_FILL :
                                state.textAlignment == 1 ? GTK_JUSTIFY_CENTER :
                                state.textAlignment == 2 ? GTK_JUSTIFY_RIGHT :
                                                           GTK_JUSTIFY_LEFT;
    require(textEditor->item() && textEditor->item()->justify == expectedJustify &&
                    gtk_text_view_get_justification(GTK_TEXT_VIEW(textEditor->widget())) == expectedLayout,
            "Floating text-justify action did not update the active draft and editor layout");
    const auto completion = textEditor->finish();
    updateTextToolbarVisibility();
    require(completion && completion->item.justify == expectedJustify,
            "Floating text-justify action was not retained in the text item");

    std::cout << "PASS: GTK4 floating toolbox text alignment and justification actions\n";
}

void Prototype::verifyPageLayoutIntegration() {
    require(pageLayoutMenuButton != nullptr, "Page layout menu button was not created");
    auto* menu = gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(pageLayoutMenuButton));
    require(menu && g_menu_model_get_n_items(menu) == 8, "Page layout menu is missing a layout or view-mode action");
    for (const char* action: {"paired-pages-mode", "set-layout-vertical", "set-layout-right-to-left",
                              "set-layout-bottom-to-top", "set-columns-or-rows", "paired-pages-offset"}) {
        require(g_action_map_lookup_action(G_ACTION_MAP(window), action) != nullptr,
                "Page layout menu action was not registered");
    }

    state.setPages({{260, 320}, {220, 280}, {280, 350}, {240, 300}});
    pageCount = 4;
    prototype::CanvasLayoutSettings settings;
    settings.columns = 1;
    settings.vertical = true;
    state.setLayoutSettings(settings);
    update();
    require(pageLabel && std::string(gtk_label_get_text(GTK_LABEL(pageLabel))) == "1 / 4",
            "Page layout test document did not update the visible page count");

    const auto activate = [this](const char* name, GVariant* parameter) {
        auto* action = g_action_map_lookup_action(G_ACTION_MAP(window), name);
        require(action != nullptr, "Page layout action disappeared during activation");
        g_action_activate(action, parameter);
    };
    activate("set-columns-or-rows", g_variant_new_int32(2));
    require(state.pageLayout.columnCount() == 2 && state.pageLayout.rowCount() == 2,
            "Selecting two vertical columns did not build a two-by-two page grid");
    activate("set-columns-or-rows", g_variant_new_int32(-2));
    require(state.layoutSettings.fixedRows && state.pageLayout.rowCount() == 2 && state.pageLayout.columnCount() == 2,
            "Selecting two fixed rows did not build the configured page grid");
    activate("set-columns-or-rows", g_variant_new_int32(2));

    activate("set-layout-vertical", g_variant_new_boolean(false));
    require(state.pageLayout.gridPosition(0).column == 0 && state.pageLayout.gridPosition(1).column == 1 &&
                    state.pageLayout.gridPosition(2).row == 1,
            "Horizontal flow did not fill columns before advancing rows");

    activate("set-layout-right-to-left", g_variant_new_boolean(true));
    require(state.pageLayout.gridPosition(0).column == 1,
            "Right-to-left reading did not reverse horizontal page order");
    activate("set-layout-bottom-to-top", g_variant_new_boolean(true));
    require(state.pageLayout.gridPosition(0).row == 1, "Bottom-to-top reading did not reverse vertical page order");
    activate("set-layout-right-to-left", g_variant_new_boolean(false));
    activate("set-layout-bottom-to-top", g_variant_new_boolean(false));

    activate("paired-pages-mode", nullptr);
    require(state.layoutSettings.pairedPages && state.pageLayout.gridPosition(0).column == 1 &&
                    state.pageLayout.gridPosition(1).row == 1 && state.pageLayout.gridPosition(2).column == 1,
            "Paired-page mode or its first-spread offset did not map pages correctly");
    activate("paired-pages-offset", g_variant_new_int32(0));
    require(state.pageLayout.gridPosition(0).column == 0 && state.pageLayout.gridPosition(1).column == 1,
            "Changing paired-page offset did not move the first spread");

    activate("paired-pages-mode", nullptr);
    activate("paired-pages-offset", g_variant_new_int32(1));
    activate("set-layout-vertical", g_variant_new_boolean(true));
    activate("set-columns-or-rows", g_variant_new_int32(2));
    require(state.pageLayout.gridPosition(0).column == 0 && state.pageLayout.gridPosition(1).row == 1 &&
                    state.pageLayout.gridPosition(2).column == 1,
            "Returning to the two-column layout did not restore page reading order");

    const auto point = state.pageScreenPoint(2, 100, 120);
    const auto hit = state.hitPage(point.x, point.y);
    require(hit && hit->page == 2 && std::abs(hit->local.x - 100) < 1e-6 && std::abs(hit->local.y - 120) < 1e-6,
            "Canvas input did not map a grid page hit back to page-local coordinates");

    prototype::ImportedSettings persisted;
    const auto loaded = prototype::loadGtk4Preferences(preferencesConfigDir, persisted);
    require(loaded.found && loaded.warning.empty() && persisted.pageLayout.columns == 2 &&
                    !persisted.pageLayout.fixedRows && persisted.pageLayout.vertical &&
                    !persisted.pageLayout.pairedPages && persisted.pageLayout.pairedPageOffset == 1,
            "Page layout actions were not persisted in GTK 4 preferences");

    update();
    capture("page-layout-grid");

    openPreferences();
    adw_preferences_dialog_set_visible_page_name(preferencesUi.dialog, "page-layout");
    const auto preferencesSettleAt = g_get_monotonic_time() + 250000;
    const auto preferencesReadyDeadline = preferencesSettleAt + 500000;
    while ((!gtk_widget_get_mapped(GTK_WIDGET(preferencesUi.dialog)) ||
            gtk_widget_get_width(GTK_WIDGET(preferencesUi.dialog)) <= 0 ||
            gtk_widget_get_height(GTK_WIDGET(preferencesUi.dialog)) <= 0 ||
            g_get_monotonic_time() < preferencesSettleAt) &&
           g_get_monotonic_time() < preferencesReadyDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(gtk_widget_get_mapped(GTK_WIDGET(preferencesUi.dialog)) &&
                    gtk_widget_get_width(GTK_WIDGET(preferencesUi.dialog)) > 0 &&
                    gtk_widget_get_height(GTK_WIDGET(preferencesUi.dialog)) > 0,
            "Page Layout Preferences did not map for the private GUI check");
    auto* flowControl = preferencesUi.controls[static_cast<size_t>(PreferenceKey::LayoutFlow)];
    auto* dimensionControl = preferencesUi.controls[static_cast<size_t>(PreferenceKey::LayoutGridDimension)];
    auto* countControl = preferencesUi.controls[static_cast<size_t>(PreferenceKey::LayoutGridCount)];
    auto* pairedControl = preferencesUi.controls[static_cast<size_t>(PreferenceKey::LayoutPairedPages)];
    auto* offsetControl = preferencesUi.controls[static_cast<size_t>(PreferenceKey::LayoutPairedPageOffset)];
    auto* horizontalDirectionControl =
            preferencesUi.controls[static_cast<size_t>(PreferenceKey::LayoutHorizontalDirection)];
    auto* verticalDirectionControl = preferencesUi.controls[static_cast<size_t>(PreferenceKey::LayoutVerticalDirection)];
    require(flowControl && dimensionControl && countControl && pairedControl && offsetControl &&
                    horizontalDirectionControl && verticalDirectionControl,
            "Page Layout Preferences is missing a layout, grid, offset, or reading-direction control");

    adw_combo_row_set_selected(ADW_COMBO_ROW(dimensionControl), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(countControl), 3);
    adw_combo_row_set_selected(ADW_COMBO_ROW(flowControl), 1);
    adw_switch_row_set_active(ADW_SWITCH_ROW(pairedControl), true);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(offsetControl), 1);
    adw_combo_row_set_selected(ADW_COMBO_ROW(horizontalDirectionControl), 1);
    adw_combo_row_set_selected(ADW_COMBO_ROW(verticalDirectionControl), 1);
    require(state.layoutSettings.fixedRows && state.layoutSettings.rows == 3 && !state.layoutSettings.vertical &&
                    state.layoutSettings.pairedPages && state.layoutSettings.pairedPageOffset == 1 &&
                    state.layoutSettings.rightToLeft && state.layoutSettings.bottomToTop &&
                    state.pageLayout.rowCount() == 3 && state.pageLayout.gridPosition(0).column == 0 &&
                    state.pageLayout.gridPosition(0).row == 2,
            "Page Layout Preferences did not apply the grid, paired offset, and reading directions");

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(offsetControl), 37);
    require(state.layoutSettings.pairedPageOffset == 37,
            "Page Layout Preferences did not retain the full legacy paired-page offset range");
    auto* offsetAction = g_action_map_lookup_action(G_ACTION_MAP(window), "paired-pages-offset");
    auto* gridAction = g_action_map_lookup_action(G_ACTION_MAP(window), "set-columns-or-rows");
    require(offsetAction && gridAction, "Page Layout Preferences lost its synchronized menu actions");
    auto* offsetActionState = g_action_get_state(offsetAction);
    auto* gridActionState = g_action_get_state(gridAction);
    require(offsetActionState && g_variant_get_int32(offsetActionState) == 37 && gridActionState &&
                    g_variant_get_int32(gridActionState) == -3,
            "Page Layout Preferences did not synchronize the menu's offset and row-count actions");
    g_clear_pointer(&offsetActionState, g_variant_unref);
    g_clear_pointer(&gridActionState, g_variant_unref);

    prototype::ImportedSettings preferenceRoundTrip;
    const auto preferenceRoundTripResult = prototype::loadGtk4Preferences(preferencesConfigDir, preferenceRoundTrip);
    require(preferenceRoundTripResult.found && preferenceRoundTripResult.warning.empty() &&
                    preferenceRoundTrip.pageLayout.fixedRows && preferenceRoundTrip.pageLayout.rows == 3 &&
                    !preferenceRoundTrip.pageLayout.vertical && preferenceRoundTrip.pageLayout.pairedPages &&
                    preferenceRoundTrip.pageLayout.pairedPageOffset == 37 &&
                    preferenceRoundTrip.pageLayout.rightToLeft && preferenceRoundTrip.pageLayout.bottomToTop,
            "Page Layout Preferences did not persist all configured layout values");
    capture("page-layout-preferences", GTK_WIDGET(preferencesUi.dialog), true);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(offsetControl), 1);
    adw_combo_row_set_selected(ADW_COMBO_ROW(horizontalDirectionControl), 0);
    adw_combo_row_set_selected(ADW_COMBO_ROW(verticalDirectionControl), 0);
    adw_switch_row_set_active(ADW_SWITCH_ROW(pairedControl), false);
    adw_combo_row_set_selected(ADW_COMBO_ROW(flowControl), 0);
    adw_combo_row_set_selected(ADW_COMBO_ROW(dimensionControl), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(countControl), 2);
    adw_dialog_close(ADW_DIALOG(preferencesUi.dialog));

    gtk_menu_button_popup(GTK_MENU_BUTTON(pageLayoutMenuButton));
    auto* popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(pageLayoutMenuButton));
    require(popover && gtk_widget_get_visible(GTK_WIDGET(popover)), "Page layout menu did not open");
    capture("page-layout-menu", GTK_WIDGET(popover), true);
    gtk_menu_button_popdown(GTK_MENU_BUTTON(pageLayoutMenuButton));
    std::cout << "PASS: GTK4 page layout actions, Preferences, grid geometry, navigation and persistence\n";
}

void Prototype::verifyPresentationModeIntegration() {
    require(pageLayoutMenuButton && headerBarWidget && toolbarWidget && fitPageButton && splitView,
            "Presentation Mode is missing its view controls");
    auto* menu = gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(pageLayoutMenuButton));
    require(menu && g_menu_model_get_n_items(menu) == 8,
            "View menu is missing Presentation Mode or fullscreen actions");
    for (const char* name: {"presentation-mode", "fullscreen", "paired-pages-mode", "set-layout-vertical"}) {
        require(g_action_map_lookup_action(G_ACTION_MAP(window), name),
                "A Presentation Mode or layout action was not registered");
    }

    state.setPages({{420, 600}, {900, 450}, {350, 800}});
    pageCount = 3;
    auto layout = importedConfig.settings.pageLayout;
    layout.presentationMode = false;
    layout.columns = 2;
    layout.fixedRows = false;
    layout.vertical = false;
    layout.pairedPages = false;
    layout.rightToLeft = false;
    layout.bottomToTop = false;
    importedConfig.settings.pageLayout = layout;
    state.setLayoutSettings(layout);
    update();

    const auto activate = [this](const char* name, GVariant* parameter) {
        auto* action = g_action_map_lookup_action(G_ACTION_MAP(window), name);
        require(action != nullptr, "A view-mode action disappeared during verification");
        g_action_activate(action, parameter);
    };
    const auto waitForUiState = [this](auto condition, const char* message) {
        const auto settleAt = g_get_monotonic_time() + 250000;
        const auto deadline = settleAt + 750000;
        while ((!condition() || g_get_monotonic_time() < settleAt) && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(condition(), message);
    };
    const auto requireCurrentPageFit = [this] {
        const auto size = state.pageSize(state.page);
        const double width = gtk_widget_get_width(canvas);
        const double height = gtk_widget_get_height(canvas);
        const double expected =
                std::clamp(std::min(width / (size.width + 14.0), height / (size.height + 14.0)), 0.1, 8.0);
        require(width > 0 && height > 0 && std::abs(state.zoom - expected) < 1e-8,
                "Presentation zoom did not fit the active page with the upstream margin");
        const auto center = state.pageScreenPoint(state.page, size.width / 2.0, size.height / 2.0);
        require(std::abs(center.x - width / 2.0) < 1e-7 && std::abs(center.y - height / 2.0) < 1e-7,
                "Presentation Mode did not center the active page");
    };

    activate("presentation-mode", g_variant_new_boolean(true));
    waitForUiState([this] { return gtk_window_is_fullscreen(GTK_WINDOW(window)); },
                   "Presentation Mode did not enter GTK fullscreen");
    require(presentationMode && state.layoutSettings.presentationMode && state.pageLayout.columnCount() == 1 &&
                    state.pageLayout.rowCount() == 3 &&
                    !g_action_get_enabled(g_action_map_lookup_action(G_ACTION_MAP(window), "set-layout-vertical")) &&
                    !gtk_widget_get_sensitive(fitPageButton),
            "Presentation Mode did not switch to one page per row or disable layout and zoom controls");
    require(windowFullscreen && !gtk_widget_get_visible(headerBarWidget) && gtk_widget_get_visible(toolbarWidget) &&
                    gtk_widget_get_visible(mainToolRailWidget) && !adw_overlay_split_view_get_show_sidebar(splitView),
            "Default Presentation Mode view visibility or fullscreen settings were not applied");
    requireCurrentPageFit();
    capture("presentation-mode");

    const double lockedZoom = state.zoom;
    zoom(lockedZoom * 1.5, gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0);
    std::string zoomError;
    require(state.zoom == lockedZoom && !setPluginZoom(lockedZoom * 1.25, zoomError) && !zoomError.empty(),
            "Presentation Mode allowed a user or plugin zoom change");

    navigateToPage(1);
    require(state.page == 1, "Presentation navigation did not select the next page");
    requireCurrentPageFit();
    navigateToPage(2);
    requireCurrentPageFit();
    gboolean handled = false;
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Up, 0U, GdkModifierType(0), &handled);
    require(handled && state.page == 1, "Up did not navigate to the previous page in Presentation Mode");
    requireCurrentPageFit();
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Down, 0U, GdkModifierType(0), &handled);
    require(handled && state.page == 2, "Down did not navigate to the next page in Presentation Mode");
    requireCurrentPageFit();

    openPreferences();
    adw_preferences_dialog_set_visible_page_name(preferencesUi.dialog, "appearance");
    const auto preferencesSettleAt = g_get_monotonic_time() + 250000;
    const auto preferencesReadyDeadline = preferencesSettleAt + 500000;
    while ((!gtk_widget_get_mapped(GTK_WIDGET(preferencesUi.dialog)) ||
            gtk_widget_get_width(GTK_WIDGET(preferencesUi.dialog)) <= 0 ||
            gtk_widget_get_height(GTK_WIDGET(preferencesUi.dialog)) <= 0 ||
            g_get_monotonic_time() < preferencesSettleAt) &&
           g_get_monotonic_time() < preferencesReadyDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(gtk_widget_get_mapped(GTK_WIDGET(preferencesUi.dialog)) &&
                    gtk_widget_get_width(GTK_WIDGET(preferencesUi.dialog)) > 0 &&
                    gtk_widget_get_height(GTK_WIDGET(preferencesUi.dialog)) > 0,
            "Appearance Preferences did not map for the private GUI check");
    for (const auto key:
         {PreferenceKey::PresentationGoFullscreen, PreferenceKey::PresentationMenubar,
          PreferenceKey::PresentationToolbar, PreferenceKey::PresentationSidebar, PreferenceKey::FullscreenMenubar,
          PreferenceKey::FullscreenToolbar, PreferenceKey::FullscreenSidebar}) {
        require(preferencesUi.controls[static_cast<std::size_t>(key)] != nullptr,
                "Appearance Preferences is missing a fullscreen or Presentation Mode visibility control");
    }
    capture("presentation-preferences", GTK_WIDGET(preferencesUi.dialog), true);
    auto* goFullscreen =
            ADW_SWITCH_ROW(preferencesUi.controls[static_cast<std::size_t>(PreferenceKey::PresentationGoFullscreen)]);
    auto* showHeader = ADW_SWITCH_ROW(preferencesUi.controls[static_cast<std::size_t>(PreferenceKey::PresentationMenubar)]);
    auto* showToolbar =
            ADW_SWITCH_ROW(preferencesUi.controls[static_cast<std::size_t>(PreferenceKey::PresentationToolbar)]);
    auto* showSidebar =
            ADW_SWITCH_ROW(preferencesUi.controls[static_cast<std::size_t>(PreferenceKey::PresentationSidebar)]);
    adw_switch_row_set_active(goFullscreen, false);
    adw_switch_row_set_active(showHeader, true);
    adw_switch_row_set_active(showToolbar, false);
    adw_switch_row_set_active(showSidebar, true);
    waitForUiState([this] { return !gtk_window_is_fullscreen(GTK_WINDOW(window)); },
                   "Presentation view Preferences did not leave GTK fullscreen");
    require(!windowFullscreen && gtk_widget_get_visible(headerBarWidget) && !gtk_widget_get_visible(toolbarWidget) &&
                    !gtk_widget_get_visible(mainToolRailWidget) && adw_overlay_split_view_get_show_sidebar(splitView),
            "Presentation view visibility Preferences did not apply while the mode was active");
    adw_switch_row_set_active(goFullscreen, true);
    adw_switch_row_set_active(showHeader, false);
    adw_switch_row_set_active(showToolbar, true);
    adw_switch_row_set_active(showSidebar, false);
    waitForUiState([this] { return gtk_window_is_fullscreen(GTK_WINDOW(window)); },
                   "Presentation view Preferences did not restore GTK fullscreen");
    require(windowFullscreen && !gtk_widget_get_visible(headerBarWidget) && gtk_widget_get_visible(toolbarWidget) &&
                    !adw_overlay_split_view_get_show_sidebar(splitView),
            "Restoring Presentation Mode view settings did not update the active window");
    adw_dialog_close(ADW_DIALOG(preferencesUi.dialog));
    waitForUiState([this] { return !gtk_widget_get_mapped(GTK_WIDGET(preferencesUi.dialog)); },
                   "Appearance Preferences did not close before the mode-exit screenshot");

    prototype::ImportedSettings persisted;
    const auto persistedResult = prototype::loadGtk4Preferences(preferencesConfigDir, persisted);
    require(persistedResult.found && persistedResult.warning.empty() && persisted.pageLayout.presentationMode &&
                    persisted.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)]
                            .goFullscreen &&
                    !persisted.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)]
                             .showMenubar &&
                    persisted.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)]
                            .showToolbar &&
                    !persisted.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].showSidebar,
            "Presentation Mode and its view settings were not persisted");

    activate("presentation-mode", g_variant_new_boolean(false));
    waitForUiState([this] { return !gtk_window_is_fullscreen(GTK_WINDOW(window)); },
                   "Leaving Presentation Mode did not leave GTK fullscreen");
    require(!presentationMode && !state.layoutSettings.presentationMode &&
                    state.layoutSettings.columns == layout.columns && !state.layoutSettings.vertical &&
                    state.pageLayout.columnCount() == 2 && !windowFullscreen &&
                    gtk_widget_get_visible(headerBarWidget) && gtk_widget_get_visible(toolbarWidget) &&
                    !adw_overlay_split_view_get_show_sidebar(splitView),
            "Leaving Presentation Mode did not restore the saved layout and default view");

    activate("fullscreen", g_variant_new_boolean(true));
    waitForUiState([this] { return gtk_window_is_fullscreen(GTK_WINDOW(window)); },
                   "Fullscreen action did not enter GTK fullscreen");
    require(fullscreenModeActive && windowFullscreen && !gtk_widget_get_visible(headerBarWidget) &&
                    gtk_widget_get_visible(toolbarWidget) && adw_overlay_split_view_get_show_sidebar(splitView),
            "Fullscreen action did not apply the fullscreen view preset");
    activate("fullscreen", g_variant_new_boolean(false));
    waitForUiState([this] { return !gtk_window_is_fullscreen(GTK_WINDOW(window)); },
                   "Leaving fullscreen did not leave GTK fullscreen");
    require(!fullscreenModeActive && !windowFullscreen && gtk_widget_get_visible(headerBarWidget) &&
                    !adw_overlay_split_view_get_show_sidebar(splitView),
            "Leaving fullscreen did not restore the default view preset");
    capture("presentation-mode-exit");
    std::cout << "PASS: GTK4 Presentation Mode per-page fit, navigation, zoom lock, fullscreen and view Preferences\n";
}

void Prototype::verifySidebarPerformanceIntegration() {
    require(pageCount == 10000 && document && splitView && pageList,
            "Sidebar performance check did not load its 10000-page PDF");
    const auto pageRange = [this] {
        int first = pageCount;
        int last = -1;
        for (auto* thumbnail: thumbnailWidgets) {
            const auto page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(thumbnail), "page"));
            first = std::min(first, page);
            last = std::max(last, page);
        }
        return std::pair{first, last};
    };
    const auto mappedRows = [this] {
        return static_cast<size_t>(std::count_if(thumbnailWidgets.begin(), thumbnailWidgets.end(),
                                                 [](GtkWidget* widget) { return gtk_widget_get_mapped(widget); }));
    };
    const auto started = g_get_monotonic_time();
    adw_overlay_split_view_set_show_sidebar(splitView, true);
    waitForNextWindowFrame();
    const auto openMilliseconds = (g_get_monotonic_time() - started) / 1000;
    const auto waitForThumbnail = [this](int page) {
        const auto deadline = g_get_monotonic_time() + 5000000;
        while (!thumbnailRenderer.find(page) && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(thumbnailRenderer.find(page) != nullptr,
                "An asynchronous page raster did not finish within five seconds");
    };
    waitForThumbnail(0);
    const auto initialThumbnailsDeadline = g_get_monotonic_time() + 5000000;
    while (thumbnailRenderer.pendingCount() > 0 && g_get_monotonic_time() < initialThumbnailsDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(thumbnailRenderer.pendingCount() == 0, "The initial visible page rasters did not drain their render queue");
    const auto maxMappedRows = static_cast<size_t>(gtk_widget_get_height(GTK_WIDGET(pageList)) / 180 + 3);
    const auto [firstPage, lastPage] = pageRange();
    std::cout << "SIDEBAR OPEN: pages=" << pageCount << " realized=" << thumbnailWidgets.size()
              << " mapped=" << mappedRows() << " rows=" << firstPage << ".." << lastPage
              << " elapsed_ms=" << openMilliseconds << '\n';
    require(thumbnailWidgets.size() <= 256 && mappedRows() <= maxMappedRows,
            "Pages sidebar created or mapped too many thumbnail widgets");
    for (int frame = 0; frame < 18; ++frame) {
        waitForNextWindowFrame();
    }
    capture("large-pdf-sidebar");

    auto* pageScroll = GTK_SCROLLED_WINDOW(gtk_widget_get_parent(GTK_WIDGET(pageList)));
    auto* adjustment = gtk_scrolled_window_get_vadjustment(pageScroll);
    const double lower = gtk_adjustment_get_lower(adjustment);
    const double scrollable = gtk_adjustment_get_upper(adjustment) - lower - gtk_adjustment_get_page_size(adjustment);
    double maxFrameMilliseconds = 0;
    size_t maxPendingCount = 0;
    require(scrollable > 0, "The 10000-page sidebar has no scrollable range");

    thumbnailRenderer.invalidate();
    sidebarCanvasRenderSuppressed = true;
    canvasRenderer.invalidate();
    gtk_adjustment_set_value(adjustment, lower + scrollable / 2.0);
    gtk_widget_queue_draw(GTK_WIDGET(pageList));
    waitForNextWindowFrame();
    prioritizeVisibleSidebarThumbnails();
    const int preferredSidebarPage = thumbnailRenderer.preferredVisiblePage();
    require(preferredSidebarPage >= 0 && preferredSidebarPage != state.page,
            "Independent sidebar scrolling did not choose a page away from the canvas page");
    const auto dispatchDeadline = g_get_monotonic_time() + 1000000;
    while (thumbnailRenderer.firstDispatchedPage() < 0 && g_get_monotonic_time() < dispatchDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(thumbnailRenderer.firstDispatchedPage() == preferredSidebarPage,
            "The sidebar scheduler did not dispatch the nearest visible page first");
    waitForThumbnail(preferredSidebarPage);
    require(canvasRenderer.empty(), "page rasters depended on the main canvas PDF cache");
    const size_t cancellationsBeforeScroll = thumbnailRenderer.offscreenCancellations();
    const int offscreenPage = pageCount + 1;
    thumbnailRenderer.setVisiblePages({{offscreenPage, 0}}, -1);
    PageRasterRequest offscreenRequest;
    offscreenRequest.page = offscreenPage;
    offscreenRequest.pageSize = {595, 842};
    thumbnailRenderer.request(std::move(offscreenRequest));
    thumbnailRenderer.setVisiblePages({{offscreenPage + 1, 0}}, -1);
    require(thumbnailRenderer.offscreenCancellations() > cancellationsBeforeScroll &&
                    thumbnailRenderer.pendingCount() == 0,
            "The sidebar scheduler retained work after its pages left the viewport");
    const auto cancelledTasksDeadline = g_get_monotonic_time() + 5000000;
    while (thumbnailRenderer.activeCount() > 0 && g_get_monotonic_time() < cancelledTasksDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(thumbnailRenderer.activeCount() == 0,
            "Offscreen thumbnail renderer processes did not stop after cancellation");
    std::cout << "SIDEBAR INDEPENDENT: canvas_page=" << state.page << " thumbnail_page=" << preferredSidebarPage
              << " first_dispatch=" << thumbnailRenderer.firstDispatchedPage()
              << " offscreen_cancelled=" << thumbnailRenderer.offscreenCancellations() - cancellationsBeforeScroll
              << '\n';
    sidebarCanvasRenderSuppressed = false;
    update();
    waitForNextWindowFrame();

    for (int frame = 1; frame <= 32; ++frame) {
        gtk_adjustment_set_value(adjustment, lower + scrollable * frame / 32.0);
        const auto frameStarted = g_get_monotonic_time();
        waitForNextWindowFrame();
        const double frameMilliseconds = (g_get_monotonic_time() - frameStarted) / 1000.0;
        maxFrameMilliseconds = std::max(maxFrameMilliseconds, frameMilliseconds);
        maxPendingCount = std::max(maxPendingCount, thumbnailRenderer.pendingCount());
        require(frameMilliseconds < 500 && maxPendingCount <= 6,
                "Rapid sidebar scrolling blocked the GTK frame or queued unbounded thumbnail work");
    }
    waitForThumbnail(pageCount - 1);
    std::cout << "SIDEBAR FAST SCROLL: frames=32 max_pending=" << maxPendingCount
              << " max_frame_ms=" << maxFrameMilliseconds << '\n';
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
    waitForNextWindowFrame();
    const auto [lastFirst, lastLast] = pageRange();
    std::cout << "SIDEBAR BOTTOM: realized=" << thumbnailWidgets.size() << " mapped=" << mappedRows()
              << " rows=" << lastFirst << ".." << lastLast << '\n';
    require(lastLast >= pageCount - 10 && thumbnailWidgets.size() <= 256 && mappedRows() <= maxMappedRows,
            "Pages sidebar did not virtualize rows when scrolling to the last page");
    gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(gtk_list_view_get_model(pageList)), pageCount - 1);
    waitForNextWindowFrame();
    require(state.page == pageCount - 1, "Selecting the last virtualized sidebar page did not navigate");
    openGotoPageDialog();
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(gotoPageSpin), 1);
    answerDialog("go");
    waitForNextWindowFrame();
    require(state.page == 0 &&
                    gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(gtk_list_view_get_model(pageList))) == 0 &&
                    gtk_adjustment_get_value(adjustment) <= gtk_adjustment_get_lower(adjustment) + 1.0,
            "Go to Page did not synchronize the sidebar selection and scroll position");
    waitForThumbnail(0);
    require(waitForCanvasRaster(state.page, canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))),
            "The canvas PDF renderer did not finish the current page");
    const double anchorX = gtk_widget_get_width(canvas) / 2.0;
    const double anchorY = gtk_widget_get_height(canvas) / 2.0;
    const auto documentAnchor = state.documentPoint(anchorX, anchorY);
    const double zoomBefore = state.zoom;
    const double zoomTarget = prototype::behavior::zoomFromScrollDelta(zoomBefore, -8, false, state.scrollZoomStep);
    const size_t cachedRasters = canvasRenderer.completedRequests();
    zoom(zoomTarget, anchorX, anchorY, true);
    waitForNextWindowFrame();
    const auto movedAnchor = state.documentPoint(anchorX, anchorY);
    require(canvasRenderer.completedRequests() == cachedRasters && std::abs(state.zoom - zoomTarget) < 1e-9 &&
                    state.zoom > zoomBefore && std::abs(movedAnchor.x - documentAnchor.x) < 1e-9 &&
                    std::abs(movedAnchor.y - documentAnchor.y) < 1e-9,
            "Smooth zoom did not reuse the current raster or preserve the pointer anchor");
    finishZoomPreview();
    update();
    waitForNextWindowFrame();
    require(waitForCanvasRaster(state.page, canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))) &&
                    canvasRenderer.completedRequests() > cachedRasters,
            "Settled smooth zoom did not refresh the page raster");
    std::cout << "PASS: GTK4 pages sidebar realizes a bounded number of thumbnails for 10000 PDF pages\n";
}

void Prototype::verifySidebarStartupIntegration() {
    require(document && pageList && splitView && pageCount > 0, "Sidebar startup check did not load its PDF");
    require(adw_overlay_split_view_get_show_sidebar(splitView),
            "Sidebar startup check did not start with the Pages sidebar open");
    const auto deadline = g_get_monotonic_time() + 6000000;
    while ((!thumbnailRenderer.find(0) || sidebarStartupReadyDraws == 0) && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(thumbnailRenderer.find(0) && sidebarStartupReadyDraws > 0,
            "The first page raster did not render and redraw without input");
    require(gtk_widget_get_width(GTK_WIDGET(pageList)) >= maxPageThumbnailWidth,
            "The Pages sidebar did not finish opening for the no-input startup check");
    const auto& preloadSettings = importedConfig.settings;
    const int expectedPreloads = std::min<int>(preloadSettings.preloadPagesBefore, state.page) +
                                 std::min<int>(preloadSettings.preloadPagesAfter, pageCount - state.page - 1);
    const auto preloadDeadline = g_get_monotonic_time() + 1000000;
    while (static_cast<int>(canvasPreloadedPages.size()) != expectedPreloads &&
           g_get_monotonic_time() < preloadDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(static_cast<int>(canvasPreloadedPages.size()) == expectedPreloads &&
                    !canvasPreloadedPages.contains(state.page),
            "The configured page-preload offsets were not scheduled around the active page");
    if (!canvasPreloadedPages.empty()) {
        const int preloadPage = canvasPreloadedPages.begin()->first;
        const auto rasterDeadline = g_get_monotonic_time() + 10000000;
        while (!canvasRenderer.find(preloadPage) && g_get_monotonic_time() < rasterDeadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(canvasRenderer.find(preloadPage),
                "A page selected by the preload offsets did not receive a cached raster");
    }
    require(waitForCanvasRaster(state.page, canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))),
            "The main canvas did not render the content-heavy PDF without input");
    waitForNextWindowFrame();
    capture("sidebar-startup-no-input");
    std::cout << "PASS: GTK4 sidebar renders its first visible thumbnail without input\n";

    const int canvasPage = state.page;
    require(pageCount > 36, "The content-heavy sidebar PDF needs at least 37 pages");
    const int targetPage = 24;
    auto* pageScroll = GTK_SCROLLED_WINDOW(gtk_widget_get_parent(GTK_WIDGET(pageList)));
    auto* adjustment = gtk_scrolled_window_get_vadjustment(pageScroll);
    const double lower = gtk_adjustment_get_lower(adjustment);
    const double scrollable = gtk_adjustment_get_upper(adjustment) - lower - gtk_adjustment_get_page_size(adjustment);
    require(scrollable > 0, "The content-heavy sidebar PDF has no range for the fast-scroll check");
    const std::array<int, 3> scrollStops{targetPage, targetPage + 12, targetPage};
    for (const int stopPage: scrollStops) {
        const double targetValue = lower + scrollable * stopPage / (pageCount - 1.0);
        for (int frame = 1; frame <= 4; ++frame) {
            gtk_adjustment_set_value(adjustment, lower + (targetValue - lower) * frame / 4.0);
            waitForNextWindowFrame();
        }
    }
    const auto mappedVisiblePages = [this] {
        std::vector<int> pages;
        const int viewportHeight = gtk_widget_get_height(GTK_WIDGET(pageList));
        for (auto* thumbnail: thumbnailWidgets) {
            const int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(thumbnail), "page"));
            graphene_rect_t bounds{};
            if (page >= 0 && gtk_widget_get_mapped(thumbnail) &&
                gtk_widget_compute_bounds(thumbnail, GTK_WIDGET(pageList), &bounds) &&
                bounds.origin.y + bounds.size.height > 0 && bounds.origin.y < viewportHeight) {
                pages.push_back(page);
            }
        }
        std::sort(pages.begin(), pages.end());
        return pages;
    };
    std::vector<int> visiblePages;
    const auto stopDeadline = g_get_monotonic_time() + 10000000;
    const auto hasRenderedInk = [this](int page) {
        const auto* preview = thumbnailRenderer.find(page);
        if (!preview || !preview->background ||
            cairo_surface_get_type(preview->background.get()) != CAIRO_SURFACE_TYPE_IMAGE) {
            return false;
        }
        auto* surface = preview->background.get();
        cairo_surface_flush(surface);
        const auto* pixels = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        const int surfaceHeight = cairo_image_surface_get_height(surface);
        const int surfaceWidth = cairo_image_surface_get_width(surface);
        for (int y = 0; y < surfaceHeight; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(pixels + y * stride);
            for (int x = 0; x < surfaceWidth; ++x) {
                if ((row[x] & 0x00ffffffU) < 0x00f0f0f0U) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto visibleThumbnailsReady = [this, &visiblePages, &mappedVisiblePages, &hasRenderedInk, targetPage] {
        visiblePages = mappedVisiblePages();
        return std::find(visiblePages.begin(), visiblePages.end(), targetPage) != visiblePages.end() &&
               std::all_of(visiblePages.begin(), visiblePages.end(),
                           [this](int page) { return thumbnailRenderer.find(page) != nullptr; }) &&
               hasRenderedInk(targetPage) && hasRenderedInk(targetPage + 1);
    };
    while (!visibleThumbnailsReady() && g_get_monotonic_time() < stopDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    const bool targetPageVisible =
            std::find(visiblePages.begin(), visiblePages.end(), targetPage) != visiblePages.end();
    const bool allVisibleThumbnailsReady = std::all_of(visiblePages.begin(), visiblePages.end(), [this](int page) {
        return thumbnailRenderer.find(page) != nullptr;
    });
    const bool knownContentPagesReady = hasRenderedInk(targetPage) && hasRenderedInk(targetPage + 1);
    if (state.page != canvasPage || !targetPageVisible || !allVisibleThumbnailsReady || !knownContentPagesReady) {
        std::cerr << "SIDEBAR FAST STOP FAILED: initial_canvas_page=" << canvasPage + 1
                  << " canvas_page=" << state.page + 1 << " target_visible=" << targetPageVisible
                  << " content_pages_ready=" << knownContentPagesReady << " visible=";
        for (const int page: visiblePages) {
            const char* status =
                    thumbnailRenderer.find(page) ?
                            ((page == targetPage || page == targetPage + 1) && !hasRenderedInk(page) ? "blank-content" :
                                                                                                       "ready") :
                    thumbnailRenderer.hasPending(page) ? "pending" :
                    thumbnailRenderer.hasFailed(page)  ? "failed" :
                                                         "missing";
            std::cerr << page + 1 << ':' << status << ',';
        }
        std::cerr << " pending_total=" << thumbnailRenderer.pendingCount()
                  << " active=" << thumbnailRenderer.activeCount() << " next=" << thumbnailRenderer.nextQueuedPage()
                  << '\n';
    }
    require(state.page == canvasPage && targetPageVisible && allVisibleThumbnailsReady && knownContentPagesReady,
            "Visible page rasters did not all finish after fast scrolling stopped");
    capture("sidebar-fast-scroll-stop");
    std::cout << "SIDEBAR FAST STOP: canvas_page=" << state.page << " visible_pages=";
    for (const int page: visiblePages) {
        std::cout << page + 1 << ',';
    }
    std::cout << " rendered=" << visiblePages.size() << '\n';
    std::cout << "PASS: GTK4 sidebar renders visible thumbnails after fast scrolling stops\n";

    const int canvasWidth = gtk_widget_get_width(canvas);
    const int canvasHeight = gtk_widget_get_height(canvas);
    navigateToPage(pageCount / 2);
    require(waitForCanvasRaster(state.page, canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))),
            "The canvas did not render the middle page before zooming out");
    const size_t fullRendersBeforeZoomOut = canvasRenderer.completedFullRequests();
    zoom(0.1, canvasWidth / 2.0, canvasHeight / 2.0, true);
    trackPointerPosition(canvasWidth / 2.0, canvasHeight / 2.0);
    finishZoomPreview();
    update();
    std::vector<int> zoomedOutPages;
    const auto zoomOutDeadline = g_get_monotonic_time() + 20000000;
    const auto visibleCanvasPagesReady = [this, &zoomedOutPages, canvasWidth, canvasHeight] {
        zoomedOutPages = state.visiblePageIndices(canvasWidth, canvasHeight);
        std::map<int, double> visiblePagePriorities;
        for (const int page: zoomedOutPages) {
            const auto size = state.pageSize(page);
            const double centerY = state.offsetY + (state.pageTop(page) + size.height / 2.0) * state.zoom;
            visiblePagePriorities.emplace(page, std::abs(centerY - canvasHeight / 2.0));
        }
        if (visiblePagePriorities.empty()) {
            visiblePagePriorities.emplace(state.page, 0);
        }
        const int detailPage = canvasDetailPage(visiblePagePriorities);
        const bool useMediumDetail = canvasUsesMediumDetail();
        return !zoomedOutPages.empty() &&
               std::all_of(zoomedOutPages.begin(), zoomedOutPages.end(),
                           [this, detailPage, useMediumDetail](int page) {
                               return page == detailPage ?
                                              canvasRenderer.matches(canvasRenderRequest(
                                                      page, canvasDetailRenderScale(
                                                                    page, gtk_widget_get_scale_factor(canvas), true))) :
                                      useMediumDetail ? canvasRenderer.matches(canvasMediumDetailRequest(page)) :
                                                        canvasRenderer.matches(canvasLodRequest(page));
                           }) &&
               canvasRenderer.matches(canvasMediumDetailRequest(detailPage)) &&
               canvasRenderer.matches(canvasLodRequest(detailPage));
    };
    while (!visibleCanvasPagesReady() && g_get_monotonic_time() < zoomOutDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(visibleCanvasPagesReady(), "Zooming out left pages without full-, medium-, and low-detail rasters");
    require(canvasRenderer.completedFullRequests() - fullRendersBeforeZoomOut <= 1,
            "Zooming out rendered full-resolution PDF rasters for multiple pages");
    const auto& renderingSettings = importedConfig.settings;
    constexpr size_t bytesPerMegabyte = 1024 * 1024;
    require(canvasRenderer.fullDetailCachePages() <= static_cast<size_t>(renderingSettings.fullDetailCachePages) &&
                    canvasRenderer.mediumDetailCachePages() <=
                            static_cast<size_t>(renderingSettings.mediumDetailCachePages) &&
                    canvasRenderer.mediumDetailCacheBytes() <=
                            static_cast<size_t>(renderingSettings.mediumDetailCacheMegabytes) * bytesPerMegabyte &&
                    canvasRenderer.lowDetailCachePages() <=
                            static_cast<size_t>(renderingSettings.lowDetailCachePages) &&
                    canvasRenderer.lowDetailCacheBytes() <=
                            static_cast<size_t>(renderingSettings.lowDetailCacheMegabytes) * bytesPerMegabyte,
            "Canvas raster caches exceeded their page or byte budgets");
    capture("canvas-zoom-out-settled");
    std::cout << "CANVAS ZOOM OUT SETTLED: visible_pages=" << zoomedOutPages.size()
              << " full_pages=" << canvasRenderer.fullDetailCachePages()
              << " medium_pages=" << canvasRenderer.mediumDetailCachePages()
              << " medium_bytes=" << canvasRenderer.mediumDetailCacheBytes()
              << " low_pages=" << canvasRenderer.lowDetailCachePages()
              << " low_bytes=" << canvasRenderer.lowDetailCacheBytes() << " cache_bytes=" << canvasRenderer.cacheBytes()
              << '\n';
    require(zoomedOutPages.size() > static_cast<size_t>(renderingSettings.fullDetailCachePages),
            "Zoom-out cache check did not expose more pages than the configured full-detail cache");

    const double mediumDetailTestZoom = renderingSettings.overviewZoomThresholdPercent / 100.0 * 0.75;
    zoom(mediumDetailTestZoom, canvasWidth / 2.0, canvasHeight / 2.0, true);
    trackPointerPosition(canvasWidth / 2.0, canvasHeight / 2.0);
    finishZoomPreview();
    update();
    const auto mediumDeadline = g_get_monotonic_time() + 20000000;
    const auto visibleMediumPagesReady = [this, &zoomedOutPages, canvasWidth, canvasHeight] {
        zoomedOutPages = state.visiblePageIndices(canvasWidth, canvasHeight);
        std::map<int, double> priorities;
        for (const int page: zoomedOutPages) {
            const auto size = state.pageSize(page);
            const double centerY = state.offsetY + (state.pageTop(page) + size.height / 2.0) * state.zoom;
            priorities.emplace(page, std::abs(centerY - canvasHeight / 2.0));
        }
        if (priorities.empty()) {
            priorities.emplace(state.page, 0);
        }
        const int detailPage = canvasDetailPage(priorities);
        const bool useMediumDetail = canvasUsesMediumDetail();
        return std::all_of(zoomedOutPages.begin(), zoomedOutPages.end(), [this, detailPage, useMediumDetail](int page) {
            return page == detailPage ?
                           canvasRenderer.matches(canvasRenderRequest(
                                   page, canvasDetailRenderScale(page, gtk_widget_get_scale_factor(canvas), true))) :
                   useMediumDetail ? canvasRenderer.matches(canvasMediumDetailRequest(page)) :
                                     canvasRenderer.matches(canvasLodRequest(page));
        });
    };
    while (!visibleMediumPagesReady() && g_get_monotonic_time() < mediumDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(visibleMediumPagesReady(), "Configured neighboring LOD pages were not ready after zoom settled");
    require(canvasRenderer.mediumDetailCachePages() <= static_cast<size_t>(renderingSettings.mediumDetailCachePages) &&
                    canvasRenderer.mediumDetailCacheBytes() <=
                            static_cast<size_t>(renderingSettings.mediumDetailCacheMegabytes) * bytesPerMegabyte,
            "Medium-detail cache exceeded its page or byte budget");
    capture("canvas-medium-detail-settled");
    canvasRenderer.setCacheLimits(1, 1, bytesPerMegabyte, 1, bytesPerMegabyte);
    require(canvasRenderer.fullDetailCachePages() <= 1 && canvasRenderer.mediumDetailCachePages() <= 1 &&
                    canvasRenderer.mediumDetailCacheBytes() <= bytesPerMegabyte &&
                    canvasRenderer.lowDetailCachePages() <= 1 &&
                    canvasRenderer.lowDetailCacheBytes() <= bytesPerMegabyte,
            "Changing LOD cache preferences did not trim existing raster caches");
    applyCanvasCacheLimits();
    std::cout << "PASS: GTK4 LOD cache limits trim rasters and zoom thresholds select visible detail\n";
}

void Prototype::verifySearchOverlayGeometry() {
    const int pageNumber = options.verifySearchPage;
    const std::string& query = options.verifySearchQuery;
    require(!query.empty() && pageNumber >= 0 && pageNumber < pageCount,
            "Search geometry verification needs a query and an in-range PDF page");
    require(searchController.entry() && searchController.widget(),
            "Search geometry verification needs the GTK search bar");

    const int originalPage = state.page;
    navigateToPage(pageNumber);
    openSearchBar();
    require(gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchController.widget())),
            "PDF Search geometry verification could not open the search bar");

    const auto mappedDeadline = g_get_monotonic_time() + 750000;
    while ((!gtk_widget_get_mapped(searchController.widget()) || gtk_widget_get_width(searchController.widget()) <= 0 ||
            gtk_widget_get_height(searchController.widget()) <= 0) &&
           g_get_monotonic_time() < mappedDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(gtk_widget_get_mapped(searchController.widget()) && gtk_widget_get_width(searchController.widget()) > 0,
            "PDF Search geometry entry did not map in the window");

    gtk_editable_set_text(GTK_EDITABLE(searchController.entry()), query.c_str());
    std::unique_ptr<SearchEventLoopHeartbeat> heartbeat;
    if (options.verifySearchScanAll) {
        heartbeat = std::make_unique<SearchEventLoopHeartbeat>(searchController);
    }
    auto searchStarted = g_get_monotonic_time();
    searchNext();
    const auto activeMatchOnPage = [this, pageNumber] {
        const auto& active = searchController.activeMatch();
        return active && *active < searchController.matches().size() &&
               searchController.matches()[*active].page == pageNumber;
    };
    const auto waitForFirstMatch = [&] {
        const auto resultDeadline = g_get_monotonic_time() + 8000000;
        while (!activeMatchOnPage() && g_get_monotonic_time() < resultDeadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(activeMatchOnPage(), "PDF Search did not find a match on the requested page");
    };
    waitForFirstMatch();
    auto firstMatchMilliseconds = (g_get_monotonic_time() - searchStarted) / 1000.0;
    if (options.verifySearchScanAll) {
        gtk_editable_set_text(GTK_EDITABLE(searchController.entry()), (query + " cancellation-probe").c_str());
        const auto cancelDeadline = g_get_monotonic_time() + 500000;
        while ((searchController.isSearching() || !searchController.matches().empty()) &&
               g_get_monotonic_time() < cancelDeadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(!searchController.isSearching() && searchController.matches().empty() &&
                        !searchController.activeMatch(),
                "Editing the Search query did not cancel the worker and clear stale matches");

        gtk_editable_set_text(GTK_EDITABLE(searchController.entry()), query.c_str());
        const auto querySettleDeadline = g_get_monotonic_time() + 220000;
        while (g_get_monotonic_time() < querySettleDeadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        searchStarted = g_get_monotonic_time();
        searchNext();
        waitForFirstMatch();
        firstMatchMilliseconds = (g_get_monotonic_time() - searchStarted) / 1000.0;

        const auto scanDeadline = g_get_monotonic_time() + 60000000;
        while (searchController.isSearching() && g_get_monotonic_time() < scanDeadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        heartbeat->stop();
        require(!searchController.isSearching(), "Long-document Search did not finish its page-by-page scan");
        require(searchController.pagesScanned() == pageCount,
                "Long-document Search stopped before scanning every page");
        require(heartbeat->ticks > 0, "Long-document Search prevented the GTK event-loop heartbeat from running");
        require(heartbeat->maximumGapMicroseconds < 50000,
                "Long-document Search blocked the GTK event loop for at least 50 ms");
        std::cout << "SEARCH RESPONSIVENESS: pages=" << searchController.pagesScanned()
                  << " heartbeat_ticks=" << heartbeat->ticks
                  << " max_ui_gap_ms=" << static_cast<double>(heartbeat->maximumGapMicroseconds) / 1000.0
                  << " max_gap_after_scanned_pages=" << heartbeat->pagesScannedAtMaximumGap
                  << " first_match_ms=" << firstMatchMilliseconds
                  << " total_scan_ms=" << static_cast<double>(g_get_monotonic_time() - searchStarted) / 1000.0 << '\n';
    }

    auto* pdfPage = pdfPageForSearch(pageNumber);
    require(pdfPage != nullptr, "PDF Search geometry verification could not access the source page");
    double pageHeight = 0;
    poppler_page_get_size(pdfPage, nullptr, &pageHeight);
    GList* results = poppler_page_find_text(pdfPage, query.c_str());
    std::vector<SearchMatch> expected;
    for (GList* result = results; result; result = result->next) {
        const auto* rectangle = static_cast<const PopplerRectangle*>(result->data);
        if (!rectangle || !std::isfinite(rectangle->x1) || !std::isfinite(rectangle->y1) ||
            !std::isfinite(rectangle->x2) || !std::isfinite(rectangle->y2)) {
            continue;
        }
        expected.push_back({pageNumber, std::min(rectangle->x1, rectangle->x2),
                            pageHeight - std::max(rectangle->y1, rectangle->y2), std::max(rectangle->x1, rectangle->x2),
                            pageHeight - std::min(rectangle->y1, rectangle->y2)});
    }
    g_list_free_full(results, [](gpointer data) { poppler_rectangle_free(static_cast<PopplerRectangle*>(data)); });
    g_object_unref(pdfPage);

    std::vector<SearchMatch> actual;
    for (const auto& match: searchController.matches()) {
        if (match.page == pageNumber) {
            actual.push_back(match);
        }
    }
    require(!expected.empty() && actual.size() == expected.size(),
            "PDF Search result count did not match Poppler's source-page matches");
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto& want = expected[index];
        const auto& got = actual[index];
        require(std::abs(want.x0 - got.x0) < 0.1 && std::abs(want.y0 - got.y0) < 0.1 &&
                        std::abs(want.x1 - got.x1) < 0.1 && std::abs(want.y1 - got.y1) < 0.1,
                "PDF Search overlay rectangle differs from the source-page match coordinates");
    }

    const int deviceScale = canvas ? gtk_widget_get_scale_factor(canvas) : 1;
    require(waitForCanvasRaster(pageNumber, canvasRenderScale(pageNumber, deviceScale), 15000),
            "PDF Search geometry verification timed out waiting for the page background raster");
    const auto* raster = canvasRenderer.find(pageNumber);
    require(raster && raster->background && cairo_surface_status(raster->background.get()) == CAIRO_STATUS_SUCCESS,
            "PDF Search geometry verification found no valid background raster to compare against");
    waitForNextWindowFrame();
    capture("search-pdf-geometry");
    closeSearchBar();
    navigateToPage(originalPage);
    update();
    std::cout << "PASS: GTK4 PDF Search overlay geometry matches Poppler page " << pageNumber + 1 << " ("
              << expected.size() << " matches)\n";
}

void Prototype::verifySearchIntegration() {
    if (!options.verifySearchQuery.empty()) {
        verifySearchOverlayGeometry();
        return;
    }
    require(pageCount >= 2 && searchController.entry() && searchController.widget(),
            "Search integration needs the two-page PDF and GTK search bar");
    const int originalPage = state.page;
    navigateToPage(0);

    prototype::TextItem sample;
    sample.page = 0;
    sample.layerId = state.activeLayerId(0);
    sample.originLayer = state.activeLayerOrigin(0);
    sample.text = "Straße search probe";
    sample.fontName = "Sans";
    sample.fontSize = 16;
    sample.x = 36;
    sample.y = 52;
    const auto [width, height] = prototype::textNaturalSize(sample);
    sample.layoutWidth = width;
    sample.layoutHeight = height;
    state.texts.push_back(sample);

    require(pluginRuntime->invokeMenu("ToolShortcuts", 12),
            "ToolShortcuts Search did not open the GTK 4 document search bar");
    require(gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchController.widget())),
            "ToolShortcuts Search left the GTK search bar closed");
    const auto mappedDeadline = g_get_monotonic_time() + 750000;
    while ((!gtk_widget_get_mapped(searchController.widget()) || gtk_widget_get_width(searchController.widget()) <= 0 ||
            gtk_widget_get_height(searchController.widget()) <= 0) &&
           g_get_monotonic_time() < mappedDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(gtk_widget_get_mapped(searchController.widget()) && gtk_widget_get_width(searchController.widget()) > 0 &&
                    gtk_widget_get_height(searchController.widget()) > 0,
            "Document Search bar did not receive mapped space in the window");

    const auto waitForSearch = [this] {
        const auto deadline = g_get_monotonic_time() + 3000000;
        while (searchController.isSearching() && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(!searchController.isSearching(), "Incremental document Search did not finish");
    };
    const auto waitForEntryChange = [] {
        const auto deadline = g_get_monotonic_time() + 220000;
        while (g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
    };

    gtk_editable_set_text(GTK_EDITABLE(searchController.entry()), "STRASSE");
    waitForEntryChange();
    require(searchController.matches().empty() && !searchController.isSearching(),
            "Editing the Search field started a document scan before Enter");
    searchNext();
    require(searchController.matches().empty() && searchController.isSearching(),
            "Search did not defer its first page scan to the GTK event loop");
    waitForSearch();
    require(searchController.matches().size() == 1 && searchController.matches().front().page == 0 &&
                    std::abs(searchController.matches().front().x0 - sample.x) < 1.0,
            "Search did not case-fold editable rich text or map its UTF-8 match to canvas coordinates");
    searchNext();
    require(searchController.activeMatch() == std::optional<size_t>{0} && state.page == 0,
            "Search Next did not select the native text match on the active page");
    const auto searchRevealDeadline = g_get_monotonic_time() + 350000;
    while (g_get_monotonic_time() < searchRevealDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(gtk_widget_get_mapped(GTK_WIDGET(searchController.entry())) &&
                    gtk_widget_get_height(GTK_WIDGET(searchController.entry())) >= 24,
            "Document Search entry did not finish revealing in the GTK search bar");
    capture("search-native-text");

    gtk_editable_set_text(GTK_EDITABLE(searchController.entry()), "xournal++");
    searchNext();
    waitForSearch();
    auto* pdfPage = pdfPageForSearch(0);
    require(pdfPage != nullptr, "Search could not access the original PDF page for geometry verification");
    double pdfHeight = 0;
    poppler_page_get_size(pdfPage, nullptr, &pdfHeight);
    GList* pdfResults = poppler_page_find_text(pdfPage, "xournal++");
    require(pdfResults != nullptr, "Search geometry fixture has no matching PDF text");
    const auto* pdfResult = static_cast<const PopplerRectangle*>(pdfResults->data);
    const double expectedPdfTop = pdfHeight - std::max(pdfResult->y1, pdfResult->y2);
    const double expectedPdfBottom = pdfHeight - std::min(pdfResult->y1, pdfResult->y2);
    const bool pdfGeometryMatches = std::any_of(searchController.matches().begin(), searchController.matches().end(),
                                                [expectedPdfTop, expectedPdfBottom](const SearchMatch& match) {
                                                    return match.page == 0 &&
                                                           std::abs(match.y0 - expectedPdfTop) < 0.1 &&
                                                           std::abs(match.y1 - expectedPdfBottom) < 0.1;
                                                });
    g_list_free_full(pdfResults, [](gpointer data) { poppler_rectangle_free(static_cast<PopplerRectangle*>(data)); });
    g_object_unref(pdfPage);
    require(pdfGeometryMatches,
            "PDF Search highlights did not convert Poppler result coordinates to canvas page coordinates");
    require(!searchController.matches().empty() &&
                    std::any_of(searchController.matches().begin(), searchController.matches().end(),
                                [](const SearchMatch& match) { return match.page == 0 && match.x1 > match.x0; }),
            "Search did not find case-insensitive text in the original PDF background");
    require(searchController.activeMatch() && searchController.matches()[*searchController.activeMatch()].page == 0,
            "Search Next did not navigate to a PDF text match");

    gtk_editable_set_text(GTK_EDITABLE(searchController.entry()), "Page 2");
    searchNext();
    waitForSearch();
    require(searchController.activeMatch() && searchController.matches()[*searchController.activeMatch()].page == 1 &&
                    state.page == 1,
            "Search Next did not continue from the current page into a later PDF page");
    searchPrevious();
    require(searchController.activeMatch() && searchController.matches()[*searchController.activeMatch()].page == 1,
            "Search Previous did not wrap among document-wide results");

    closeSearchBar();
    require(!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(searchController.widget())) &&
                    searchController.matches().empty() && !searchController.activeMatch(),
            "Closing search did not hide the bar and clear its result overlays");
    state.texts.pop_back();
    navigateToPage(originalPage);
    update();
    std::cout << "PASS: GTK4 document Search across PDF and editable text\n";
}

void Prototype::verifyScrollMotionIntegration() {
    require(canvas && scrollController, "Touchpad scroll verification needs the mapped GTK scroll controller");
    const auto viewportWidth = gtk_widget_get_width(canvas);
    const auto viewportHeight = gtk_widget_get_height(canvas);
    require(viewportWidth > 0 && viewportHeight > 0, "Touchpad scroll verification canvas is not mapped");
    const double pageWidth = std::max(600.0, viewportWidth + 300.0);
    std::vector<prototype::PageSize> pages(12, {pageWidth, 800});
    pageCount = static_cast<int>(pages.size());
    state.setPages(std::move(pages));
    state.zoom = 1.0;
    state.selectActivePage(0);
    state.offsetX = (viewportWidth - state.layoutWidth() * state.zoom) / 2.0;
    state.offsetY = 0;
    state.constrainScrollOffsets(viewportWidth, viewportHeight);
    state.syncActivePage(viewportWidth, viewportHeight);
    update();
    capture("scroll-start");

    const auto emitScroll = [this](double dx, double dy) {
        gboolean handled = false;
        g_signal_emit_by_name(scrollController, "scroll", dx, dy, &handled);
        require(handled, "The GTK scroll controller did not handle a continuous scroll signal");
    };
    const auto emitScrollBegin = [this] { g_signal_emit_by_name(scrollController, "scroll-begin"); };
    const auto emitScrollEnd = [this] { g_signal_emit_by_name(scrollController, "scroll-end"); };

    const auto startX = state.offsetX;
    const auto startY = state.offsetY;
    emitScrollBegin();
    emitScroll(0.25, 0.9);
    emitScroll(0.2, 1.4);
    emitScroll(0.4, 2.0);
    emitScrollEnd();
    require(state.offsetX == startX && state.offsetY < startY,
            "A vertical touchpad gesture should move down the pages without horizontal drift");
    capture("scroll-gesture");

    const auto momentumStartY = state.offsetY;
    g_signal_emit_by_name(scrollController, "decelerate", 0.0, 0.9);
    const auto momentumDeadline = g_get_monotonic_time() + 900000;
    while (state.offsetY > momentumStartY - 12 && g_get_monotonic_time() < momentumDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(state.offsetY < momentumStartY - 12 && state.offsetX == startX,
            "GTK touchpad release velocity did not produce bounded vertical momentum");
    capture("scroll-momentum");

    state.offsetX = 0;
    state.constrainScrollOffsets(viewportWidth, viewportHeight);
    require(state.hasHorizontalScrollRoom(viewportWidth),
            "a wide single-column page should expose its horizontal scroll range");
    emitScrollBegin();
    emitScroll(10.0, 0.2);
    emitScroll(10.0, 0.2);
    emitScrollEnd();
    require(state.offsetX < 0 && state.offsetX >= -4.0,
            "intentional horizontal movement should stay low and remain inside the page bounds");
    capture("scroll-horizontal-grid");

    const double leftGridBound = viewportWidth - state.layoutWidth() * state.zoom;
    state.offsetX = leftGridBound;
    state.constrainScrollOffsets(viewportWidth, viewportHeight);
    emitScrollBegin();
    emitScroll(20.0, 0);
    emitScrollEnd();
    require(std::abs(state.offsetX - leftGridBound) < 1e-9,
            "horizontal touchpad motion must stop at the page-grid edge");

    const auto bottomOffset = viewportHeight - state.layoutHeight() * state.zoom;
    state.offsetX = startX;
    state.offsetY = bottomOffset;
    state.constrainScrollOffsets(viewportWidth, viewportHeight);
    update();
    emitScrollBegin();
    emitScroll(0, 20);
    emitScrollEnd();
    const auto clampedOffset = state.offsetY;
    g_signal_emit_by_name(scrollController, "decelerate", 0.0, 0.9);
    const auto stopDeadline = g_get_monotonic_time() + 500000;
    while (scrollMomentum.isActive() && g_get_monotonic_time() < stopDeadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(!scrollMomentum.isActive() && std::abs(state.offsetY - clampedOffset) < 1e-9,
            "Touchpad momentum must stop at the page-grid boundary");
    capture("scroll-boundary");
    std::cout << "PASS: GTK4 touchpad axis intent, page-grid bounds and momentum\n";
}

void Prototype::verifySmartSelectPerformanceIntegration() {
    require(canvas && mouse, "Smart Select performance verification needs the mapped canvas drag controller");
    if (document && pageList && splitView) {
        adw_overlay_split_view_set_show_sidebar(splitView, true);
        const auto sidebarDeadline = g_get_monotonic_time() + 10000000;
        while ((!gtk_widget_get_mapped(GTK_WIDGET(pageList)) || !thumbnailRenderer.find(0) ||
                thumbnailRenderer.pendingCount() > 0 || thumbnailRenderer.activeCount() > 0) &&
               g_get_monotonic_time() < sidebarDeadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(gtk_widget_get_mapped(GTK_WIDGET(pageList)) && thumbnailRenderer.find(0) &&
                        thumbnailRenderer.pendingCount() == 0 && thumbnailRenderer.activeCount() == 0,
                "The Pages sidebar did not finish rendering visible PDF thumbnails before the ink measurement");
    }
    const int viewportWidth = gtk_widget_get_width(canvas);
    const int viewportHeight = gtk_widget_get_height(canvas);
    require(viewportWidth > 0 && viewportHeight > 0, "Smart Select performance canvas is not mapped");

    require(options.pdf.empty() || (document && pageCount > 0),
            "The requested PDF was not loaded for the ink performance check");
    if (!document || pageCount <= 0) {
        state.setPages({{600, 800}});
        pageCount = 1;
    }
    CanvasLayoutSettings testLayout;
    state.setLayoutSettings(testLayout);
    state.selectActivePage(0);
    const auto pageSize = state.pageSize(0);
    require(std::isfinite(pageSize.width) && std::isfinite(pageSize.height) && pageSize.width > 0 &&
                    pageSize.height > 0,
            "Smart Select performance page has invalid dimensions");
    state.zoom =
            std::clamp(std::min({0.75, viewportWidth / pageSize.width, viewportHeight / pageSize.height}), 0.1, 8.0);
    state.offsetX = (viewportWidth - pageSize.width * state.zoom) / 2.0 - state.pageLeft(0) * state.zoom;
    state.offsetY = (viewportHeight - pageSize.height * state.zoom) / 2.0 - state.pageTop(0) * state.zoom;
    state.constrainScrollOffsets(viewportWidth, viewportHeight);
    state.syncActivePage(viewportWidth, viewportHeight);
    fitMode = false;
    fitted = true;

    constexpr size_t strokeCount = 40;
    constexpr size_t pointsPerStroke = 128;
    constexpr size_t pointsPerFrame = 16;
    state.ink.clear();
    state.ink.reserve(strokeCount * 2);
    inkRasterCache.clear();

    const auto waitForFrame = [this](int previousPaints, long long started, std::string_view description) {
        const auto deadline = started + 500000;
        while (paints <= previousPaints && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(500);
        }
        const auto errorMessage = std::string(description) + " did not produce a GTK canvas frame";
        require(paints > previousPaints, errorMessage.c_str());
        return (g_get_monotonic_time() - started) / 1000.0;
    };
    const auto measureInkTool = [&](Tool tool, std::vector<double>& frameLatencyMs) {
        selectTool(tool);
        const size_t firstStroke = state.ink.size();
        const size_t bytesCopiedBefore = inkRasterCache.stats().bytesCopied;
        frameLatencyMs.reserve(strokeCount * pointsPerStroke / pointsPerFrame);

        for (size_t strokeIndex = 0; strokeIndex < strokeCount; ++strokeIndex) {
            const double verticalProgress = static_cast<double>(strokeIndex) / (strokeCount - 1);
            const double localY = pageSize.height * (0.1 + verticalProgress * 0.8);
            const auto start = state.pageScreenPoint(0, pageSize.width * 0.12, localY);
            const auto end = state.pageScreenPoint(0, pageSize.width * 0.88, localY);
            g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
            require(input.active(), "The mouse drawing controller did not begin the ink stroke");

            for (size_t framePoint = 0; framePoint < pointsPerStroke; framePoint += pointsPerFrame) {
                const int previousPaints = paints;
                const auto started = g_get_monotonic_time();
                const size_t endPoint = std::min(pointsPerStroke, framePoint + pointsPerFrame);
                for (size_t pointIndex = framePoint; pointIndex < endPoint; ++pointIndex) {
                    const double progress = static_cast<double>(pointIndex + 1) / pointsPerStroke;
                    const double dx = (end.x - start.x) * progress;
                    const double dy = (end.y - start.y) * progress + std::sin(progress * 8 * G_PI) * 2.0;
                    g_signal_emit_by_name(mouse, "drag-update", dx, dy);
                }
                frameLatencyMs.push_back(waitForFrame(previousPaints, started, "Ink drawing"));
            }

            g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
            const int previousPaints = paints;
            const auto started = g_get_monotonic_time();
            waitForFrame(previousPaints, started, "Finishing an ink stroke");
            require(state.ink.size() == firstStroke + strokeIndex + 1,
                    "The mouse drawing controller did not commit the expected ink stroke");
        }

        const size_t bytesCopied = inkRasterCache.stats().bytesCopied - bytesCopiedBefore;
        std::sort(frameLatencyMs.begin(), frameLatencyMs.end());
        const auto p95Index = (frameLatencyMs.size() * 95 + 99) / 100 - 1;
        const std::array<double, 3> summary{frameLatencyMs[frameLatencyMs.size() / 2], frameLatencyMs[p95Index],
                                            frameLatencyMs.back()};
        return std::pair{summary, bytesCopied};
    };

    std::vector<double> penFrameLatencyMs;
    const auto [penLatency, penBytesCopied] = measureInkTool(Tool::Pen, penFrameLatencyMs);
    std::vector<double> highlighterFrameLatencyMs;
    const size_t highlighterStart = state.ink.size();
    const auto [highlighterLatency, highlighterBytesCopied] =
            measureInkTool(Tool::Highlighter, highlighterFrameLatencyMs);
    require(state.ink.size() == highlighterStart + strokeCount,
            "The Highlighter drawing performance fixture did not commit all strokes");
    capture("smart-select-performance-ink");

    state.selected.clear();
    for (size_t i = highlighterStart; i < state.ink.size(); ++i) {
        state.selected.push_back(i);
    }
    require(state.selected.size() == strokeCount, "Smart Select performance fixture did not select every stroke");
    selectTool(Tool::SmartSelect);
    update();
    capture("smart-select-performance-selected");

    const auto bounds = state.selectionBox();
    require(bounds.has_value(), "Smart Select performance fixture has no selection bounds");
    const double startX = state.offsetX + (state.pageLeft(0) + (bounds->x0 + bounds->x1) / 2) * state.zoom;
    const double startY = state.offsetY + (state.pageTop(0) + (bounds->y0 + bounds->y1) / 2) * state.zoom;
    g_signal_emit_by_name(mouse, "drag-begin", startX, startY);
    require(state.movingSelection(), "Smart Select did not begin moving the selected annotation group");

    std::vector<double> frameLatencyMs;
    frameLatencyMs.reserve(24);
    for (int frame = 1; frame <= 24; ++frame) {
        const int previousPaints = paints;
        const auto started = g_get_monotonic_time();
        const double dx = frame * 3.0;
        const double dy = frame * 0.5;
        g_signal_emit_by_name(mouse, "drag-update", dx, dy);
        const auto deadline = started + 500000;
        while (paints <= previousPaints && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(500);
        }
        require(paints > previousPaints, "Smart Select drag did not produce a GTK canvas frame");
        frameLatencyMs.push_back((g_get_monotonic_time() - started) / 1000.0);
    }
    g_signal_emit_by_name(mouse, "drag-end", 72.0, 12.0);
    require(!state.movingSelection(), "Smart Select drag did not finish the annotation group move");
    update();
    capture("smart-select-performance-moved");

    std::sort(frameLatencyMs.begin(), frameLatencyMs.end());
    const double medianMs = frameLatencyMs[frameLatencyMs.size() / 2];
    const double p95Ms = frameLatencyMs[(frameLatencyMs.size() * 95 + 99) / 100 - 1];
    const double maxMs = frameLatencyMs.back();
    std::cout << "RESULT: GTK4 live ink frame latency pen_median_ms=" << penLatency[0]
              << " pen_p95_ms=" << penLatency[1] << " pen_max_ms=" << penLatency[2]
              << " pen_cache_bytes_copied=" << penBytesCopied << " highlighter_median_ms=" << highlighterLatency[0]
              << " highlighter_p95_ms=" << highlighterLatency[1] << " highlighter_max_ms=" << highlighterLatency[2]
              << " highlighter_cache_bytes_copied=" << highlighterBytesCopied
              << " (40 strokes per tool, 128 points each)\n";
    std::cout << "PASS: GTK4 Smart Select drag raster-cache frame latency median_ms=" << medianMs << " p95_ms=" << p95Ms
              << " max_ms=" << maxMs << " (40 highlighter strokes, 128 points each)\n";
    if (options.pdf.empty()) {
        require(penLatency[1] < 100 && penLatency[2] < 250,
                "Pen drawing exceeded the 100 ms p95 / 250 ms frame responsiveness limit");
        require(highlighterLatency[1] < 100 && highlighterLatency[2] < 250,
                "Highlighter drawing exceeded the 100 ms p95 / 250 ms frame responsiveness limit");
        require(p95Ms < 100 && maxMs < 250,
                "Smart Select drag exceeded the 100 ms p95 / 250 ms maximum-frame responsiveness limit");
    }
}

void Prototype::verifyPenLineStyleIntegration() {
    require(lineStyleDropdown && gtk_widget_get_visible(lineStyleDropdown),
            "Pen line-style selector is not visible for the Pen tool");
    require(state.brush.tool == Tool::Pen && gtk_drop_down_get_selected(GTK_DROP_DOWN(lineStyleDropdown)) == 0,
            "The default Pen line style is not Solid");

    const std::array<std::pair<guint, prototype::LineDashPattern>, 4> namedStyles{
            {{0, {}}, {1, {6, 3}}, {2, {6, 3, 0.5, 3}}, {3, {0.5, 3}}}};
    for (const auto& [index, expected]: namedStyles) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(lineStyleDropdown), index);
        require(penBrush.lineDashes == expected && state.brush.lineDashes == expected,
                "Pen line-style selector did not apply its named dash pattern");
        require(gtk_drop_down_get_selected(GTK_DROP_DOWN(lineStyleDropdown)) == index,
                "Pen line-style selector did not display its selected named pattern");
        require(importedConfig.settings.penLineStyle == prototype::formatLineStyle(expected),
                "Pen line-style selector did not persist its named pattern");
        if (index == 1) {
            capture("pen-line-style-dash", lineStyleDropdown, true);
        }
    }

    std::string error;
    require(changePluginActionState("tool-pen-line-style", std::string{"cust: 1.25 2.50"}, error) &&
                    penBrush.lineDashes == prototype::LineDashPattern{1.25, 2.5} &&
                    gtk_drop_down_get_selected(GTK_DROP_DOWN(lineStyleDropdown)) == 4,
            error.empty() ? "Plugin action did not set a custom Pen dash pattern" : error.c_str());
    prototype::PluginActionValue actionState;
    require(getPluginActionState("tool-pen-line-style", actionState, error) &&
                    std::holds_alternative<std::string>(actionState) &&
                    std::get<std::string>(actionState) == "cust: 1.25 2.50",
            error.empty() ? "Plugin action state did not report the custom Pen dash pattern" : error.c_str());
    prototype::PluginToolInfo toolInfo;
    require(getPluginToolInfo("pen", toolInfo, error) && toolInfo.lineStyle && *toolInfo.lineStyle == "cust: 1.25 2.50",
            error.empty() ? "Pen tool info did not report the custom dash pattern" : error.c_str());
    prototype::PluginToolInfo activeToolInfo;
    require(getPluginToolInfo("active", activeToolInfo, error) && activeToolInfo.lineStyle &&
                    *activeToolInfo.lineStyle == "cust: 1.25 2.50",
            error.empty() ? "Active tool info did not report its custom dash pattern" : error.c_str());
    prototype::PluginToolInfo highlighterToolInfo;
    require(getPluginToolInfo("highlighter", highlighterToolInfo, error) && !highlighterToolInfo.lineStyle,
            error.empty() ? "Highlighter tool info should omit the Pen-only line-style field" : error.c_str());

    gtk_drop_down_set_selected(GTK_DROP_DOWN(lineStyleDropdown), 0);
    require(penBrush.lineDashes.empty() && importedConfig.settings.penLineStyle == "plain",
            "Selecting Solid did not reset and persist the Pen style");
    std::cout << "PASS: GTK4 Pen named/custom line-style selector and plugin state\n";
}

}  // namespace prototype::gtk4
