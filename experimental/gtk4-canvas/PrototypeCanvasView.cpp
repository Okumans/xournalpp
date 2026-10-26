#include "Prototype.h"

namespace prototype::gtk4 {

GtkWidget* Prototype::button(const char* icon, const char* tooltip, GCallback callback) {
    auto* result = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(result, tooltip);
    g_signal_connect(result, "clicked", callback, this);
    return result;
}

void Prototype::activate(GtkApplication* app) {
    AdwColorScheme colorScheme = ADW_COLOR_SCHEME_DEFAULT;
    if (options.explicitDark) {
        colorScheme = ADW_COLOR_SCHEME_FORCE_DARK;
    } else {
        switch (importedConfig.settings.themeVariant) {
            case prototype::ThemeVariant::ForceDark:
                colorScheme = ADW_COLOR_SCHEME_FORCE_DARK;
                break;
            case prototype::ThemeVariant::ForceLight:
                colorScheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
                break;
            case prototype::ThemeVariant::UseSystem:
            default:
                colorScheme = ADW_COLOR_SCHEME_DEFAULT;
                break;
        }
    }
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), colorScheme);
    window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Xournal++ · GTK 4 canvas prototype");
    gtk_window_set_default_size(GTK_WINDOW(window), options.width, options.height);
    audioPlayback = std::make_unique<prototype::AudioPlayback>(
            options.disableAudio || importedConfig.settings.disableAudio,
            [this](const std::string& message) { showAlert("Unable to play audio recording", message); },
            [this] {
                if (audioControls) {
                    syncAudioControls();
                    update();
                }
            });

    auto* view = adw_toolbar_view_new();
    contentView = view;
    auto* header = adw_header_bar_new();
    headerBarWidget = header;
    heading = adw_window_title_new("Blank notebook", "");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), heading);
    sidebarButton = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebarButton), "view-sidebar-start-symbolic");
    gtk_widget_set_tooltip_text(sidebarButton, "Show pages");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebarButton);
    searchButton =
            button("edit-find-symbolic", "Find text (Ctrl+F)",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->openSearchBar(); }));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), searchButton);
    preferencesButton =
            button("preferences-system-symbolic", "Preferences",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->openPreferences(); }));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), preferencesButton);
    buildPageLayoutMenu(header);
    const char* presentationAccels[] = {"F5", nullptr};
    const char* fullscreenAccels[] = {"F11", nullptr};
    gtk_application_set_accels_for_action(app, "win.presentation-mode", presentationAccels);
    gtk_application_set_accels_for_action(app, "win.fullscreen", fullscreenAccels);
    auto* pluginsManagerAction = g_simple_action_new("plugins-manager", nullptr);
    g_signal_connect(pluginsManagerAction, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
                         static_cast<Prototype*>(data)->openPluginManager();
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(pluginsManagerAction));
    g_object_unref(pluginsManagerAction);

    auto* pluginsMenu = g_menu_new();
    auto* pluginSections = g_menu_new();
    const auto pluginActions =
            pluginRuntime ? pluginRuntime->appendMenuSections(pluginSections, G_ACTION_MAP(window), app) : 0;
    if (pluginActions > 0) {
        g_menu_append_section(pluginsMenu, nullptr, G_MENU_MODEL(pluginSections));
    }
    auto* managerSection = g_menu_new();
    g_menu_append(managerSection, "Manage Plugins…", "win.plugins-manager");
    g_menu_append_section(pluginsMenu, nullptr, G_MENU_MODEL(managerSection));
    pluginsMenuButton = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(pluginsMenuButton), "Plugins");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(pluginsMenuButton), G_MENU_MODEL(pluginsMenu));
    gtk_widget_set_tooltip_text(pluginsMenuButton, "Plugin actions and plugin manager");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), pluginsMenuButton);
    g_object_unref(managerSection);
    g_object_unref(pluginSections);
    g_object_unref(pluginsMenu);
    openButton = button("document-open-symbolic", "Open (Ctrl+O)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                            auto* self = static_cast<Prototype*>(data);
                            self->protectDocument([self] { self->chooseOpen(); });
                        }));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), openButton);
    saveButton = button("document-save-symbolic", "Save (Ctrl+S)",
                        G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->save(); }));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), saveButton);
    saveAsButton = button("document-save-as-symbolic", "Save As (Ctrl+Shift+S)",
                          G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->chooseSave(); }));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), saveAsButton);
    exportPdfButton =
            button("document-send-symbolic", "Export as PDF (Ctrl+E)",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->chooseExportPdf(); }));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), exportPdfButton);
    undoButton = button("edit-undo-symbolic", "Undo (Ctrl+Z)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                            auto* self = static_cast<Prototype*>(data);
                            self->performHistoryStep(false);
                        }));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), undoButton);
    redoButton = button("edit-redo-symbolic", "Redo (Ctrl+Shift+Z)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                            auto* self = static_cast<Prototype*>(data);
                            self->performHistoryStep(true);
                        }));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), redoButton);
    fitPageButton = button("zoom-fit-best-symbolic", "Fit page (Ctrl+0)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                               auto* self = static_cast<Prototype*>(data);
                               self->fitted = false;
                               self->update();
                           }));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), fitPageButton);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    auto* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    toolbarWidget = toolbar;
    gtk_widget_add_css_class(toolbar, "toolbar");
    gtk_widget_set_halign(toolbar, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(toolbar, true);
    const std::array<const char*, 5> toolNames = {
            "Pen", "Highlighter", "Eraser", "Smart Select", "Hand tool",
    };
    const std::array<const char*, 5> toolIcons = {
            "document-edit-symbolic", "applications-graphics-symbolic",
            "edit-clear-symbolic",    "edit-select-all-symbolic",
            "pan-down-symbolic",
    };
    for (size_t i = 0; i < toolButtons.size(); ++i) {
        toolButtons[i] = gtk_toggle_button_new();
        gtk_button_set_child(GTK_BUTTON(toolButtons[i]), gtk_image_new_from_icon_name(toolIcons[i]));
        gtk_accessible_update_property(GTK_ACCESSIBLE(toolButtons[i]), GTK_ACCESSIBLE_PROPERTY_LABEL, toolNames[i], -1);
        gtk_widget_set_tooltip_text(toolButtons[i], toolNames[i]);
        if (i) {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(toolButtons[i]), GTK_TOGGLE_BUTTON(toolButtons[0]));
        }
        g_object_set_data(G_OBJECT(toolButtons[i]), "tool", GUINT_TO_POINTER(i));
        g_signal_connect(toolButtons[i], "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (gtk_toggle_button_get_active(button) && !self->syncingTools) {
                                 const auto index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "tool"));
                                 const auto tool = index == 3 ? Tool::SmartSelect :
                                                   index == 4 ? Tool::Hand :
                                                                static_cast<Tool>(index);
                                 self->selectTool(tool);
                             }
                         }),
                         this);
    }
    gtk_widget_set_tooltip_text(toolButtons[3],
                                "Smart Select (S); open Selection mode for rectangle, lasso, object, and PDF text");
    gtk_widget_set_tooltip_text(toolButtons[4], "Drag anywhere on the canvas to pan; keep selections active");
    floatingToolboxToolButton = gtk_toggle_button_new();
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(floatingToolboxToolButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
    gtk_button_set_child(GTK_BUTTON(floatingToolboxToolButton), gtk_image_new_from_icon_name("view-grid-symbolic"));
    gtk_widget_set_tooltip_text(floatingToolboxToolButton,
                                "Floating toolbox: click the canvas to open tools at the pointer");
    gtk_accessible_update_property(GTK_ACCESSIBLE(floatingToolboxToolButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Floating toolbox tool", -1);
    g_signal_connect(floatingToolboxToolButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (gtk_toggle_button_get_active(button) && !self->syncingTools) {
                             self->selectTool(Tool::FloatingToolbox);
                         }
                     }),
                     this);
    textButton = gtk_toggle_button_new();
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(textButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
    gtk_button_set_child(GTK_BUTTON(textButton), gtk_image_new_from_icon_name("format-text-symbolic"));
    gtk_widget_set_tooltip_text(textButton, "Text tool (T)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(textButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Text tool", -1);
    g_signal_connect(textButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingTools &&
                             (gtk_toggle_button_get_active(button) || self->state.brush.tool == Tool::Text)) {
                             self->selectTool(Tool::Text);
                         }
                     }),
                     this);
    latexButton = gtk_toggle_button_new();
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(latexButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
    gtk_button_set_child(GTK_BUTTON(latexButton), gtk_label_new("x²"));
    gtk_widget_set_tooltip_text(latexButton, "LaTeX formula tool");
    gtk_accessible_update_property(GTK_ACCESSIBLE(latexButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "LaTeX formula tool",
                                   -1);
    g_signal_connect(latexButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingTools &&
                             (gtk_toggle_button_get_active(button) || self->state.brush.tool == Tool::Latex)) {
                             self->selectTool(Tool::Latex);
                         }
                     }),
                     this);
    linkButton = gtk_toggle_button_new();
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(linkButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
    gtk_button_set_child(GTK_BUTTON(linkButton), gtk_image_new_from_icon_name("insert-link-symbolic"));
    gtk_widget_set_tooltip_text(linkButton, "Add or edit a link (L)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(linkButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Link tool", -1);
    g_signal_connect(linkButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingTools &&
                             (gtk_toggle_button_get_active(button) || self->state.brush.tool == Tool::Link)) {
                             self->selectTool(Tool::Link);
                         }
                     }),
                     this);
    for (size_t i = 0; i < laserButtons.size(); ++i) {
        laserButtons[i] = gtk_toggle_button_new();
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(laserButtons[i]), GTK_TOGGLE_BUTTON(toolButtons[0]));
        const char* icon = i == 0 ? "media-record-symbolic" : "applications-graphics-symbolic";
        const char* label = i == 0 ? "Laser Pointer Pen" : "Laser Pointer Highlighter";
        gtk_button_set_child(GTK_BUTTON(laserButtons[i]), gtk_image_new_from_icon_name(icon));
        gtk_widget_set_tooltip_text(laserButtons[i], label);
        gtk_accessible_update_property(GTK_ACCESSIBLE(laserButtons[i]), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
        g_object_set_data(G_OBJECT(laserButtons[i]), "laser-tool-index", GUINT_TO_POINTER(i));
        g_signal_connect(laserButtons[i], "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!gtk_toggle_button_get_active(button) || self->syncingTools) {
                                 return;
                             }
                             const auto index =
                                     GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "laser-tool-index"));
                             self->selectTool(index == 0 ? Tool::LaserPointerPen : Tool::LaserPointerHighlighter);
                         }),
                         this);
    }
    playObjectButton = gtk_toggle_button_new();
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(playObjectButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
    gtk_button_set_child(GTK_BUTTON(playObjectButton), gtk_image_new_from_icon_name("audio-x-generic-symbolic"));
    gtk_widget_set_tooltip_text(playObjectButton, "Play Object: click an audio-linked stroke or text object");
    gtk_accessible_update_property(GTK_ACCESSIBLE(playObjectButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Play Object tool",
                                   -1);
    g_signal_connect(playObjectButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (gtk_toggle_button_get_active(button) && !self->syncingTools) {
                             self->selectTool(Tool::PlayObject);
                         }
                     }),
                     this);
    imageButton = button("insert-image-symbolic", "Insert Image",
                         G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->chooseImage(); }));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), imageButton);

    audioControls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(audioControls, "linked");
    audioBackButton =
            button("media-seek-backward-symbolic", "Seek backward", G_CALLBACK(+[](GtkButton*, gpointer data) {
                       auto* self = static_cast<Prototype*>(data);
                       if (self->audioPlayback) {
                           const auto seconds = std::min(self->importedConfig.settings.defaultSeekTimeSeconds, 86400u);
                           self->audioPlayback->seekBySeconds(-static_cast<int>(seconds));
                       }
                   }));
    audioPauseButton =
            button("media-playback-pause-symbolic", "Pause audio playback", G_CALLBACK(+[](GtkButton*, gpointer data) {
                       auto* self = static_cast<Prototype*>(data);
                       if (!self->audioPlayback) {
                           return;
                       }
                       if (self->audioPlayback->isPaused()) {
                           self->audioPlayback->resume();
                       } else {
                           self->audioPlayback->pause();
                       }
                   }));
    audioForwardButton =
            button("media-seek-forward-symbolic", "Seek forward", G_CALLBACK(+[](GtkButton*, gpointer data) {
                       auto* self = static_cast<Prototype*>(data);
                       if (self->audioPlayback) {
                           const auto seconds = std::min(self->importedConfig.settings.defaultSeekTimeSeconds, 86400u);
                           self->audioPlayback->seekBySeconds(static_cast<int>(seconds));
                       }
                   }));
    audioStopButton =
            button("media-playback-stop-symbolic", "Stop audio playback", G_CALLBACK(+[](GtkButton*, gpointer data) {
                       auto* self = static_cast<Prototype*>(data);
                       if (self->audioPlayback) {
                           self->audioPlayback->stop();
                       }
                   }));
    for (auto* control: {audioBackButton, audioPauseButton, audioForwardButton, audioStopButton}) {
        gtk_box_append(GTK_BOX(audioControls), control);
    }
    gtk_box_append(GTK_BOX(toolbar), audioControls);
    gtk_widget_set_visible(audioControls, false);

    textFontEntry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(textFontEntry), "Font");
    gtk_editable_set_width_chars(GTK_EDITABLE(textFontEntry), 10);
    gtk_editable_set_text(GTK_EDITABLE(textFontEntry), state.textFontName.c_str());
    gtk_widget_set_tooltip_text(textFontEntry, "Text font family");
    g_signal_connect(textFontEntry, "changed", G_CALLBACK(+[](GtkEditable* entry, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const char* font = gtk_editable_get_text(entry);
                         if (font && *font) {
                             self->state.textFontName = font;
                             if (self->textEditor && self->textEditor->active()) {
                                 const auto* item = self->textEditor->item();
                                 self->textEditor->setBaseFont(self->state.textFontName, item->fontSize);
                                 self->updateTextEditorPosition();
                             } else if (self->state.brush.tool == Tool::Text && !self->state.selectedTexts.empty()) {
                                 if (self->state.updateSelectedTexts([&](prototype::TextItem& item) {
                                         item.fontName = self->state.textFontName;
                                         const auto [width, height] = prototype::textNaturalSize(item);
                                         item.layoutWidth = width;
                                         item.layoutHeight = height;
                                     })) {
                                     self->update();
                                 }
                             }
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(toolbar), textFontEntry);
    textFontSize = gtk_spin_button_new_with_range(1, 512, 1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(textFontSize), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), state.textFontSize);
    gtk_widget_set_tooltip_text(textFontSize, "Text font size");
    g_signal_connect(textFontSize, "value-changed", G_CALLBACK(+[](GtkSpinButton* spin, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->state.textFontSize = gtk_spin_button_get_value(spin);
                         if (self->textEditor && self->textEditor->active()) {
                             const auto* item = self->textEditor->item();
                             self->textEditor->setBaseFont(item->fontName, self->state.textFontSize);
                             self->updateTextEditorPosition();
                         } else if (self->state.brush.tool == Tool::Text && !self->state.selectedTexts.empty()) {
                             if (self->state.updateSelectedTexts([&](prototype::TextItem& item) {
                                     item.fontSize = self->state.textFontSize;
                                     const auto [width, height] = prototype::textNaturalSize(item);
                                     item.layoutWidth = width;
                                     item.layoutHeight = height;
                                 })) {
                                 self->update();
                             }
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(toolbar), textFontSize);
    textFormatBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    for (size_t i = 0; i < textFormatButtons.size(); ++i) {
        static constexpr const char* labels[] = {"B", "I", "U", "S", "x²", "x₂"};
        static constexpr const char* tips[] = {"Bold (Ctrl+B)",        "Italic (Ctrl+I)",
                                               "Underline (Ctrl+U)",   "Strikethrough (Ctrl+Shift+K)",
                                               "Superscript (Ctrl+.)", "Subscript (Ctrl+,)"};
        textFormatButtons[i] = gtk_button_new_with_label(labels[i]);
        gtk_widget_set_tooltip_text(textFormatButtons[i], tips[i]);
        g_object_set_data(G_OBJECT(textFormatButtons[i]), "text-format", GUINT_TO_POINTER(i));
        g_signal_connect(textFormatButtons[i], "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "text-format"));
                             self->applyTextFormat(static_cast<prototype::TextEditorOverlay::Format>(index));
                         }),
                         this);
        gtk_box_append(GTK_BOX(textFormatBox), textFormatButtons[i]);
    }
    gtk_box_append(GTK_BOX(toolbar), textFormatBox);
    gtk_widget_set_visible(textFontEntry, false);
    gtk_widget_set_visible(textFontSize, false);
    gtk_widget_set_visible(textFormatBox, false);
    colorBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    auto* colorDialog = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(colorDialog, false);
    // The button constructor takes ownership of the dialog.
    colorButton = gtk_color_dialog_button_new(colorDialog);
    gtk_widget_set_tooltip_text(colorButton, "Ink color");
    g_signal_connect(
            colorButton, "notify::rgba", G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                if (!self->syncingBrush && self->state.brush.tool != Tool::Eraser) {
                    const auto* color = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(self->colorButton));
                    const bool textContext =
                            self->state.brush.tool == Tool::Text || (self->textEditor && self->textEditor->active());
                    const std::array selectedColor{static_cast<double>(color->red), static_cast<double>(color->green),
                                                   static_cast<double>(color->blue)};
                    if (textContext) {
                        self->state.textColor = selectedColor;
                        if (self->state.brush.tool == Tool::Text) {
                            self->state.brush.color = selectedColor;
                        }
                        self->applyTextColor(Prototype::packTextColor(selectedColor));
                    } else {
                        self->state.brush.color = selectedColor;
                    }
                    self->input.cancel();
                    self->update();
                }
            })),
            this);
    gtk_box_append(GTK_BOX(colorBox), colorButton);
    gtk_box_append(GTK_BOX(colorBox), createPalette());
    gtk_box_append(GTK_BOX(toolbar), colorBox);

    const char* const eraserModes[] = {"Standard", "Whiteout", "Delete Stroke", nullptr};
    eraserModeDropdown = gtk_drop_down_new_from_strings(eraserModes);
    gtk_widget_set_tooltip_text(eraserModeDropdown, "Eraser mode");
    g_signal_connect(eraserModeDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingBrush) {
                             const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->eraserModeDropdown));
                             if (selected <= 2) {
                                 self->input.cancel();
                                 self->state.eraserMode = static_cast<prototype::EraserMode>(selected);
                                 self->update();
                             }
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(toolbar), eraserModeDropdown);

    gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    const char* const sizeLabels[] = {"Very Fine", "Fine", "Medium", "Thick", "Very Thick", "Custom", nullptr};
    sizeDropdown = gtk_drop_down_new_from_strings(sizeLabels);
    gtk_widget_set_tooltip_text(sizeDropdown, "Tool size preset");
    g_signal_connect(sizeDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingBrush) {
                             const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->sizeDropdown));
                             if (selected < 5) {
                                 const auto width = prototype::widthsForTool(self->state.brush.tool)[selected];
                                 self->input.cancel();
                                 self->state.brush.width = width;
                                 if (self->state.brush.tool == Tool::Eraser) {
                                     self->eraserBrush.width = width;
                                     self->state.eraserWidth = width;
                                 }
                                 self->syncingBrush = true;
                                 gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->widthButton), width);
                                 self->syncingBrush = false;
                                 self->update();
                             }
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(toolbar), sizeDropdown);
    const char* const lineStyleLabels[] = {"Solid", "Dashed", "Dash dot", "Dotted", "Custom", "Mixed", nullptr};
    lineStyleDropdown = gtk_drop_down_new_from_strings(lineStyleLabels);
    gtk_widget_set_tooltip_text(lineStyleDropdown, "Pen line style");
    g_signal_connect(lineStyleDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->syncingBrush) {
                             return;
                         }
                         const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->lineStyleDropdown));
                         std::string style;
                         switch (selected) {
                             case 0:
                                 style = "plain";
                                 break;
                             case 1:
                                 style = "dash";
                                 break;
                             case 2:
                                 style = "dashdot";
                                 break;
                             case 3:
                                 style = "dot";
                                 break;
                             default:
                                 break;
                         }
                         if (!style.empty()) {
                             std::string error;
                             if (!self->changePluginActionState("tool-pen-line-style", std::move(style), error)) {
                                 std::cerr << "xournalpp-gtk4-canvas: " << error << '\n';
                             }
                         } else {
                             self->syncingBrush = true;
                             self->syncPenLineStyleDropdown();
                             self->syncingBrush = false;
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(toolbar), lineStyleDropdown);
    widthButton = gtk_spin_button_new_with_range(0.2, 36.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widthButton), 2);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widthButton), true);
    gtk_editable_set_width_chars(GTK_EDITABLE(widthButton), 5);
    gtk_widget_set_tooltip_text(widthButton, "Stroke or eraser width in document points");
    g_signal_connect(widthButton, "value-changed", G_CALLBACK(+[](GtkSpinButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingBrush) {
                             self->input.cancel();
                             self->state.brush.width = gtk_spin_button_get_value(button);
                             if (self->state.brush.tool == Tool::Eraser) {
                                 self->eraserBrush.width = self->state.brush.width;
                                 self->state.eraserWidth = self->state.brush.width;
                             }
                             self->syncingBrush = true;
                             self->syncSizeDropdown();
                             self->syncingBrush = false;
                             self->update();
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(toolbar), widthButton);

    auto* drawingOptions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(drawingOptions, 10);
    gtk_widget_set_margin_bottom(drawingOptions, 10);
    gtk_widget_set_margin_start(drawingOptions, 10);
    gtk_widget_set_margin_end(drawingOptions, 10);

    const char* const drawingModes[] = {"Freehand", "Line",         "Rectangle",       "Ellipse",
                                        "Arrow",    "Double arrow", "Coordinate axes", "Shape recognizer",
                                        "Spline",   nullptr};
    drawingModeDropdown = gtk_drop_down_new_from_strings(drawingModes);
    gtk_widget_set_tooltip_text(drawingModeDropdown, "Drawing tool");
    g_signal_connect(drawingModeDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingBrush) {
                             const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->drawingModeDropdown));
                             if (selected <= static_cast<guint>(prototype::DrawingMode::Spline)) {
                                 self->input.cancel();
                                 self->state.drawingMode = static_cast<prototype::DrawingMode>(selected);
                                 const bool supportsStabilizer =
                                         self->state.drawingMode == prototype::DrawingMode::Freehand ||
                                         self->state.drawingMode == prototype::DrawingMode::ShapeRecognizer;
                                 gtk_widget_set_visible(self->averagingDropdown, supportsStabilizer);
                                 gtk_widget_set_visible(self->preprocessorDropdown, supportsStabilizer);
                                 self->update();
                             }
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(drawingOptions), gtk_label_new("Tool"));
    gtk_box_append(GTK_BOX(drawingOptions), drawingModeDropdown);

    const char* const averagingModes[] = {"No smoothing", "Arithmetic", "Velocity Gaussian", nullptr};
    averagingDropdown = gtk_drop_down_new_from_strings(averagingModes);
    gtk_widget_set_tooltip_text(averagingDropdown, "Stroke averaging; defaults to the Xournal++ no-stabilizer setting");
    g_signal_connect(averagingDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingBrush) {
                             const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->averagingDropdown));
                             if (selected <= static_cast<guint>(prototype::AveragingMethod::VelocityGaussian)) {
                                 self->input.cancel();
                                 auto config = self->input.stabilizerConfig;
                                 config.averaging = static_cast<prototype::AveragingMethod>(selected);
                                 self->input.setStabilizerConfig(config);
                             }
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(drawingOptions), gtk_label_new("Averaging"));
    gtk_box_append(GTK_BOX(drawingOptions), averagingDropdown);

    const char* const preprocessors[] = {"No preprocessor", "Deadzone", "Inertia", nullptr};
    preprocessorDropdown = gtk_drop_down_new_from_strings(preprocessors);
    gtk_widget_set_tooltip_text(preprocessorDropdown, "Stroke preprocessor");
    g_signal_connect(preprocessorDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->syncingBrush) {
                             const auto selected =
                                     gtk_drop_down_get_selected(GTK_DROP_DOWN(self->preprocessorDropdown));
                             if (selected <= static_cast<guint>(prototype::StrokePreprocessor::Inertia)) {
                                 self->input.cancel();
                                 auto config = self->input.stabilizerConfig;
                                 config.preprocessor = static_cast<prototype::StrokePreprocessor>(selected);
                                 self->input.setStabilizerConfig(config);
                             }
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(drawingOptions), gtk_label_new("Preprocessor"));
    gtk_box_append(GTK_BOX(drawingOptions), preprocessorDropdown);
    drawingOptionsExpander = gtk_expander_new("Drawing tools and stabilization");
    gtk_expander_set_child(GTK_EXPANDER(drawingOptionsExpander), drawingOptions);
    gtk_box_append(GTK_BOX(paletteContent), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(paletteContent), drawingOptionsExpander);
    gtk_widget_set_tooltip_text(paletteButton, "Quick colors and drawing tools");

    const char* selectionModes[] = {"Rectangle",
                                    "Lasso",
                                    "Smart",
                                    "Rectangle: Visible layers",
                                    "Lasso: Visible layers",
                                    "Object",
                                    "PDF text: Linear",
                                    "PDF text: Rectangle",
                                    "Vertical space",
                                    nullptr};
    selectionModeDropdown = gtk_drop_down_new_from_strings(selectionModes);
    gtk_widget_set_tooltip_text(selectionModeDropdown,
                                "Object selects the nearest item in the active layer. Visible-layer modes "
                                "search from the top visible layer downward and select editable objects on "
                                "the first layer with matches. Vertical space shifts objects wholly above or "
                                "below a dragged boundary; hold Control to switch sides.");
    g_signal_connect(
            selectionModeDropdown, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
                const auto tool = selected == 8                  ? Tool::VerticalSpace :
                                  selected == 1 || selected == 4 ? Tool::SelectLasso :
                                  selected == 2                  ? Tool::SmartSelect :
                                  selected == 5                  ? Tool::SelectObject :
                                  selected == 6                  ? Tool::SelectPdfTextLinear :
                                  selected == 7                  ? Tool::SelectPdfTextRect :
                                                                   Tool::SelectRectangle;
                const bool acrossLayers = selected == 3 || selected == 4;
                if (tool == Tool::VerticalSpace) {
                    self->selectTool(tool);
                    return;
                }
                if (prototype::isPdfTextSelectionTool(self->state.brush.tool) &&
                    prototype::isPdfTextSelectionTool(tool)) {
                    if (self->state.brush.tool != tool) {
                        self->switchPdfTextSelectionStyle();
                    }
                } else if ((prototype::isSelectionTool(self->state.brush.tool) ||
                            self->state.brush.tool == Tool::VerticalSpace) &&
                           (tool != self->state.brush.tool || acrossLayers != self->state.selectionAcrossLayers)) {
                    self->activateSelection(tool, acrossLayers);
                }
            }),
            this);
    gtk_box_append(GTK_BOX(toolbar), selectionModeDropdown);
    selectionActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(selectionActions, "linked");
    gtk_box_append(GTK_BOX(selectionActions),
                   button("edit-copy-symbolic", "Copy selection (Ctrl+C)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                              static_cast<Prototype*>(data)->copySelection();
                          })));
    gtk_box_append(GTK_BOX(selectionActions),
                   button("edit-cut-symbolic", "Cut selection (Ctrl+X)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                              static_cast<Prototype*>(data)->copySelection(true);
                          })));
    gtk_box_append(
            GTK_BOX(selectionActions),
            button("edit-paste-symbolic", "Paste selection or image (Ctrl+V)",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->pasteSelection(); })));
    gtk_box_append(GTK_BOX(selectionActions),
                   button("edit-duplicate-symbolic", "Duplicate selection", G_CALLBACK(+[](GtkButton*, gpointer data) {
                              static_cast<Prototype*>(data)->duplicateSelection();
                          })));
    gtk_box_append(GTK_BOX(toolbar), selectionActions);
    selectionLabel = gtk_label_new("Drag to select");
    gtk_widget_set_tooltip_text(selectionLabel, "Drag to move, drag a square to resize, or drag the circle to rotate");
    gtk_box_append(GTK_BOX(toolbar), selectionLabel);
    gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    auto* navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(navigation, "linked");
    prevPageButton = button("go-previous-symbolic", "Previous page (Page Up / Shift+K)",
                            G_CALLBACK(+[](GtkButton*, gpointer data) {
                                auto* self = static_cast<Prototype*>(data);
                                self->navigateToPage(self->state.page - 1);
                            }));
    gtk_box_append(GTK_BOX(navigation), prevPageButton);
    pageButton = gtk_button_new();
    gtk_widget_add_css_class(pageButton, "flat");
    gtk_widget_set_tooltip_text(pageButton, "Go to page (Ctrl+G or :)");
    pageLabel = gtk_label_new("");
    gtk_button_set_child(GTK_BUTTON(pageButton), pageLabel);
    g_signal_connect(
            pageButton, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->openGotoPageDialog(); }), this);
    gtk_box_append(GTK_BOX(navigation), pageButton);
    nextPageButton =
            button("go-next-symbolic", "Next page (Page Down / Shift+J)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                       auto* self = static_cast<Prototype*>(data);
                       self->navigateToPage(self->state.page + 1);
                   }));
    gtk_box_append(GTK_BOX(navigation), nextPageButton);
    gtk_box_append(GTK_BOX(navigation), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    insertPageButton = button(
            "list-add-symbolic", "Insert page after (Ctrl+D)",
            G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->insertPageAfterCurrent(); }));
    gtk_box_append(GTK_BOX(navigation), insertPageButton);
    deletePageButton =
            button("list-remove-symbolic", "Delete current page (Ctrl+Shift+Delete)",
                   G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->deleteCurrentPage(); }));
    gtk_box_append(GTK_BOX(navigation), deletePageButton);
    zoomLabel = gtk_label_new("");
    gtk_widget_set_size_request(zoomLabel, 60, -1);
    gtk_box_append(GTK_BOX(navigation), zoomLabel);
    gtk_box_append(GTK_BOX(toolbar), navigation);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), toolbar);
    auto* searchBar = searchController.createWidget();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), searchBar);
    mainToolRailWidget = createMainToolRail();
    selectTool(importedConfig.settings.activeTool);

    canvas = gtk_drawing_area_new();
    canvasRenderer.setRedrawWidget(canvas);
    gtk_widget_set_focusable(canvas, true);
    gtk_widget_set_hexpand(canvas, true);
    gtk_widget_set_vexpand(canvas, true);
    canvasOverlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(canvasOverlay), canvas);
    textEditor = std::make_unique<prototype::TextEditorOverlay>(GTK_OVERLAY(canvasOverlay));
    textEditor->setLayoutChangedCallback([this] { updateTextEditorPosition(); });
    textEditor->setWrap(false);
    createPdfActionsPopover();
    createLinkInfoPopover();
    createFloatingToolbox();
    gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(canvas),
            +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                try {
                    self->draw(cr, width, height);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << '\n';
                    self->failed = true;
                    g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                }
            },
            this, nullptr);
    g_signal_connect(canvas, "resize", G_CALLBACK(+[](GtkDrawingArea*, int width, int height, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->fitMode || !self->fitted) {
                             self->input.cancel();
                             self->fitCurrentView(width, height);
                             self->fitted = true;
                             self->fitMode = true;
                             self->lastWidth = width;
                             self->lastHeight = height;
                             self->update();
                         }
                         if (gtk_widget_get_visible(self->floatingToolbox)) {
                             self->positionFloatingToolbox(self->floatingToolboxAnchor.x,
                                                           self->floatingToolboxAnchor.y);
                         }
                     }),
                     this);
    splitView = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_content(splitView, canvasOverlay);
    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
    adw_overlay_split_view_set_min_sidebar_width(splitView, 160);
    adw_overlay_split_view_set_max_sidebar_width(splitView, 180);
    // Sidebar edge swipes could steal drawing gestures. Its button is explicit.
    adw_overlay_split_view_set_enable_show_gesture(splitView, false);
    adw_overlay_split_view_set_enable_hide_gesture(splitView, false);
    adw_overlay_split_view_set_show_sidebar(splitView, importedConfig.settings.showSidebar);
    g_object_bind_property(splitView, "show-sidebar", sidebarButton, "active",
                           GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
    g_signal_connect(splitView, "notify::show-sidebar", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->applyingViewMode || self->presentationMode || self->fullscreenModeActive) {
                             return;
                         }
                         auto& settings = self->importedConfig.settings;
                         settings.showSidebar = adw_overlay_split_view_get_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(object));
                         settings.viewModes[static_cast<std::size_t>(prototype::ViewModePreset::Default)].showSidebar =
                                 settings.showSidebar;
                         std::string error;
                         if (!prototype::saveGtk4Preferences(self->preferencesConfigDir, settings, error)) {
                             self->showAlert("Could not save sidebar preference", error);
                         }
                     }),
                     this);
    auto* adaptive = adw_breakpoint_bin_new();
    gtk_widget_set_size_request(adaptive, 320, 240);
    auto* workspace = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(workspace), mainToolRailWidget);
    gtk_box_append(GTK_BOX(workspace), GTK_WIDGET(splitView));
    adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(adaptive), workspace);
    auto* breakpoint = adw_breakpoint_new(
            adw_breakpoint_condition_new_length(ADW_BREAKPOINT_CONDITION_MAX_WIDTH, 720, ADW_LENGTH_UNIT_PX));
    adw_breakpoint_add_setters(breakpoint, G_OBJECT(splitView), "collapsed", TRUE, nullptr);
    adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(adaptive), breakpoint);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), adaptive);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), view);
    applyCurrentViewMode();
    connectInput();
    g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
                         auto* self = static_cast<Prototype*>(data);
                         self->finishTextEditing();
                         if (self->busy || self->choosingFile || self->messageDialog) {
                             return true;
                         }
                         if (self->autosaving) {
                             self->waitForAutosave([self] { self->protectDocument([self] { self->closeWindow(); }); });
                             return true;
                         }
                         self->protectDocument([self] { self->closeWindow(); });
                         return true;
                     }),
                     this);
    g_signal_connect(window, "notify::is-active", G_CALLBACK(+[](GObject* window, GParamSpec*, gpointer data) {
                         if (!gtk_window_is_active(GTK_WINDOW(window))) {
                             auto* self = static_cast<Prototype*>(data);
                             self->resetInput(true);
                             self->update();
                         }
                     }),
                     this);
    g_signal_connect(canvas, "unmap",
                     G_CALLBACK(+[](GtkWidget*, gpointer data) { static_cast<Prototype*>(data)->resetInput(true); }),
                     this);
    update();
    windowPaintable = gtk_widget_paintable_new(window);
    gtk_window_present(GTK_WINDOW(window));
    paintClock = GDK_FRAME_CLOCK(g_object_ref(gtk_widget_get_frame_clock(window)));
    g_signal_connect(paintClock, "after-paint",
                     G_CALLBACK(+[](GdkFrameClock*, gpointer data) { ++static_cast<Prototype*>(data)->paints; }), this);
    gtk_widget_grab_focus(canvas);
    if (!options.worker.empty()) {
        scheduleAutosave(static_cast<guint>(std::max(0, effectiveAutosaveSeconds)));
        requestOpenDocument(sourcePath, true);
    }
    if (options.verifyPlugins || options.verifySearch || options.verifyScrollMotion ||
        options.verifySidebarPerformance || options.verifySidebarStartup || options.verifyPageLayout || options.verifyPresentationMode ||
        options.verifyPenLineStyle || options.verifyBeamer || options.verifyLayerActions || options.verifyLatex) {
        g_timeout_add(
                200,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    if (self->busy || (self->options.worker.size() && !self->nativeSession)) {
                        return G_SOURCE_CONTINUE;
                    }
                    try {
                        if (self->options.verifySearch) {
                            self->verifySearchIntegration();
                        } else if (self->options.verifyScrollMotion) {
                            self->verifyScrollMotionIntegration();
                        } else if (self->options.verifySidebarPerformance) {
                            self->verifySidebarPerformanceIntegration();
                        } else if (self->options.verifySidebarStartup) {
                            self->verifySidebarStartupIntegration();
                        } else if (self->options.verifyPageLayout) {
                            self->verifyPageLayoutIntegration();
                        } else if (self->options.verifyPresentationMode) {
                            self->verifyPresentationModeIntegration();
                        } else if (self->options.verifyPenLineStyle) {
                            self->verifyPenLineStyleIntegration();
                        } else if (self->options.verifyPlugins) {
                            self->verifyPluginIntegration();
                        } else if (self->options.verifyBeamer) {
                            self->verifyBeamerIntegration();
                        } else if (self->options.verifyLayerActions) {
                            self->verifyLayerActionsIntegration();
                        } else {
                            self->verifyLatexIntegration();
                        }
                    } catch (const std::exception& error) {
                        const char* verification = self->options.verifyLatex              ? "LATEX" :
                                                   self->options.verifySearch             ? "SEARCH" :
                                                   self->options.verifyScrollMotion       ? "SCROLL MOTION" :
                                                   self->options.verifySidebarPerformance ? "SIDEBAR PERFORMANCE" :
                                                   self->options.verifySidebarStartup     ? "SIDEBAR STARTUP" :
                                                   self->options.verifyPresentationMode   ? "PRESENTATION MODE" :
                                                   self->options.verifyPageLayout         ? "PAGE LAYOUT" :
                                                   self->options.verifyPenLineStyle       ? "PEN LINE STYLE" :
                                                   self->options.verifyBeamer             ? "BEAMER" :
                                                   self->options.verifyLayerActions       ? "LAYER ACTIONS" :
                                                                                            "PLUGIN";
                        std::cerr << verification << " VERIFY FAILED: " << error.what() << '\n';
                        self->failed = true;
                    }
                    g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                    return G_SOURCE_REMOVE;
                },
                this);
    }
    if (const char* marker = g_getenv("XOURNALPP_CRASH_AFTER_AUTOSAVE_MARKER"); marker && *marker) {
        crashMarkerPath = marker;
        crashHelperSource = g_timeout_add(
                50,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    try {
                        return self->stepCrashHelper();
                    } catch (const std::exception& error) {
                        std::cerr << "CRASH HELPER FAILED: " << error.what() << '\n';
                        self->failed = true;
                        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                        self->crashHelperSource = 0;
                        return G_SOURCE_REMOVE;
                    }
                },
                this);
    }
    if (const char* action = g_getenv("XOURNALPP_RECOVERY_ACTION"); action && *action) {
        recoveryActionSpec = action;
        recoveryVerifySource = g_timeout_add(
                80,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    try {
                        return self->stepRecoveryVerify();
                    } catch (const std::exception& error) {
                        std::cerr << "RECOVERY VERIFY FAILED: " << error.what() << '\n';
                        self->failed = true;
                        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                        self->recoveryVerifySource = 0;
                        return G_SOURCE_REMOVE;
                    }
                },
                this);
    }
    if (!options.smokeDirectory.empty() && !options.verifyPlugins && !options.verifySearch &&
        !options.verifyScrollMotion &&
        !options.verifySidebarPerformance && !options.verifySidebarStartup && !options.verifyPageLayout &&
        !options.verifyPresentationMode && !options.verifyPenLineStyle && !options.verifyBeamer &&
        !options.verifyLayerActions && !options.verifyLatex) {
        if (const char* verifyMode = g_getenv("XOURNALPP_SETTINGS_VERIFY_MODE"); verifyMode && *verifyMode) {
            settingsVerifyMode = verifyMode;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_PREFERENCES_VERIFY_MODE"); verifyMode && *verifyMode) {
            preferencesVerifyMode = verifyMode;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_LAYER_VERIFY_MODE"); verifyMode && *verifyMode) {
            layerVerifyMode = verifyMode;
            layerVerifyStep = 0;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_IMAGE_VERIFY_MODE"); verifyMode && *verifyMode) {
            imageVerifyMode = verifyMode;
            imageVerifySource =
                    g_getenv("XOURNALPP_IMAGE_VERIFY_SOURCE") ? g_getenv("XOURNALPP_IMAGE_VERIFY_SOURCE") : "";
            imageVerifyStep = 0;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_SELECTION_VERIFY_MODE"); verifyMode && *verifyMode) {
            selectionVerifyMode = verifyMode;
            selectionVerifyStep = 0;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_PDF_TEXT_SELECTION_VERIFY_MODE"); verifyMode && *verifyMode) {
            pdfTextSelectionVerifyMode = verifyMode;
            pdfTextSelectionVerifyStep = 0;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_LINK_VERIFY_MODE"); verifyMode && *verifyMode) {
            linkVerifyMode = verifyMode;
            linkVerifyStep = 0;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_LASER_VERIFY_MODE"); verifyMode && *verifyMode) {
            laserVerifyMode = verifyMode;
            laserVerifyStep = 0;
        }
        if (const char* verifyMode = g_getenv("XOURNALPP_PLAY_OBJECT_VERIFY_MODE"); verifyMode && *verifyMode) {
            playObjectVerifyMode = verifyMode;
            playObjectVerifyStep = 0;
        }
        heartbeatSource = g_timeout_add(
                10,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    if (self->busy) {
                        ++self->busyHeartbeats;
                    }
                    if (self->autosaving) {
                        ++self->autosaveHeartbeats;
                    }
                    return G_SOURCE_CONTINUE;
                },
                this);
        g_timeout_add(
                150,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    try {
                        return self->smoke();
                    } catch (const SnapshotPending&) {
                        if (++self->snapshotWaits < 60) {
                            if (!self->settingsVerifyMode.empty()) {
                                --self->settingsVerifyStep;
                            } else if (!self->preferencesVerifyMode.empty()) {
                                --self->preferencesVerifyStep;
                            } else if (!self->imageVerifyMode.empty()) {
                                --self->imageVerifyStep;
                            } else if (!self->layerVerifyMode.empty()) {
                                --self->layerVerifyStep;
                            } else if (!self->selectionVerifyMode.empty()) {
                                --self->selectionVerifyStep;
                            } else if (!self->pdfTextSelectionVerifyMode.empty()) {
                                --self->pdfTextSelectionVerifyStep;
                            } else if (!self->laserVerifyMode.empty()) {
                                --self->laserVerifyStep;
                            } else if (!self->playObjectVerifyMode.empty()) {
                                --self->playObjectVerifyStep;
                            } else if (self->documentSmokeStep >= 0) {
                                --self->documentSmokeStep;
                            } else {
                                --self->smokeStep;
                            }
                            self->lastSmokePaint = self->paints;
                            gtk_widget_queue_draw(self->window);
                            return G_SOURCE_CONTINUE;
                        }
                        std::cerr << "SMOKE FAILED: window snapshot never became ready\n";
                        self->failed = true;
                        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                        return G_SOURCE_REMOVE;
                    } catch (const std::exception& error) {
                        std::cerr << "SMOKE FAILED: " << error.what() << '\n';
                        self->failed = true;
                        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                        return G_SOURCE_REMOVE;
                    }
                },
                this);
    }
}

