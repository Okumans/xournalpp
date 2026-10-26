#include "Prototype.h"

namespace prototype::gtk4 {

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

GtkWidget* Prototype::preferenceCombo(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                                      const char* const* options, guint selected, PreferenceKey key) {
    auto* row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    auto* model = gtk_string_list_new(options);
    adw_combo_row_set_model(row, G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(row, selected);
    g_object_set_data(G_OBJECT(row), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
    g_signal_connect(row, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto key = static_cast<PreferenceKey>(
                                 GPOINTER_TO_UINT(g_object_get_data(object, "gtk4-preference-key")));
                         self->changePreference(key, GTK_WIDGET(object));
                     }),
                     this);
    preferenceControls[static_cast<size_t>(key)] = GTK_WIDGET(row);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return GTK_WIDGET(row);
}

GtkWidget* Prototype::preferenceSwitch(AdwPreferencesGroup* group, const char* title, const char* subtitle, bool active,
                                       PreferenceKey key) {
    auto* row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(row, active);
    g_object_set_data(G_OBJECT(row), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
    g_signal_connect(row, "notify::active", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto key = static_cast<PreferenceKey>(
                                 GPOINTER_TO_UINT(g_object_get_data(object, "gtk4-preference-key")));
                         self->changePreference(key, GTK_WIDGET(object));
                     }),
                     this);
    preferenceControls[static_cast<size_t>(key)] = GTK_WIDGET(row);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return GTK_WIDGET(row);
}

GtkWidget* Prototype::preferenceSpin(AdwPreferencesGroup* group, const char* title, const char* subtitle, double value,
                                     double minimum, double maximum, double step, guint digits, const char* suffix,
                                     PreferenceKey key) {
    auto* row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(row, subtitle);
    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    auto* spin = gtk_spin_button_new_with_range(minimum, maximum, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), true);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_widget_set_size_request(spin, 96, -1);
    gtk_box_append(GTK_BOX(box), spin);
    if (suffix && *suffix) {
        gtk_box_append(GTK_BOX(box), gtk_label_new(suffix));
    }
    adw_action_row_add_suffix(row, box);
    adw_action_row_set_activatable_widget(row, spin);
    g_object_set_data(G_OBJECT(spin), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
    g_signal_connect(spin, "value-changed", G_CALLBACK(+[](GtkSpinButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto key = static_cast<PreferenceKey>(
                                 g_object_get_data(G_OBJECT(button), "gtk4-preference-key") ?
                                         GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "gtk4-preference-key")) :
                                         0);
                         self->changePreference(key, GTK_WIDGET(button));
                     }),
                     this);
    preferenceControls[static_cast<size_t>(key)] = GTK_WIDGET(spin);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return GTK_WIDGET(spin);
}

GtkWidget* Prototype::preferenceColor(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                                      std::uint32_t color, PreferenceKey key) {
    auto* row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(row, subtitle);
    auto* dialog = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(dialog, true);
    auto* button = gtk_color_dialog_button_new(dialog);
    gtk_widget_set_size_request(button, 48, 36);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    const auto rgba = unpackArgbColor(color);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
    adw_action_row_add_suffix(row, button);
    adw_action_row_set_activatable_widget(row, button);
    g_object_set_data(G_OBJECT(button), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
    g_signal_connect(button, "notify::rgba", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto key = static_cast<PreferenceKey>(
                                 GPOINTER_TO_UINT(g_object_get_data(object, "gtk4-preference-key")));
                         self->changePreference(key, GTK_WIDGET(object));
                     }),
                     this);
    preferenceControls[static_cast<size_t>(key)] = button;
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return GTK_WIDGET(button);
}

GtkWidget* Prototype::preferenceTextEntry(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                                          const std::string& value, PreferenceKey key) {
    auto* row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(row, subtitle);
    auto* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), value.c_str());
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 30);
    gtk_widget_set_hexpand(entry, true);
    gtk_widget_set_size_request(entry, 240, -1);
    adw_action_row_add_suffix(row, entry);
    adw_action_row_set_activatable_widget(row, entry);
    preferenceControls[static_cast<size_t>(key)] = entry;
    if (key == PreferenceKey::LatexDefaultText || key == PreferenceKey::LatexGeneratorCommand ||
        key == PreferenceKey::LatexExternalEditorCommand || key == PreferenceKey::LatexTemporaryFileExtension) {
        g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
                             static_cast<Prototype*>(data)->markLatexTextPreferencesPending();
                         }),
                         this);
    }
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return entry;
}

GtkWidget* Prototype::preferenceTemplatePath(AdwPreferencesGroup* group, const std::string& value) {
    auto* row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "TeX template");
    adw_action_row_set_subtitle(row, "Leave empty to use the bundled template");
    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    auto* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), value.c_str());
    gtk_editable_set_width_chars(GTK_EDITABLE(entry), 28);
    gtk_widget_set_size_request(entry, 240, -1);
    gtk_widget_set_hexpand(entry, true);
    latexTemplatePathEntry = entry;
    g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
                         static_cast<Prototype*>(data)->markLatexTextPreferencesPending();
                     }),
                     this);
    gtk_box_append(GTK_BOX(box), entry);
    auto* browse = gtk_button_new_with_label("Browse…");
    gtk_widget_set_valign(browse, GTK_ALIGN_CENTER);
    gtk_accessible_update_property(GTK_ACCESSIBLE(browse), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Choose a LaTeX template file", -1);
    g_signal_connect(
            browse, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->chooseLatexTemplate(); }), this);
    gtk_box_append(GTK_BOX(box), browse);
    adw_action_row_add_suffix(row, box);
    adw_action_row_set_activatable_widget(row, entry);
    preferenceControls[static_cast<size_t>(PreferenceKey::LatexTemplatePath)] = entry;
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return entry;
}