void Prototype::update() {
    if (closed) {
        return;
    }
    if (state.page != lastPreloadPage) {
        scheduleCanvasPagePreload();
    }
    if (pdfTextSelection && pdfTextSelection->finalized && state.page != pdfTextSelection->page) {
        clearPdfTextSelection(false);
    }
    if (pdfActionsObservedZoom && std::abs(*pdfActionsObservedZoom - state.zoom) > 1e-9 && pdfTextSelection &&
        pdfTextSelection->finalized) {
        hidePdfActionsPreservingSelection();
    }
    pdfActionsObservedZoom = state.zoom;
    ensureSearchMatchesFresh();
    const auto& pageLayers = state.layersForPage(state.page);
    if (layerRows && (layerPanelPage != state.page || !layerPanelSnapshot || *layerPanelSnapshot != pageLayers)) {
        rebuildLayerStack();
    }
    updateTextEditorPosition();
    updateTitle();
    if (selectionLabel) {
        std::string selectionStatus =
                state.hasSelection() ?
                        std::to_string(state.selectionCount()) + " selected" :
                pdfTextSelection && pdfTextSelection->finalized ?
                        std::to_string(g_utf8_strlen(pdfTextSelection->text.c_str(), -1)) + " PDF characters selected" :
                pdfTextSelection && pdfTextSelection->dragging ? "Selecting PDF text" :
                state.brush.tool == Tool::SelectObject         ? "Click to select" :
                                                                 "Drag to select";
        gtk_label_set_text(GTK_LABEL(selectionLabel), selectionStatus.c_str());
    }
    if (lineStyleDropdown) {
        gtk_widget_set_visible(lineStyleDropdown, state.brush.tool == Tool::Pen || selectedPenStrokeCount() > 0);
    }
    if (pdfTextSelection && pdfTextSelection->finalized && pdfActionsPopover &&
        gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover))) {
        updatePdfActionsPosition(false);
    }
    gtk_widget_set_sensitive(openButton, !options.worker.empty());
    gtk_widget_set_sensitive(saveButton, bool(nativeSession));
    gtk_widget_set_sensitive(saveAsButton, bool(nativeSession));
    gtk_widget_set_sensitive(exportPdfButton, bool(nativeSession));
    gtk_widget_set_sensitive(textButton, bool(nativeSession));
    gtk_widget_set_sensitive(latexButton, bool(nativeSession));
    gtk_widget_set_sensitive(playObjectButton, bool(nativeSession));
    gtk_widget_set_sensitive(imageButton, bool(nativeSession));
    gtk_widget_set_sensitive(prevPageButton, state.page > 0);
    gtk_widget_set_sensitive(nextPageButton, state.page + 1 < pageCount);
    gtk_widget_set_sensitive(insertPageButton, bool(nativeSession));
    gtk_widget_set_sensitive(deletePageButton, bool(nativeSession) && pageCount > 1);
    const auto label = std::to_string(state.page + 1) + " / " + std::to_string(pageCount);
    gtk_label_set_text(GTK_LABEL(pageLabel), label.c_str());
    gtk_label_set_text(GTK_LABEL(zoomLabel), (std::to_string(static_cast<int>(state.zoom * 100)) + "%").c_str());
    gtk_widget_set_sensitive(undoButton, !busy && state.canUndo());
    gtk_widget_set_sensitive(redoButton, !busy && state.canRedo());
    const bool wasSyncingBrush = syncingBrush;
    syncingBrush = true;
    if (drawingModeDropdown &&
        gtk_drop_down_get_selected(GTK_DROP_DOWN(drawingModeDropdown)) != static_cast<guint>(state.drawingMode)) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown), static_cast<guint>(state.drawingMode));
    }
    const auto floatingModeIndex = static_cast<guint>(prototype::floatingDrawingModeComboIndex(state.drawingMode));
    for (auto* selector: floatingDrawingModeSelectors) {
        if (gtk_drop_down_get_selected(selector) != floatingModeIndex) {
            gtk_drop_down_set_selected(selector, floatingModeIndex);
        }
    }
    if (toolSupportsSelectionFill() && !state.hasSelectedFillableInk()) {
        selectionFillEnabled = false;
    }
    const bool fillAvailable = fillToggleAvailable();
    const bool fillActive = fillToggleActive();
    for (auto* toggle: floatingFillToggles) {
        gtk_widget_set_sensitive(GTK_WIDGET(toggle), fillAvailable);
        if (gtk_toggle_button_get_active(toggle) != fillActive) {
            gtk_toggle_button_set_active(toggle, fillActive);
        }
    }
    const bool fillOpacityAvailableNow = fillOpacityAvailable();
    for (auto* button: floatingFillOpacityButtons) {
        gtk_widget_set_sensitive(GTK_WIDGET(button), fillOpacityAvailableNow);
    }
    syncPenLineStyleDropdown();
    syncingBrush = wasSyncingBrush;
    updateFloatingPageSpinners();
    updateFloatingLayerMenus();
    if (pageList) {
        auto* selection = GTK_SINGLE_SELECTION(gtk_list_view_get_model(pageList));
        const bool pageChanged = gtk_single_selection_get_selected(selection) != static_cast<guint>(state.page);
        syncingPage = true;
        gtk_single_selection_set_selected(selection, static_cast<guint>(state.page));
        syncingPage = false;
        if (pageChanged) {
            scrollPageSidebarToCurrent();
        }
    }
    for (auto* thumbnail: thumbnailWidgets) {
        if (gtk_widget_get_mapped(thumbnail)) {
            gtk_widget_queue_draw(thumbnail);
        }
    }
    gtk_widget_queue_draw(canvas);
    syncAudioControls();
    ensureLaserFadeTimer();
}