GtkWidget* Prototype::preferenceFont(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                                     const std::string& value) {
    auto* row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(row, subtitle);
    auto* dialog = gtk_font_dialog_new();
    gtk_font_dialog_set_font_map(dialog, pango_cairo_font_map_get_default());
    auto* button = GTK_FONT_DIALOG_BUTTON(gtk_font_dialog_button_new(dialog));
    gtk_font_dialog_button_set_level(button, GTK_FONT_LEVEL_FONT);
    PangoFontDescription* description = pango_font_description_from_string(value.c_str());
    gtk_font_dialog_button_set_font_desc(button, description);
    pango_font_description_free(description);
    adw_action_row_add_suffix(row, GTK_WIDGET(button));
    adw_action_row_set_activatable_widget(row, GTK_WIDGET(button));
    g_signal_connect(button, "notify::font-desc", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                         static_cast<Prototype*>(data)->changePreference(PreferenceKey::LatexEditorFont,
                                                                         GTK_WIDGET(object));
                     }),
                     this);
    preferenceControls[static_cast<size_t>(PreferenceKey::LatexEditorFont)] = GTK_WIDGET(button);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return GTK_WIDGET(button);
}

GtkWidget* Prototype::preferenceLatexTheme(AdwPreferencesGroup* group, const std::string& selectedTheme) {
    auto* row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Syntax color scheme");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Choose colors for LaTeX source highlighting");
    auto* model = gtk_string_list_new(nullptr);
    gtk_string_list_append(model, "System default");
    latexSourceViewThemeIds.clear();
    latexSourceViewThemeIds.emplace_back();
    guint selectedIndex = 0;
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
    auto* manager = gtk_source_style_scheme_manager_get_default();
    const char* const* ids = gtk_source_style_scheme_manager_get_scheme_ids(manager);
    if (ids) {
        for (const char* const* id = ids; *id; ++id) {
            auto* scheme = gtk_source_style_scheme_manager_get_scheme(manager, *id);
            if (!scheme) {
                continue;
            }
            const auto index = static_cast<guint>(latexSourceViewThemeIds.size());
            latexSourceViewThemeIds.emplace_back(*id);
            gtk_string_list_append(model, gtk_source_style_scheme_get_name(scheme));
            if (selectedTheme == *id) {
                selectedIndex = index;
            }
        }
    }
#else
    (void)selectedTheme;
    gtk_widget_set_sensitive(GTK_WIDGET(row), false);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "GtkSourceView was unavailable when this build was made");
#endif
    adw_combo_row_set_model(row, G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(row, selectedIndex);
    g_object_set_data(G_OBJECT(row), "gtk4-preference-key",
                      GUINT_TO_POINTER(static_cast<guint>(PreferenceKey::LatexSourceViewTheme)));
    g_signal_connect(row, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->changePreference(PreferenceKey::LatexSourceViewTheme, GTK_WIDGET(object));
                     }),
                     this);
    preferenceControls[static_cast<size_t>(PreferenceKey::LatexSourceViewTheme)] = GTK_WIDGET(row);
    adw_preferences_group_add(group, GTK_WIDGET(row));
    return GTK_WIDGET(row);
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
        auto* entry = preferenceControls[static_cast<size_t>(keys[index])];
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
    auto* entry = preferenceControls[static_cast<size_t>(PreferenceKey::MainToolRail)];
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
    pendingMainToolRail = *parsed;
    rebuildMainToolRailEditor();
    rebuildMainToolRail();
    return true;
}

GtkWidget* Prototype::createMainToolRailEditorRow(prototype::MainToolAction action) {
    auto* row = gtk_list_box_row_new();
    auto* content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(content, 4);
    gtk_widget_set_margin_bottom(content, 4);
    gtk_widget_set_margin_start(content, 8);
    gtk_widget_set_margin_end(content, 8);
    auto* label = gtk_label_new(std::string(prototype::mainToolRailLabel(action)).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
    gtk_widget_set_hexpand(label, true);
    gtk_box_append(GTK_BOX(content), label);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), content);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   std::string(prototype::mainToolRailLabel(action)).c_str(), -1);

    auto* source = gtk_drag_source_new();
    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    const auto token = prototype::mainToolRailToken(action);
    const std::string actionToken(token);
    g_object_set_data_full(G_OBJECT(source), "gtk4-main-tool-action", g_strdup(actionToken.c_str()), g_free);
    g_signal_connect(source, "prepare",
                     G_CALLBACK(+[](GtkDragSource* dragSource, double, double, gpointer) -> GdkContentProvider* {
                         const auto* actionName = static_cast<const char*>(
                                 g_object_get_data(G_OBJECT(dragSource), "gtk4-main-tool-action"));
                         return actionName ? gdk_content_provider_new_typed(G_TYPE_STRING, actionName) : nullptr;
                     }),
                     nullptr);
    g_object_set_data(G_OBJECT(row), "gtk4-main-tool-drag-source", source);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
    gtk_widget_set_cursor_from_name(row, "grab");
    return row;
}

GtkWidget* Prototype::createMainToolRailEditorList(bool visible) {
    auto* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");

    auto* target = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
    g_object_set_data(G_OBJECT(list), "gtk4-main-tool-drop-target", target);
    g_object_set_data(G_OBJECT(target), "gtk4-main-tool-list", list);
    g_object_set_data(G_OBJECT(target), "gtk4-main-tool-list-visible", GINT_TO_POINTER(visible));
    gtk_drop_target_set_preload(target, true);
    g_signal_connect(target, "drop",
                     G_CALLBACK(+[](GtkDropTarget* dropTarget, const GValue* value, double, double y,
                                    gpointer data) -> gboolean {
                         auto* self = static_cast<Prototype*>(data);
                         const char* actionName = g_value_get_string(value);
                         auto* list = GTK_LIST_BOX(g_object_get_data(G_OBJECT(dropTarget), "gtk4-main-tool-list"));
                         const bool toRail = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dropTarget),
                                                                               "gtk4-main-tool-list-visible")) != 0;
                         if (!actionName || !list) {
                             return false;
                         }

                         std::size_t targetIndex = 0;
                         if (toRail) {
                             auto* row = gtk_list_box_get_row_at_y(list, static_cast<int>(y));
                             for (auto* child = gtk_widget_get_first_child(GTK_WIDGET(list)); child;
                                  child = gtk_widget_get_next_sibling(child)) {
                                 ++targetIndex;
                             }
                             if (row) {
                                 graphene_rect_t bounds{};
                                 targetIndex = static_cast<std::size_t>(gtk_list_box_row_get_index(row));
                                 if (gtk_widget_compute_bounds(GTK_WIDGET(row), GTK_WIDGET(list), &bounds) &&
                                     y >= bounds.origin.y + bounds.size.height / 2.0F) {
                                     ++targetIndex;
                                 }
                             }
                         }
                         return self->applyMainToolRailDrop(actionName, toRail, targetIndex);
                     }),
                     this);
    gtk_widget_add_controller(list, GTK_EVENT_CONTROLLER(target));
    return list;
}

void Prototype::rebuildMainToolRailEditor() {
    if (!mainToolRailVisibleList || !mainToolRailAvailableList) {
        return;
    }
    for (auto* list: {mainToolRailVisibleList, mainToolRailAvailableList}) {
        while (auto* row = gtk_widget_get_first_child(list)) {
            gtk_list_box_remove(GTK_LIST_BOX(list), row);
        }
    }
    for (const auto action: pendingMainToolRail) {
        gtk_list_box_append(GTK_LIST_BOX(mainToolRailVisibleList), createMainToolRailEditorRow(action));
    }
    for (const auto& entry: prototype::mainToolRailNames) {
        if (std::find(pendingMainToolRail.begin(), pendingMainToolRail.end(), entry.first) !=
            pendingMainToolRail.end()) {
            continue;
        }
        gtk_list_box_append(GTK_LIST_BOX(mainToolRailAvailableList), createMainToolRailEditorRow(entry.first));
    }
}

bool Prototype::applyMainToolRailDrop(std::string_view actionToken, bool toRail, std::size_t targetIndex) {
    const auto action = prototype::parseMainToolRailToken(actionToken);
    auto* entry = preferenceControls[static_cast<size_t>(PreferenceKey::MainToolRail)];
    const char* rawLayout = gtk_editable_get_text(GTK_EDITABLE(entry));
    const auto parsedLayout = prototype::parseMainToolRail(rawLayout ? rawLayout : "");
    if (!action || !parsedLayout) {
        return false;
    }
    if (pendingMainToolRail != *parsedLayout) {
        pendingMainToolRail = *parsedLayout;
        rebuildMainToolRailEditor();
    }
    if (!prototype::moveMainToolRailAction(pendingMainToolRail, *action, toRail, targetIndex)) {
        return false;
    }
    const auto serialized = prototype::serializeMainToolRail(pendingMainToolRail);
    gtk_editable_set_text(GTK_EDITABLE(entry), serialized.c_str());
    gtk_widget_remove_css_class(entry, "error");
    rebuildMainToolRailEditor();
    return true;
}

bool Prototype::emitMainToolRailDropForTest(GtkWidget* list, std::string_view actionToken, double y) {
    auto* target = GTK_DROP_TARGET(g_object_get_data(G_OBJECT(list), "gtk4-main-tool-drop-target"));
    if (!target) {
        return false;
    }
    const std::string token(actionToken);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_STRING);
    g_value_set_string(&value, token.c_str());
    gboolean accepted = false;
    g_signal_emit_by_name(target, "drop", &value, 0.0, y, &accepted);
    g_value_unset(&value);
    return accepted;
}