void Prototype::updateFloatingPageSpinners() {
    const bool wasSyncingPage = syncingPage;
    syncingPage = true;

    const int count = std::max(0, pageCount);
    const int current = count > 0 ? std::clamp(state.page + 1, 1, count) : 0;
    std::string status = "of " + std::to_string(count);
    if (nativeSession && state.page >= 0 && static_cast<std::size_t>(state.page) < nativeSession->pdfPageMap.size()) {
        const int pdfPage = nativeSession->pdfPageMap[static_cast<std::size_t>(state.page)];
        if (pdfPage >= 0) {
            status += ", PDF Page " + std::to_string(pdfPage + 1);
        }
    }

    for (const auto& [spin, label]: floatingPageSpinners) {
        gtk_spin_button_set_range(spin, count > 0 ? 1 : 0, count);
        if (spin != pendingFloatingPageSpin) {
            gtk_spin_button_set_value(spin, current);
        }
        gtk_label_set_text(label, status.c_str());
        gtk_widget_set_sensitive(GTK_WIDGET(spin), count > 0);
    }

    syncingPage = wasSyncingPage;
}

void Prototype::ensureLaserFadeTimer() {
    if (laserFadeSource || !state.hasLaserFades()) {
        return;
    }
    laserFadeSource = g_timeout_add(
            prototype::behavior::laserPointerFadeStepMs,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                if (!self->state.hasLaserFades()) {
                    self->laserFadeSource = 0;
                    return G_SOURCE_REMOVE;
                }
                if (self->state.advanceLaserFades()) {
                    self->update();
                }
                if (!self->state.hasLaserFades()) {
                    self->laserFadeSource = 0;
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            },
            this);
}