void Prototype::changePreference(PreferenceKey key, GtkWidget* control) {
    if (syncingPreferenceControls) {
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
                auto* countControl = preferenceControls[static_cast<size_t>(PreferenceKey::LayoutGridCount)];
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
            if (selected >= latexSourceViewThemeIds.size()) {
                return;
            }
            settings.latexSourceViewThemeId = latexSourceViewThemeIds[selected];
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
            updateLatexPreferenceSensitivity();
            break;
        case PreferenceKey::LatexEditorWordWrap:
            settings.latexEditorWordWrap = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            break;
        case PreferenceKey::LatexUseExternalEditor:
            settings.latexUseExternalEditor = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
            updateLatexPreferenceSensitivity();
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
    const char* currentPath = gtk_editable_get_text(GTK_EDITABLE(latexTemplatePathEntry));
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
                        gtk_editable_set_text(GTK_EDITABLE(context->first->latexTemplatePathEntry), path);
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
        auto* entry = preferenceControls[static_cast<size_t>(key)];
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
    updateLatexPreferenceSensitivity();
    if (latexSettingsApplyStatus) {
        gtk_label_set_text(GTK_LABEL(latexSettingsApplyStatus), "Saved");
        gtk_widget_add_css_class(latexSettingsApplyStatus, "success");
        gtk_widget_remove_css_class(latexSettingsApplyStatus, "error");
        gtk_widget_remove_css_class(latexSettingsApplyStatus, "dim-label");
    }
    return true;
}

void Prototype::markLatexTextPreferencesPending() {
    if (!latexSettingsApplyStatus) {
        return;
    }
    gtk_label_set_text(GTK_LABEL(latexSettingsApplyStatus), "Unsaved changes");
    gtk_widget_add_css_class(latexSettingsApplyStatus, "dim-label");
    gtk_widget_remove_css_class(latexSettingsApplyStatus, "success");
    gtk_widget_remove_css_class(latexSettingsApplyStatus, "error");
}

void Prototype::updateLatexPreferenceSensitivity() {
    const auto& settings = importedConfig.settings;
    const bool sourceEditorEnabled = !settings.latexUseExternalEditor;
    const auto setSensitive = [this](PreferenceKey key, bool sensitive) {
        if (auto* control = preferenceControls[static_cast<size_t>(key)]) {
            gtk_widget_set_sensitive(control, sensitive);
        }
    };
    setSensitive(PreferenceKey::LatexEditorFont, settings.latexUseCustomEditorFont && sourceEditorEnabled);
    setSensitive(PreferenceKey::LatexExternalEditorAutoConfirm, settings.latexUseExternalEditor);
    setSensitive(PreferenceKey::LatexExternalEditorCommand, settings.latexUseExternalEditor);
    setSensitive(PreferenceKey::LatexTemporaryFileExtension, settings.latexUseExternalEditor);
    setSensitive(PreferenceKey::LatexSourceViewTheme, sourceEditorEnabled);
    setSensitive(PreferenceKey::LatexSourceViewAutoIndent, sourceEditorEnabled);
    setSensitive(PreferenceKey::LatexSourceViewSyntaxHighlight, sourceEditorEnabled);
    setSensitive(PreferenceKey::LatexSourceViewShowLineNumbers, sourceEditorEnabled);
    setSensitive(PreferenceKey::LatexEditorWordWrap, sourceEditorEnabled);
#ifndef GTK4_CANVAS_HAVE_SOURCEVIEW
    setSensitive(PreferenceKey::LatexSourceViewTheme, false);
    setSensitive(PreferenceKey::LatexSourceViewAutoIndent, false);
    setSensitive(PreferenceKey::LatexSourceViewSyntaxHighlight, false);
    setSensitive(PreferenceKey::LatexSourceViewShowLineNumbers, false);
#endif
}

void Prototype::testLatexSetup() {
    if (!applyLatexPreferences()) {
        return;
    }
    if (latexTemplatePath.empty()) {
        showAlert("Choose a TeX template", "Select a template file before checking the LaTeX setup.");
        return;
    }
    gtk_widget_set_sensitive(latexSetupTestButton, false);
    gtk_label_set_text(GTK_LABEL(latexSetupTestStatus), "Compiling the sample formula…");
    gtk_widget_remove_css_class(latexSetupTestStatus, "success");
    gtk_widget_remove_css_class(latexSetupTestStatus, "error");
    const auto& settings = importedConfig.settings;
    prototype::LatexEditor::testConfiguration(
            latexTemplatePath, settings.latexGeneratorCommand, settings.latexDefaultText, latexBrush.color,
            [this](bool success, std::string message) {
                gtk_widget_set_sensitive(latexSetupTestButton, true);
                gtk_label_set_text(GTK_LABEL(latexSetupTestStatus),
                                   success ? "LaTeX setup is ready" : "LaTeX setup needs attention");
                gtk_widget_add_css_class(latexSetupTestStatus, success ? "success" : "error");
                showAlert(success ? "LaTeX setup works" : "LaTeX setup failed", message);
            });
}

void Prototype::openPreferences() {
    if (preferencesDialog) {
        adw_dialog_present(ADW_DIALOG(preferencesDialog), window);
        return;
    }

    preferencesDialog = ADW_PREFERENCES_DIALOG(g_object_ref_sink(adw_preferences_dialog_new()));
    adw_preferences_dialog_set_search_enabled(preferencesDialog, true);
    pendingMainToolRail = importedConfig.settings.mainToolRail;

    auto* appearance = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_name(appearance, "appearance");
    adw_preferences_page_set_title(appearance, "Appearance");
    adw_preferences_page_set_icon_name(appearance, "applications-graphics-symbolic");
    auto* appearanceGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(appearanceGroup, "Window");
    static const char* const themes[] = {"Follow system", "Light", "Dark", nullptr};
    preferenceCombo(appearanceGroup, "Color scheme", "Choose how the canvas follows the desktop appearance", themes,
                    static_cast<guint>(settingsThemeIndex(importedConfig.settings.themeVariant)), PreferenceKey::Theme);
    preferenceSwitch(
            appearanceGroup, "Show pages sidebar", "Keep page rasters beside the canvas",
            importedConfig.settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Default)].showSidebar,
            PreferenceKey::Sidebar);
    adw_preferences_page_add(appearance, appearanceGroup);

    const auto& viewModes = importedConfig.settings.viewModes;
    auto* fullscreenGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(fullscreenGroup, "Fullscreen view");
    preferenceSwitch(fullscreenGroup, "Show header bar", "Keep the app header visible in fullscreen",
                     viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Fullscreen)].showMenubar,
                     PreferenceKey::FullscreenMenubar);
    preferenceSwitch(fullscreenGroup, "Show tool controls", "Keep the canvas toolbar and tool rail visible",
                     viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Fullscreen)].showToolbar,
                     PreferenceKey::FullscreenToolbar);
    preferenceSwitch(fullscreenGroup, "Show pages sidebar", "Keep page rasters visible in fullscreen",
                     viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Fullscreen)].showSidebar,
                     PreferenceKey::FullscreenSidebar);
    adw_preferences_page_add(appearance, fullscreenGroup);

    auto* presentationGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(presentationGroup, "Presentation view");
    preferenceSwitch(presentationGroup, "Use fullscreen", "Enter fullscreen when Presentation Mode starts",
                     viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].goFullscreen,
                     PreferenceKey::PresentationGoFullscreen);
    preferenceSwitch(presentationGroup, "Show header bar", "Keep the app header visible while presenting",
                     viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].showMenubar,
                     PreferenceKey::PresentationMenubar);
    preferenceSwitch(presentationGroup, "Show tool controls", "Keep canvas tools visible while presenting",
                     viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].showToolbar,
                     PreferenceKey::PresentationToolbar);
    preferenceSwitch(presentationGroup, "Show pages sidebar", "Keep page rasters visible while presenting",
                     viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Presentation)].showSidebar,
                     PreferenceKey::PresentationSidebar);
    adw_preferences_page_add(appearance, presentationGroup);
    adw_preferences_dialog_add(preferencesDialog, appearance);

    auto* layoutPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_name(layoutPage, "page-layout");
    adw_preferences_page_set_title(layoutPage, "Page Layout");
    adw_preferences_page_set_icon_name(layoutPage, "view-grid-symbolic");
    auto* layoutFlowGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(layoutFlowGroup, "Page flow and grid");
    static const char* const pageFlowModes[] = {"Vertical", "Horizontal", nullptr};
    preferenceCombo(layoutFlowGroup, "Page flow", "Choose how pages advance through the grid", pageFlowModes,
                    importedConfig.settings.pageLayout.vertical ? 0 : 1, PreferenceKey::LayoutFlow);
    static const char* const gridDimensions[] = {"Columns", "Rows", nullptr};
    preferenceCombo(layoutFlowGroup, "Grid count applies to", "Choose a fixed column or row count", gridDimensions,
                    importedConfig.settings.pageLayout.fixedRows ? 1 : 0, PreferenceKey::LayoutGridDimension);
    const auto& pageLayout = importedConfig.settings.pageLayout;
    preferenceSpin(layoutFlowGroup, "Grid size", "Set the number of columns or rows",
                   static_cast<double>(pageLayout.fixedRows ? pageLayout.rows : pageLayout.columns), 1, 8, 1, 0, "",
                   PreferenceKey::LayoutGridCount);
    adw_preferences_page_add(layoutPage, layoutFlowGroup);

    auto* pairedGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(pairedGroup, "Paired pages");
    preferenceSwitch(pairedGroup, "Show facing pages", "Place adjacent pages in two-page spreads",
                     pageLayout.pairedPages, PreferenceKey::LayoutPairedPages);
    preferenceSpin(pairedGroup, "First spread offset", "Usually 0 or 1; the legacy setting allows 0–100",
                   pageLayout.pairedPageOffset, 0, 100, 1, 0, "pages", PreferenceKey::LayoutPairedPageOffset);
    adw_preferences_page_add(layoutPage, pairedGroup);

    auto* readingGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(readingGroup, "Reading direction");
    static const char* const horizontalDirections[] = {"Left to right", "Right to left", nullptr};
    preferenceCombo(readingGroup, "Horizontal reading", "Choose the page order across columns", horizontalDirections,
                    pageLayout.rightToLeft ? 1 : 0, PreferenceKey::LayoutHorizontalDirection);
    static const char* const verticalDirections[] = {"Top to bottom", "Bottom to top", nullptr};
    preferenceCombo(readingGroup, "Vertical reading", "Choose the page order down rows", verticalDirections,
                    pageLayout.bottomToTop ? 1 : 0, PreferenceKey::LayoutVerticalDirection);
    adw_preferences_page_add(layoutPage, readingGroup);
    adw_preferences_dialog_add(preferencesDialog, layoutPage);

    auto* latexPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_name(latexPage, "latex");
    adw_preferences_page_set_title(latexPage, "LaTeX");
    adw_preferences_page_set_icon_name(latexPage, "accessories-text-editor-symbolic");
    auto* latexGenerationGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(latexGenerationGroup, "Formula generation");
    adw_preferences_group_set_description(latexGenerationGroup,
                                          "Set the source, template and TeX command used when you insert a formula.");
    preferenceSwitch(latexGenerationGroup, "Check LaTeX dependencies automatically",
                     "Preserve the legacy setting; compiler errors are reported by the configured generator",
                     importedConfig.settings.latexAutoCheckDependencies, PreferenceKey::LatexAutoCheckDependencies);
    preferenceTextEntry(latexGenerationGroup, "Default formula",
                        "Selected when you click an empty page with the LaTeX tool",
                        importedConfig.settings.latexDefaultText, PreferenceKey::LatexDefaultText);
    preferenceTemplatePath(latexGenerationGroup,
                           prototype::preferences_detail::pathToUtf8(importedConfig.settings.latexTemplatePath));
    preferenceTextEntry(latexGenerationGroup, "Generator command", "Use {} where the temporary .tex path belongs",
                        importedConfig.settings.latexGeneratorCommand, PreferenceKey::LatexGeneratorCommand);
    auto* latexTestRow = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(latexTestRow), "Test the template and compiler");
    adw_action_row_set_subtitle(latexTestRow, "Compile the default formula without changing the document");
    latexSetupTestStatus = gtk_label_new("");
    gtk_widget_add_css_class(latexSetupTestStatus, "dim-label");
    gtk_widget_set_margin_end(latexSetupTestStatus, 8);
    adw_action_row_add_suffix(latexTestRow, latexSetupTestStatus);
    latexSetupTestButton = gtk_button_new_with_label("Test setup");
    g_signal_connect(latexSetupTestButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->testLatexSetup(); }),
                     this);
    adw_action_row_add_suffix(latexTestRow, latexSetupTestButton);
    adw_preferences_group_add(latexGenerationGroup, GTK_WIDGET(latexTestRow));
    adw_preferences_page_add(latexPage, latexGenerationGroup);

    auto* sourceEditorGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(sourceEditorGroup, "In-app source editor");
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
    adw_preferences_group_set_description(sourceEditorGroup,
                                          "Tune the editor used when external editing is turned off.");
#else
    adw_preferences_group_set_description(
            sourceEditorGroup,
            "GtkSourceView was unavailable when this build was made. Source highlighting and indentation "
            "options are disabled.");
#endif
    preferenceSwitch(sourceEditorGroup, "Use a custom editor font", "Choose a font for LaTeX source",
                     importedConfig.settings.latexUseCustomEditorFont, PreferenceKey::LatexUseCustomEditorFont);
    preferenceFont(sourceEditorGroup, "Editor font", "Set the family, style and size for the source editor",
                   importedConfig.settings.latexEditorFont);
    preferenceSwitch(sourceEditorGroup, "Wrap long lines", "Keep long formulas inside the editor width",
                     importedConfig.settings.latexEditorWordWrap, PreferenceKey::LatexEditorWordWrap);
    preferenceSwitch(sourceEditorGroup, "Indent automatically", "Indent new lines to match the current formula",
                     importedConfig.settings.latexSourceViewAutoIndent, PreferenceKey::LatexSourceViewAutoIndent);
    preferenceSwitch(sourceEditorGroup, "Highlight LaTeX syntax", "Color LaTeX commands and comments",
                     importedConfig.settings.latexSourceViewSyntaxHighlight,
                     PreferenceKey::LatexSourceViewSyntaxHighlight);
    preferenceSwitch(sourceEditorGroup, "Show line numbers", "Number each line in the source editor",
                     importedConfig.settings.latexSourceViewShowLineNumbers,
                     PreferenceKey::LatexSourceViewShowLineNumbers);
    preferenceLatexTheme(sourceEditorGroup, importedConfig.settings.latexSourceViewThemeId);
    adw_preferences_page_add(latexPage, sourceEditorGroup);

    auto* externalEditorGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(externalEditorGroup, "External editor");
    adw_preferences_group_set_description(
            externalEditorGroup, "Open the temporary source in another app. The preview updates when that app closes.");
    preferenceSwitch(externalEditorGroup, "Use an external editor", "Open LaTeX source outside Xournal++",
                     importedConfig.settings.latexUseExternalEditor, PreferenceKey::LatexUseExternalEditor);
    preferenceTextEntry(externalEditorGroup, "Editor command", "Leave empty to use the editor from $EDITOR",
                        importedConfig.settings.latexExternalEditorCommand, PreferenceKey::LatexExternalEditorCommand);
    preferenceTextEntry(
            externalEditorGroup, "Temporary file extension", "The source file extension passed to the external editor",
            importedConfig.settings.latexTemporaryFileExtension, PreferenceKey::LatexTemporaryFileExtension);
    preferenceSwitch(externalEditorGroup, "Insert automatically when valid",
                     "Insert the result after the external editor closes and compilation succeeds",
                     importedConfig.settings.latexExternalEditorAutoConfirm,
                     PreferenceKey::LatexExternalEditorAutoConfirm);
    adw_preferences_page_add(latexPage, externalEditorGroup);

    auto* latexSaveGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(latexSaveGroup, "Pending text settings");
    adw_preferences_group_set_description(latexSaveGroup,
                                          "Apply the formula, template and editor command fields above.");
    auto* latexSaveRow = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(latexSaveRow), "Save LaTeX settings");
    adw_action_row_set_subtitle(latexSaveRow, "Switches and selectors save as you change them");
    latexSettingsApplyStatus = gtk_label_new("");
    gtk_widget_add_css_class(latexSettingsApplyStatus, "dim-label");
    gtk_widget_set_margin_end(latexSettingsApplyStatus, 8);
    adw_action_row_add_suffix(latexSaveRow, latexSettingsApplyStatus);
    latexSettingsApplyButton = gtk_button_new_with_label("Apply");
    g_signal_connect(latexSettingsApplyButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->applyLatexPreferences()) {
                             gtk_label_set_text(GTK_LABEL(self->latexSettingsApplyStatus), "Saved");
                             gtk_widget_add_css_class(self->latexSettingsApplyStatus, "success");
                             gtk_widget_remove_css_class(self->latexSettingsApplyStatus, "error");
                         }
                     }),
                     this);
    adw_action_row_add_suffix(latexSaveRow, latexSettingsApplyButton);
    adw_preferences_group_add(latexSaveGroup, GTK_WIDGET(latexSaveRow));
    adw_preferences_page_add(latexPage, latexSaveGroup);

    adw_preferences_dialog_add(preferencesDialog, latexPage);
    updateLatexPreferenceSensitivity();

    auto* inputPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_name(inputPage, "input");
    adw_preferences_page_set_title(inputPage, "Input");
    adw_preferences_page_set_icon_name(inputPage, "input-tablet-symbolic");
    auto* pressureGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(pressureGroup, "Stylus pressure");
    preferenceSwitch(pressureGroup, "Use pressure sensitivity", "Apply pen pressure to stroke width",
                     importedConfig.settings.pressureSensitivity, PreferenceKey::PressureSensitivity);
    preferenceSpin(pressureGroup, "Minimum pressure", "Pressure floor for a light stroke",
                   importedConfig.settings.minimumPressure, 0.01, 1.0, 0.01, 2, "", PreferenceKey::MinimumPressure);
    preferenceSpin(pressureGroup, "Pressure multiplier", "Scale reported pressure before drawing",
                   importedConfig.settings.pressureMultiplier, 0.05, 10.0, 0.05, 2, "×",
                   PreferenceKey::PressureMultiplier);
    adw_preferences_page_add(inputPage, pressureGroup);
    auto* buttonsGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(buttonsGroup, "Stylus buttons");
    adw_preferences_group_set_description(
            buttonsGroup, "Tool mappings apply while a side button is held; Floating Toolbox opens at the pointer.");
    static const char* const buttonActions[] = {"No action",        "Pen",  "Highlighter", "Eraser", "Hand tool",
                                                "Floating toolbox", nullptr};
    preferenceCombo(buttonsGroup, "Button 1", "First side button", buttonActions,
                    static_cast<guint>(importedConfig.settings.stylusButtonOne), PreferenceKey::StylusButtonOne);
    preferenceCombo(buttonsGroup, "Button 2", "Second side button", buttonActions,
                    static_cast<guint>(importedConfig.settings.stylusButtonTwo), PreferenceKey::StylusButtonTwo);
    adw_preferences_page_add(inputPage, buttonsGroup);
    auto* highlightGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(highlightGroup, "Pointer position highlight");
    preferenceSwitch(highlightGroup, "Highlight pointer position",
                     "Show a translucent circle around the pointer; the HighlightPosition plugin can toggle it",
                     importedConfig.settings.highlightPosition, PreferenceKey::PositionHighlight);
    preferenceColor(highlightGroup, "Circle color", "Choose the fill color and opacity",
                    importedConfig.settings.cursorHighlightColor, PreferenceKey::CursorHighlightColor);
    preferenceSpin(highlightGroup, "Circle radius", "Set the pointer highlight radius",
                   importedConfig.settings.cursorHighlightRadius, 0.0, 30.0, 1.0, 0, "px",
                   PreferenceKey::CursorHighlightRadius);
    preferenceColor(highlightGroup, "Border color", "Choose the outline color and opacity",
                    importedConfig.settings.cursorHighlightBorderColor, PreferenceKey::CursorHighlightBorderColor);
    preferenceSpin(highlightGroup, "Border width", "Set the pointer highlight outline width",
                   importedConfig.settings.cursorHighlightBorderWidth, 0.0, 30.0, 1.0, 0, "px",
                   PreferenceKey::CursorHighlightBorderWidth);
    adw_preferences_page_add(inputPage, highlightGroup);
    adw_preferences_dialog_add(preferencesDialog, inputPage);

    auto* behavior = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_name(behavior, "behavior");
    adw_preferences_page_set_title(behavior, "Behavior");
    adw_preferences_page_set_icon_name(behavior, "preferences-system-symbolic");
    auto* autosaveGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(autosaveGroup, "Autosave");
    preferenceSwitch(autosaveGroup, "Autosave changes", "Save recovery data while editing a document",
                     importedConfig.settings.autosaveEnabled, PreferenceKey::AutosaveEnabled);
    preferenceSpin(autosaveGroup, "Autosave interval", "Set to 0 to disable periodic saves",
                   importedConfig.settings.autosaveTimeoutMinutes, 0, 1440, 1, 0, "minutes",
                   PreferenceKey::AutosaveTimeout);
    adw_preferences_page_add(behavior, autosaveGroup);
    auto* navigationGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(navigationGroup, "Navigation and zoom");
    preferenceSpin(navigationGroup, "Keyboard zoom step", "Change zoom with the keyboard",
                   importedConfig.settings.zoomStep * 100.0, 0.5, 200.0, 0.5, 1, "%", PreferenceKey::ZoomStep);
    preferenceSpin(navigationGroup, "Scroll zoom step", "Change zoom with the pointer wheel",
                   importedConfig.settings.scrollZoomStep * 100.0, 0.5, 200.0, 0.5, 1, "%",
                   PreferenceKey::ScrollZoomStep);
    preferenceSpin(navigationGroup, "Preload pages before", "Render this many preceding pages after scrolling settles",
                   importedConfig.settings.preloadPagesBefore, 0, 99, 1, 0, "pages", PreferenceKey::PreloadPagesBefore);
    preferenceSpin(navigationGroup, "Preload pages after", "Render this many following pages after scrolling settles",
                   importedConfig.settings.preloadPagesAfter, 0, 99, 1, 0, "pages", PreferenceKey::PreloadPagesAfter);
    static const char* const preloadDetailOptions[] = {"Automatic", "Medium", "Low", nullptr};
    preferenceCombo(navigationGroup, "Preload detail", "Use the zoom threshold or always preload one LOD tier",
                    preloadDetailOptions, static_cast<guint>(importedConfig.settings.preloadDetail),
                    PreferenceKey::PreloadDetail);
    preferenceSpin(navigationGroup, "Selection edge-pan speed",
                   "Move the canvas as a selection is dragged beyond its visible edge",
                   importedConfig.settings.edgePanSpeed, 0.0, 100.0, 1.0, 0, "%", PreferenceKey::EdgePanSpeed);
    preferenceSpin(navigationGroup, "Selection edge-pan multiplier",
                   "Increase panning speed as more of the selection moves out of view",
                   importedConfig.settings.edgePanMaxMultiplier, 0.1, 10.0, 0.5, 1, "×",
                   PreferenceKey::EdgePanMaxMultiplier);
    adw_preferences_page_add(behavior, navigationGroup);
    auto* renderingGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(renderingGroup, "Canvas rendering");
    preferenceSpin(renderingGroup, "Full-detail cache", "Maximum number of full-resolution pages to keep",
                   importedConfig.settings.fullDetailCachePages, 1, 8, 1, 0, "pages",
                   PreferenceKey::FullDetailCachePages);
    preferenceSpin(renderingGroup, "Medium-detail cache", "Maximum pages kept at medium detail",
                   importedConfig.settings.mediumDetailCachePages, 1, 64, 1, 0, "pages",
                   PreferenceKey::MediumDetailCachePages);
    preferenceSpin(renderingGroup, "Medium-detail memory budget", "Maximum memory used by medium rasters",
                   importedConfig.settings.mediumDetailCacheMegabytes, 1, 512, 1, 0, "MiB",
                   PreferenceKey::MediumDetailCacheMegabytes);
    preferenceSpin(renderingGroup, "Low-detail cache", "Maximum pages kept at low detail",
                   importedConfig.settings.lowDetailCachePages, 1, 512, 1, 0, "pages",
                   PreferenceKey::LowDetailCachePages);
    preferenceSpin(renderingGroup, "Low-detail memory budget", "Maximum memory used by low-detail rasters",
                   importedConfig.settings.lowDetailCacheMegabytes, 1, 512, 1, 0, "MiB",
                   PreferenceKey::LowDetailCacheMegabytes);
    preferenceSpin(renderingGroup, "Overview zoom threshold",
                   "At or below this zoom, keep full detail only for the pointer or active page",
                   importedConfig.settings.overviewZoomThresholdPercent, 5, 100, 1, 0, "%",
                   PreferenceKey::OverviewZoomThreshold);
    preferenceSpin(renderingGroup, "Low-detail zoom threshold",
                   "Below this zoom, neighboring and automatically preloaded pages use low detail",
                   importedConfig.settings.lowDetailZoomThresholdPercent, 5, 50, 1, 0, "%",
                   PreferenceKey::LowDetailZoomThreshold);
    adw_preferences_page_add(behavior, renderingGroup);
    auto* selectionGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(selectionGroup, "Selection");
    preferenceSwitch(selectionGroup, "Snap image rotation",
                     "Snap to 15° increments; hold Alt to temporarily reverse this setting",
                     importedConfig.settings.snapRotation, PreferenceKey::SnapRotation);
    preferenceSpin(selectionGroup, "Rotation snap tolerance", "Snap image rotation to 15° increments near a match",
                   importedConfig.settings.snapRotationTolerance, 0.0, 1.0, 0.05, 2, "",
                   PreferenceKey::RotationSnapTolerance);
    preferenceSwitch(selectionGroup, "Snap spacing to the page grid",
                     "Snap vertical-space boundaries to the current page grid", importedConfig.settings.snapGrid,
                     PreferenceKey::SnapGrid);
    preferenceSpin(selectionGroup, "Grid spacing", "Spacing used when snapping vertical-space boundaries",
                   importedConfig.settings.snapGridSize, 0.1, 1000.0, 0.1, 2, "pt", PreferenceKey::SnapGridSize);
    preferenceSpin(selectionGroup, "Grid snap tolerance", "How close a boundary must be to snap",
                   importedConfig.settings.snapGridTolerance, 0.0, 1.0, 0.05, 2, "", PreferenceKey::SnapGridTolerance);
    adw_preferences_page_add(behavior, selectionGroup);
    adw_preferences_dialog_add(preferencesDialog, behavior);

    auto* toolbarPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_name(toolbarPage, "toolbar");
    adw_preferences_page_set_title(toolbarPage, "Toolbar");
    adw_preferences_page_set_icon_name(toolbarPage, "view-grid-symbolic");
    auto* toolboxGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(toolboxGroup, "Floating toolbox rows");
    adw_preferences_group_set_description(
            toolboxGroup,
            "Reorder or remove comma-separated Xournal++ actions such as PEN, HIGHLIGHTER, LASER_POINTER, "
            "PDF_TOOL, UNDO, ZOOM_SLIDER, DRAW_ELLIPSE, FINE, COLOR(n), and DRAW_ARROW. Imported rows are "
            "saved in GTK 4 preferences when you apply changes.");
    static constexpr std::array<PreferenceKey, 4> rowKeys = {
            PreferenceKey::FloatingToolboxRow1,
            PreferenceKey::FloatingToolboxRow2,
            PreferenceKey::FloatingToolboxRow3,
            PreferenceKey::FloatingToolboxRow4,
    };
    static constexpr std::array<const char*, 4> rowTitles = {
            "Row 1",
            "Row 2",
            "Row 3",
            "Row 4",
    };
    for (std::size_t index = 0; index < rowKeys.size(); ++index) {
        const auto value = prototype::serializeFloatingToolboxRow(importedConfig.settings.floatingToolboxRows[index]);
        preferenceTextEntry(toolboxGroup, rowTitles[index],
                            "Comma-separated actions; use Plugin::ID for a registered Lua button", value,
                            rowKeys[index]);
    }
    auto* applyRow = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(applyRow), "Save toolbar layout");
    adw_action_row_set_subtitle(applyRow, "Apply the four rows and save them for future sessions");
    auto* applyButton = gtk_button_new_with_label("Apply");
    floatingToolboxApplyButton = applyButton;
    g_signal_connect(applyButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Prototype*>(data)->applyFloatingToolboxRows();
                     }),
                     this);
    adw_action_row_add_suffix(applyRow, applyButton);
    adw_preferences_group_add(toolboxGroup, GTK_WIDGET(applyRow));
    adw_preferences_page_add(toolbarPage, toolboxGroup);
    adw_preferences_dialog_add(preferencesDialog, toolbarPage);

    auto* mainRailGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(mainRailGroup, "Main tool rail");
    adw_preferences_group_set_description(
            mainRailGroup,
            "Choose and order the tools shown beside the canvas. Smart Select stays on the rail; omitted tools "
            "remain available under More tools.");
    auto* mainToolRailEntry = preferenceTextEntry(
            mainRailGroup, "Tool order", "Comma-separated actions; edit visually below",
            prototype::serializeMainToolRail(importedConfig.settings.mainToolRail), PreferenceKey::MainToolRail);
    g_signal_connect(mainToolRailEntry, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const char* raw = gtk_editable_get_text(editable);
                         const auto parsed = prototype::parseMainToolRail(raw ? raw : "");
                         if (parsed && self->pendingMainToolRail != *parsed) {
                             self->pendingMainToolRail = *parsed;
                             self->rebuildMainToolRailEditor();
                         }
                     }),
                     this);
    auto* mainRailApplyRow = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mainRailApplyRow), "Save main tool rail");
    adw_action_row_set_subtitle(mainRailApplyRow, "Apply the tool order and save it for future sessions");
    auto* mainRailApplyButton = gtk_button_new_with_label("Apply");
    mainToolRailApplyButton = mainRailApplyButton;
    g_signal_connect(mainRailApplyButton, "clicked",
                     G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->applyMainToolRail(); }),
                     this);
    adw_action_row_add_suffix(mainRailApplyRow, mainRailApplyButton);
    adw_preferences_group_add(mainRailGroup, GTK_WIDGET(mainRailApplyRow));
    adw_preferences_page_add(toolbarPage, mainRailGroup);

    auto* mainRailEditorGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(mainRailEditorGroup, "Arrange tools");
    adw_preferences_group_set_description(
            mainRailEditorGroup,
            "Drag tools into the main rail to add them. Drop within the rail to reorder; drag a tool back to "
            "More tools to remove it. Changes take effect when you apply the layout.");
    auto* mainRailEditor = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);

    auto* visibleColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    auto* visibleLabel = gtk_label_new("Main tool rail");
    gtk_widget_add_css_class(visibleLabel, "heading");
    gtk_widget_set_halign(visibleLabel, GTK_ALIGN_START);
    mainToolRailVisibleList = createMainToolRailEditorList(true);
    auto* visibleScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(visibleScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(visibleScroll), 220);
    gtk_widget_set_hexpand(visibleScroll, true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(visibleScroll), mainToolRailVisibleList);
    gtk_box_append(GTK_BOX(visibleColumn), visibleLabel);
    gtk_box_append(GTK_BOX(visibleColumn), visibleScroll);

    auto* availableColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    auto* availableLabel = gtk_label_new("More tools");
    gtk_widget_add_css_class(availableLabel, "heading");
    gtk_widget_set_halign(availableLabel, GTK_ALIGN_START);
    mainToolRailAvailableList = createMainToolRailEditorList(false);
    auto* availableScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(availableScroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(availableScroll), 220);
    gtk_widget_set_hexpand(availableScroll, true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(availableScroll), mainToolRailAvailableList);
    gtk_box_append(GTK_BOX(availableColumn), availableLabel);
    gtk_box_append(GTK_BOX(availableColumn), availableScroll);

    gtk_box_append(GTK_BOX(mainRailEditor), visibleColumn);
    gtk_box_append(GTK_BOX(mainRailEditor), availableColumn);
    adw_preferences_group_add(mainRailEditorGroup, mainRailEditor);
    rebuildMainToolRailEditor();
    adw_preferences_page_add(toolbarPage, mainRailEditorGroup);
    adw_dialog_present(ADW_DIALOG(preferencesDialog), window);
}

}  // namespace prototype::gtk4