void Prototype::zoom(double value, double x, double y, bool previewRaster) {
    cancelScrollMomentum();
    if (presentationMode || input.active()) {
        return;
    }
    if (zoomPreviewSource) {
        g_source_remove(zoomPreviewSource);
        zoomPreviewSource = 0;
    }
    zoomPreview = previewRaster;
    state.zoomAt(value, x, y);
    fitMode = false;
    fitted = true;
    update();
    if (previewRaster) {
        zoomPreviewSource = g_timeout_add(
                120,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    self->zoomPreviewSource = 0;
                    self->zoomPreview = false;
                    self->update();
                    return G_SOURCE_REMOVE;
                },
                this);
    }
}

void Prototype::finishZoomPreview() {
    if (zoomPreviewSource) {
        g_source_remove(zoomPreviewSource);
        zoomPreviewSource = 0;
    }
    zoomPreview = false;
}

bool Prototype::mouseEvent(GtkEventController* controller) {
    auto* event = gtk_event_controller_get_current_event(controller);
    if (!event) {
        return true;  // Signal-level integration tests have no backend event.
    }
    auto* device = gdk_event_get_device(event);
    return !gdk_event_get_device_tool(event) && device && gdk_device_get_source(device) != GDK_SOURCE_TOUCHSCREEN;
}

std::optional<prototype::Point> Prototype::surfacePoint(double x, double y) const {
    auto* native = gtk_widget_get_native(canvas);
    double tx, ty;
    gtk_native_get_surface_transform(native, &tx, &ty);
    const graphene_point_t from{static_cast<float>(x + tx), static_cast<float>(y + ty)};
    graphene_point_t to;
    if (!gtk_widget_compute_point(GTK_WIDGET(native), canvas, &from, &to)) {
        return std::nullopt;
    }
    return prototype::Point{to.x, to.y};
}

prototype::Point Prototype::scrollAnchor(GtkEventController* controller) const {
    double x, y;
    auto* event = gtk_event_controller_get_current_event(controller);
    if (event && gdk_event_get_position(event, &x, &y)) {
        if (const auto point = surfacePoint(x, y)) {
            return *point;
        }
    }
    return {gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0};
}

}  // namespace prototype::gtk4
