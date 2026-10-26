#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::addFloatingToolboxToolAction(GtkWidget* row, Tool tool, const char* label) {
    auto* toolButton = gtk_toggle_button_new_with_label(label);
    gtk_widget_add_css_class(toolButton, "flat");
    gtk_widget_set_size_request(toolButton, 82, 36);
    floatingToolboxToolButtons.emplace_back(tool, toolButton);
    g_object_set_data(G_OBJECT(toolButton), "floating-toolbox-tool", GINT_TO_POINTER(static_cast<int>(tool)));
    g_signal_connect(toolButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!gtk_toggle_button_get_active(button) || self->syncingTools) {
                             return;
                         }
                         const auto selectedTool = static_cast<Tool>(
                                 GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-toolbox-tool")));
                         self->selectTool(selectedTool);
                         self->hideFloatingToolbox();
                     }),
                     this);
    gtk_box_append(GTK_BOX(row), toolButton);
}

std::string Prototype::layerDisplayName(const prototype::PageLayers& pageLayers, size_t layerId) {
    if (layerId == 0) {
        return pageLayers.backgroundName.value_or("Background");
    }
    if (layerId > pageLayers.layers.size()) {
        return "No layer";
    }
    const auto& name = pageLayers.layers[layerId - 1].name;
    return name.empty() ? "Layer " + std::to_string(layerId) : name;
}

void Prototype::appendFloatingLayerControl(FloatingLayerMenuWidgets& menu, int page, size_t layerId,
                                           const std::string& name, bool visible, bool selectable, bool active,
                                           GtkCheckButton*& activeGroup) {
    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* select = nullptr;
    if (selectable) {
        auto* check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(name.c_str()));
        gtk_check_button_set_group(check, activeGroup);
        activeGroup = check;
        gtk_check_button_set_active(check, active);
        gtk_widget_set_hexpand(GTK_WIDGET(check), true);
        gtk_widget_set_halign(GTK_WIDGET(check), GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(GTK_WIDGET(check), name.c_str());
        g_object_set_data(G_OBJECT(check), "floating-layer-id", GUINT_TO_POINTER(layerId));
        g_object_set_data(G_OBJECT(check), "floating-layer-visibility", GINT_TO_POINTER(0));
        g_object_set_data(G_OBJECT(check), "floating-layer-page", GINT_TO_POINTER(page + 1));
        g_object_set_data(G_OBJECT(check), "floating-layer-popover", menu.popover);
        g_signal_connect(check, "toggled", G_CALLBACK(+[](GtkCheckButton* button, gpointer data) {
                             if (!gtk_check_button_get_active(button)) {
                                 return;
                             }
                             auto* self = static_cast<Prototype*>(data);
                             if (self->syncingFloatingLayers) {
                                 return;
                             }
                             const int page =
                                     GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-layer-page")) - 1;
                             const auto layerId = static_cast<size_t>(
                                     GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "floating-layer-id")));
                             auto* popover = GTK_POPOVER(g_object_get_data(G_OBJECT(button), "floating-layer-popover"));
                             if (page != self->state.page) {
                                 gtk_popover_popdown(popover);
                                 return;
                             }
                             self->finishTextEditing();
                             self->resetInput(true);
                             if (self->state.selectLayer(page, layerId)) {
                                 self->selectedImageIndex.reset();
                                 self->rebuildLayerStack();
                                 self->update();
                             }
                             gtk_popover_popdown(popover);
                         }),
                         this);
        select = GTK_WIDGET(check);
    } else {
        auto* label = gtk_label_new(name.c_str());
        gtk_widget_set_hexpand(label, true);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        select = label;
    }

    auto* visibility = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
    gtk_button_set_icon_name(GTK_BUTTON(visibility), visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
    gtk_toggle_button_set_active(visibility, visible);
    gtk_widget_add_css_class(GTK_WIDGET(visibility), "flat");
    const auto visibilityLabel = std::string(visible ? "Hide " : "Show ") + name;
    gtk_widget_set_tooltip_text(GTK_WIDGET(visibility), visibilityLabel.c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(visibility), GTK_ACCESSIBLE_PROPERTY_LABEL, visibilityLabel.c_str(),
                                   -1);
    g_object_set_data(G_OBJECT(visibility), "floating-layer-id", GUINT_TO_POINTER(layerId));
    g_object_set_data(G_OBJECT(visibility), "floating-layer-visibility", GINT_TO_POINTER(1));
    g_object_set_data(G_OBJECT(visibility), "floating-layer-page", GINT_TO_POINTER(page + 1));
    g_signal_connect(
            visibility, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                if (self->syncingFloatingLayers) {
                    return;
                }
                const int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-layer-page")) - 1;
                const auto layerId =
                        static_cast<size_t>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "floating-layer-id")));
                if (page != self->state.page) {
                    return;
                }
                const bool visible = gtk_toggle_button_get_active(button);
                self->finishTextEditing();
                self->resetInput(true);
                const auto label = std::string(visible ? "Hide " : "Show ") +
                                   layerDisplayName(self->state.layersForPage(page), layerId);
                gtk_button_set_icon_name(GTK_BUTTON(button),
                                         visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
                gtk_widget_set_tooltip_text(GTK_WIDGET(button), label.c_str());
                gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(),
                                               -1);
                if (self->state.setLayerVisible(page, layerId, visible)) {
                    if (self->selectedImageIndex && *self->selectedImageIndex < self->state.images.size() &&
                        self->state.images[*self->selectedImageIndex].page == page &&
                        self->state.images[*self->selectedImageIndex].layerId == layerId) {
                        self->selectedImageIndex.reset();
                    }
                    self->thumbnailRenderer.invalidate();
                    self->update();
                }
            }),
            this);

    gtk_box_append(GTK_BOX(row), select);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(visibility));
    gtk_box_append(menu.layerRows, row);
}

bool Prototype::sameFloatingLayerStructure(const prototype::PageLayers& first, const prototype::PageLayers& second) {
    if (first.backgroundName != second.backgroundName || first.initialLayerCount != second.initialLayerCount ||
        first.layers.size() != second.layers.size()) {
        return false;
    }
    for (size_t index = 0; index < first.layers.size(); ++index) {
        const auto& left = first.layers[index];
        const auto& right = second.layers[index];
        if (left.name != right.name || left.origins != right.origins ||
            left.hasNonEditableElements != right.hasNonEditableElements) {
            return false;
        }
    }
    return true;
}

void Prototype::updateFloatingLayerMenuSensitivity(FloatingLayerMenuWidgets& menu,
                                                   const prototype::PageLayers& pageLayers) {
    gtk_widget_set_sensitive(GTK_WIDGET(menu.showAllButton), bool(nativeSession) && !pageLayers.layers.empty());
    gtk_widget_set_sensitive(GTK_WIDGET(menu.hideAllButton), bool(nativeSession) && !pageLayers.layers.empty());
    gtk_widget_set_sensitive(GTK_WIDGET(menu.addAboveButton), bool(nativeSession) && pageLayers.layers.size() < 1000);
    gtk_widget_set_sensitive(GTK_WIDGET(menu.addBelowButton), bool(nativeSession) && pageLayers.layers.size() < 1000);
}

void Prototype::updateFloatingLayerMenuRows(FloatingLayerMenuWidgets& menu, int page,
                                            const prototype::PageLayers& pageLayers) {
    const bool wasSyncing = syncingFloatingLayers;
    syncingFloatingLayers = true;
    for (size_t layerId = 1; layerId <= pageLayers.layers.size(); ++layerId) {
        if (auto* control = findFloatingLayerControl(menu, layerId, false)) {
            gtk_check_button_set_active(GTK_CHECK_BUTTON(control), pageLayers.selectedLayer == layerId);
        }
        if (auto* control = findFloatingLayerControl(menu, layerId, true)) {
            const bool visible = pageLayers.layers[layerId - 1].visible;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control), visible);
            gtk_button_set_icon_name(GTK_BUTTON(control), visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
            const auto label = std::string(visible ? "Hide " : "Show ") + layerDisplayName(pageLayers, layerId);
            gtk_widget_set_tooltip_text(control, label.c_str());
            gtk_accessible_update_property(GTK_ACCESSIBLE(control), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
        }
    }
    if (auto* control = findFloatingLayerControl(menu, 0, true)) {
        const bool visible = pageLayers.backgroundVisible;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control), visible);
        gtk_button_set_icon_name(GTK_BUTTON(control), visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
        const auto label = std::string(visible ? "Hide " : "Show ") + layerDisplayName(pageLayers, 0);
        gtk_widget_set_tooltip_text(control, label.c_str());
        gtk_accessible_update_property(GTK_ACCESSIBLE(control), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
    }
    updateFloatingLayerMenuSensitivity(menu, pageLayers);
    menu.page = page;
    menu.snapshot = pageLayers;
    syncingFloatingLayers = wasSyncing;
}

void Prototype::rebuildFloatingLayerRows(FloatingLayerMenuWidgets& menu, int page,
                                         const prototype::PageLayers& pageLayers) {
    while (auto* child = gtk_widget_get_first_child(GTK_WIDGET(menu.layerRows))) {
        gtk_box_remove(menu.layerRows, child);
    }

    GtkCheckButton* activeGroup = nullptr;
    for (size_t layerId = pageLayers.layers.size(); layerId > 0; --layerId) {
        const auto& layer = pageLayers.layers[layerId - 1];
        appendFloatingLayerControl(menu, page, layerId, layerDisplayName(pageLayers, layerId), layer.visible, true,
                                   pageLayers.selectedLayer == layerId, activeGroup);
    }
    appendFloatingLayerControl(menu, page, 0, layerDisplayName(pageLayers, 0), pageLayers.backgroundVisible, false,
                               false, activeGroup);

    updateFloatingLayerMenuRows(menu, page, pageLayers);
}

void Prototype::rebuildFloatingLayerMenus() {
    for (auto& menu: floatingLayerMenus) {
        const auto& pageLayers = state.layersForPage(state.page);
        gtk_label_set_text(menu.activeLayerLabel, layerDisplayName(pageLayers, pageLayers.selectedLayer).c_str());
        rebuildFloatingLayerRows(menu, state.page, pageLayers);
    }
}

void Prototype::scheduleFloatingLayerMenuRebuild() {
    if (floatingLayerRefreshSource) {
        return;
    }
    floatingLayerRefreshSource = g_idle_add(
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                self->floatingLayerRefreshSource = 0;
                if (!self->closed) {
                    self->rebuildFloatingLayerMenus();
                }
                return G_SOURCE_REMOVE;
            },
            this);
}

void Prototype::updateFloatingLayerMenus() {
    for (auto& menu: floatingLayerMenus) {
        const auto& pageLayers = state.layersForPage(state.page);
        gtk_label_set_text(menu.activeLayerLabel, layerDisplayName(pageLayers, pageLayers.selectedLayer).c_str());
        updateFloatingLayerMenuSensitivity(menu, pageLayers);
        if (menu.page != state.page || !menu.snapshot || !sameFloatingLayerStructure(*menu.snapshot, pageLayers)) {
            scheduleFloatingLayerMenuRebuild();
        } else if (*menu.snapshot != pageLayers) {
            updateFloatingLayerMenuRows(menu, state.page, pageLayers);
        }
    }
}

GtkWidget* Prototype::findFloatingLayerControl(const FloatingLayerMenuWidgets& menu, size_t layerId,
                                               bool visibility) const {
    for (auto* row = gtk_widget_get_first_child(GTK_WIDGET(menu.layerRows)); row;
         row = gtk_widget_get_next_sibling(row)) {
        for (auto* child = gtk_widget_get_first_child(row); child; child = gtk_widget_get_next_sibling(child)) {
            if (static_cast<size_t>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "floating-layer-id"))) ==
                        layerId &&
                static_cast<bool>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "floating-layer-visibility"))) ==
                        visibility) {
                return child;
            }
        }
    }
    return nullptr;
}

void Prototype::addFloatingLayerMenu(GtkWidget* row) {
    FloatingLayerMenuWidgets menu;
    menu.button = GTK_MENU_BUTTON(gtk_menu_button_new());
    menu.activeLayerLabel = GTK_LABEL(gtk_label_new("Layer 1"));
    gtk_menu_button_set_child(menu.button, GTK_WIDGET(menu.activeLayerLabel));
    gtk_menu_button_set_always_show_arrow(menu.button, true);
    gtk_widget_add_css_class(GTK_WIDGET(menu.button), "flat");
    gtk_widget_set_tooltip_text(GTK_WIDGET(menu.button), "Select the active layer and change visibility");

    menu.popover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_add_css_class(GTK_WIDGET(menu.popover), "toolbar");
    auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(content, 6);
    gtk_widget_set_margin_bottom(content, 6);
    gtk_widget_set_margin_start(content, 6);
    gtk_widget_set_margin_end(content, 6);

    auto* visibilityActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(visibilityActions, "linked");
    const auto addMenuAction = [this](GtkBox* parent, const char* label, const char* actionName) {
        auto* action = GTK_BUTTON(gtk_button_new_with_label(label));
        gtk_widget_add_css_class(GTK_WIDGET(action), "flat");
        g_object_set_data_full(G_OBJECT(action), "floating-layer-action", g_strdup(actionName), g_free);
        g_signal_connect(action, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->finishTextEditing();
                             self->resetInput(true);
                             const auto* name = static_cast<const char*>(
                                     g_object_get_data(G_OBJECT(button), "floating-layer-action"));
                             const std::string_view action(name ? name : "");
                             bool changed = false;
                             if (action == "show-all") {
                                 changed = self->state.setAllLayersVisible(self->state.page, true);
                             } else if (action == "hide-all") {
                                 changed = self->state.setAllLayersVisible(self->state.page, false);
                             } else if (action == "add-above") {
                                 changed = self->state.addLayerAboveCurrent();
                             } else if (action == "add-below") {
                                 changed = self->state.addLayerBelowCurrent();
                             }
                             if (changed) {
                                 self->refreshAfterLayerMutation();
                             }
                         }),
                         this);
        gtk_box_append(parent, GTK_WIDGET(action));
        return action;
    };
    menu.showAllButton = addMenuAction(GTK_BOX(visibilityActions), "Show all", "show-all");
    menu.hideAllButton = addMenuAction(GTK_BOX(visibilityActions), "Hide all", "hide-all");
    gtk_box_append(GTK_BOX(content), visibilityActions);
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    auto* addActions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    menu.addAboveButton = addMenuAction(GTK_BOX(addActions), "Add a layer above the active layer", "add-above");
    menu.addBelowButton = addMenuAction(GTK_BOX(addActions), "Add a layer below the active layer", "add-below");
    gtk_box_append(GTK_BOX(content), addActions);
    gtk_box_append(GTK_BOX(content), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    menu.layerRows = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(menu.layerRows));
    gtk_popover_set_child(menu.popover, content);
    gtk_menu_button_set_popover(menu.button, GTK_WIDGET(menu.popover));
    floatingLayerMenus.push_back(menu);
    auto* label = gtk_label_new("Layer");
    gtk_widget_set_margin_start(label, 7);
    gtk_widget_set_margin_end(label, 7);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(menu.button));
    rebuildFloatingLayerRows(floatingLayerMenus.back(), state.page, state.layersForPage(state.page));
}

void Prototype::addFloatingToolboxAction(GtkWidget* row, const std::string& token) {
    auto addTool = [&](Tool tool, const char* label) { addFloatingToolboxToolAction(row, tool, label); };
    auto addButton = [&](const char* label, GCallback callback) {
        auto* action = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(action, "flat");
        g_signal_connect(action, "clicked", callback, this);
        gtk_box_append(GTK_BOX(row), action);
    };

    if (token == "PEN") {
        addTool(Tool::Pen, "Pen");
    } else if (token == "HIGHLIGHTER") {
        addTool(Tool::Highlighter, "Highlight");
    } else if (token == "ERASER") {
        addTool(Tool::Eraser, "Eraser");
    } else if (token == "HAND") {
        addTool(Tool::Hand, "Hand");
    } else if (token == "SELECT") {
        addTool(Tool::SelectRectangle, "Select");
    } else if (token == "SELECT_RECT") {
        addTool(Tool::SelectRectangle, "Rectangle select");
    } else if (token == "SELECT_LASSO" || token == "SELECT_REGION") {
        addTool(Tool::SelectLasso, "Lasso");
    } else if (token == "SMART_SELECT") {
        addTool(Tool::SmartSelect, "Smart Select");
    } else if (token == "SELECT_OBJECT") {
        addTool(Tool::SelectObject, "Select object");
    } else if (token == "TEXT") {
        addTool(Tool::Text, "Text");
    } else if (token == "LATEX") {
        addTool(Tool::Latex, "LaTeX");
    } else if (token == "LINK") {
        addTool(Tool::Link, "Link");
    } else if (token == "PLAY_OBJECT") {
        addTool(Tool::PlayObject, "Play audio");
    } else if (token == "LASER_POINTER") {
        addTool(Tool::LaserPointerPen, "Laser pen");
        addTool(Tool::LaserPointerHighlighter, "Laser marker");
    } else if (token == "PDF_TOOL") {
        addTool(Tool::SelectPdfTextLinear, "PDF text");
        addTool(Tool::SelectPdfTextRect, "PDF area");
    } else if (token == "UNDO") {
        addButton("Undo", G_CALLBACK(+[](GtkButton*, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      g_signal_emit_by_name(self->undoButton, "clicked");
                  }));
    } else if (token == "REDO") {
        addButton("Redo", G_CALLBACK(+[](GtkButton*, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      g_signal_emit_by_name(self->redoButton, "clicked");
                  }));
    } else if (token == "PAGE_SPIN") {
        auto* pageControl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        auto* pageLabel = gtk_label_new("Page");
        auto* spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, std::max(1, pageCount), 1));
        gtk_spin_button_set_numeric(spin, true);
        gtk_spin_button_set_digits(spin, 0);
        gtk_editable_set_width_chars(GTK_EDITABLE(spin), 4);
        gtk_widget_set_tooltip_text(GTK_WIDGET(spin), "Go to page");
        gtk_accessible_update_property(GTK_ACCESSIBLE(spin), GTK_ACCESSIBLE_PROPERTY_LABEL, "Current page number", -1);
        auto* status = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(status), "dim-label");
        floatingPageSpinners.emplace_back(spin, status);
        g_signal_connect(spin, "value-changed", G_CALLBACK(+[](GtkSpinButton* changed, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->syncingPage) {
                                 return;
                             }
                             if (self->floatingPageSpinTimeout) {
                                 g_source_remove(self->floatingPageSpinTimeout);
                             }
                             self->pendingFloatingPageSpin = changed;
                             self->floatingPageSpinTimeout = g_timeout_add(
                                     100,
                                     +[](gpointer timeoutData) -> gboolean {
                                         auto* self = static_cast<Prototype*>(timeoutData);
                                         self->floatingPageSpinTimeout = 0;
                                         auto* pending = std::exchange(self->pendingFloatingPageSpin, nullptr);
                                         if (pending && !self->closed) {
                                             self->navigateToPage(gtk_spin_button_get_value_as_int(pending) - 1);
                                         }
                                         return G_SOURCE_REMOVE;
                                     },
                                     self);
                         }),
                         this);
        gtk_box_append(GTK_BOX(pageControl), pageLabel);
        gtk_box_append(GTK_BOX(pageControl), GTK_WIDGET(spin));
        gtk_box_append(GTK_BOX(pageControl), GTK_WIDGET(status));
        gtk_box_append(GTK_BOX(row), pageControl);
    } else if (token == "LAYER") {
        addFloatingLayerMenu(row);
    } else if (token == "ZOOM_SLIDER") {
        auto* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 25.0, 400.0, 5.0);
        gtk_widget_set_size_request(scale, 132, -1);
        gtk_scale_set_draw_value(GTK_SCALE(scale), true);
        gtk_scale_set_digits(GTK_SCALE(scale), 0);
        gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
        gtk_scale_set_format_value_func(
                GTK_SCALE(scale), +[](GtkScale*, double value, gpointer) { return g_strdup_printf("%.0f%%", value); },
                nullptr, nullptr);
        gtk_widget_set_tooltip_text(scale, "Zoom percentage");
        gtk_range_set_value(GTK_RANGE(scale), state.zoom * 100.0);
        g_signal_connect(scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const double x = gtk_widget_get_width(self->canvas) / 2.0;
                             const double y = gtk_widget_get_height(self->canvas) / 2.0;
                             self->zoom(gtk_range_get_value(range) / 100.0, x, y, true);
                         }),
                         this);
        gtk_box_append(GTK_BOX(row), scale);
    } else if (token == "TOOL_FILL") {
        auto* toggle = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Fill"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(toggle), "Fill new Pen/Highlighter strokes or the selected strokes");
        gtk_accessible_update_property(GTK_ACCESSIBLE(toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, "Fill", -1);
        gtk_toggle_button_set_active(toggle, fillToggleActive());
        gtk_widget_set_sensitive(GTK_WIDGET(toggle), fillToggleAvailable());
        g_object_set_data_full(G_OBJECT(toggle), "floating-toolbox-token", g_strdup(token.c_str()), g_free);
        g_signal_connect(toggle, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->syncingBrush) {
                                 return;
                             }
                             std::string error;
                             if (!self->setFillEnabled(gtk_toggle_button_get_active(button), error)) {
                                 self->showAlert("Could not change Fill", error);
                             }
                         }),
                         this);
        floatingFillToggles.push_back(toggle);
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(toggle));
    } else if (token == "FILL_OPACITY") {
        auto* opacityButton = gtk_button_new_with_label("Opacity");
        gtk_widget_add_css_class(opacityButton, "flat");
        gtk_widget_set_tooltip_text(opacityButton, "Set fill opacity for Pen or Highlighter");
        gtk_accessible_update_property(GTK_ACCESSIBLE(opacityButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Fill opacity",
                                       -1);
        gtk_widget_set_sensitive(opacityButton, fillOpacityAvailable());
        g_object_set_data_full(G_OBJECT(opacityButton), "floating-toolbox-token", g_strdup(token.c_str()), g_free);
        g_signal_connect(opacityButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             std::string error;
                             if (!self->openFillOpacityDialog(error)) {
                                 self->showAlert("Could not change fill opacity", error);
                             }
                         }),
                         this);
        floatingFillOpacityButtons.push_back(GTK_BUTTON(opacityButton));
        gtk_box_append(GTK_BOX(row), opacityButton);
    } else if (token == "DRAW") {
        auto* modeNames = gtk_string_list_new(nullptr);
        for (const auto& choice: prototype::floatingDrawingModeChoices) {
            const std::string label(choice.label);
            gtk_string_list_append(modeNames, label.c_str());
        }
        auto* selector = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(modeNames), nullptr));
        gtk_widget_set_tooltip_text(GTK_WIDGET(selector), "Drawing type");
        gtk_accessible_update_property(GTK_ACCESSIBLE(selector), GTK_ACCESSIBLE_PROPERTY_LABEL, "Drawing type", -1);
        gtk_drop_down_set_selected(selector,
                                   static_cast<guint>(prototype::floatingDrawingModeComboIndex(state.drawingMode)));
        g_object_set_data_full(G_OBJECT(selector), "floating-drawing-mode-token", g_strdup(token.c_str()), g_free);
        g_signal_connect(selector, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->syncingBrush) {
                                 return;
                             }
                             const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
                             const auto mode = prototype::floatingDrawingModeComboMode(selected);
                             if (!mode) {
                                 return;
                             }
                             self->input.cancel();
                             self->state.drawingMode = *mode;
                             const bool supportsStabilizer = *mode == prototype::DrawingMode::Freehand ||
                                                             *mode == prototype::DrawingMode::ShapeRecognizer;
                             gtk_widget_set_visible(self->averagingDropdown, supportsStabilizer);
                             gtk_widget_set_visible(self->preprocessorDropdown, supportsStabilizer);
                             self->update();
                         }),
                         this);
        floatingDrawingModeSelectors.push_back(selector);
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(selector));
    } else if (const auto drawingModeAction = prototype::floatingDrawingModeAction(token)) {
        const auto mode = drawingModeAction->mode;
        const std::string label(drawingModeAction->label);
        addButton(label.c_str(), G_CALLBACK(+[](GtkButton* button, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      const auto selectedMode = static_cast<prototype::DrawingMode>(
                              GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-drawing-mode")));
                      if (self->drawingModeDropdown) {
                          gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drawingModeDropdown),
                                                     static_cast<guint>(selectedMode));
                      } else {
                          self->state.drawingMode = selectedMode;
                          self->update();
                      }
                  }));
        auto* modeButton = gtk_widget_get_last_child(row);
        gtk_widget_set_tooltip_text(modeButton, label.c_str());
        g_object_set_data(G_OBJECT(modeButton), "floating-drawing-mode", GINT_TO_POINTER(static_cast<int>(mode)));
        g_object_set_data_full(G_OBJECT(modeButton), "floating-drawing-mode-token", g_strdup(token.c_str()), g_free);
    } else if (token == "FORMAT_ALIGN_LEFT" || token == "FORMAT_ALIGN_CENTER" || token == "FORMAT_ALIGN_RIGHT") {
        const int alignment = token == "FORMAT_ALIGN_CENTER" ? 1 : token == "FORMAT_ALIGN_RIGHT" ? 2 : 0;
        const char* label = alignment == 1 ? "Align center" : alignment == 2 ? "Align right" : "Align left";
        addButton(label, G_CALLBACK(+[](GtkButton* button, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      self->applyTextAlignment(
                              GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-text-alignment")));
                  }));
        auto* alignButton = gtk_widget_get_last_child(row);
        gtk_widget_set_tooltip_text(alignButton, label);
        g_object_set_data(G_OBJECT(alignButton), "floating-text-alignment", GINT_TO_POINTER(alignment));
        g_object_set_data_full(G_OBJECT(alignButton), "gtk4-floating-text-layout", g_strdup(token.c_str()), g_free);
    } else if (token == "FORMAT_JUSTIFY") {
        addButton("Justify", G_CALLBACK(+[](GtkButton*, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      self->applyTextJustify(!self->state.textJustify);
                  }));
        auto* justifyButton = gtk_widget_get_last_child(row);
        gtk_widget_set_tooltip_text(justifyButton, "Toggle justified text");
        g_object_set_data_full(G_OBJECT(justifyButton), "gtk4-floating-text-layout", g_strdup(token.c_str()), g_free);
    } else if (token.starts_with("FORMAT_")) {
        using Format = prototype::TextEditorOverlay::Format;
        std::optional<Format> format;
        const char* label = nullptr;
        const char* tooltip = nullptr;
        if (token == "FORMAT_BOLD") {
            format = Format::Bold;
            label = "Bold";
            tooltip = "Bold text (Ctrl+B)";
        } else if (token == "FORMAT_ITALIC") {
            format = Format::Italic;
            label = "Italic";
            tooltip = "Italic text (Ctrl+I)";
        } else if (token == "FORMAT_UNDERLINE") {
            format = Format::Underline;
            label = "Underline";
            tooltip = "Underline text (Ctrl+U)";
        } else if (token == "FORMAT_STRIKETHROUGH") {
            format = Format::Strikethrough;
            label = "Strikethrough";
            tooltip = "Strikethrough text (Ctrl+Shift+K)";
        } else if (token == "FORMAT_SUPERSCRIPT") {
            format = Format::Superscript;
            label = "Superscript";
            tooltip = "Superscript text (Ctrl+.)";
        } else if (token == "FORMAT_SUBSCRIPT") {
            format = Format::Subscript;
            label = "Subscript";
            tooltip = "Subscript text (Ctrl+,)";
        }

        if (!format) {
            auto* unsupported = gtk_button_new_with_label(token.c_str());
            gtk_widget_set_sensitive(unsupported, false);
            gtk_widget_set_tooltip_text(unsupported, "This text formatting action is not ported yet");
            gtk_box_append(GTK_BOX(row), unsupported);
        } else {
            addButton(label, G_CALLBACK(+[](GtkButton* button, gpointer data) {
                          auto* self = static_cast<Prototype*>(data);
                          const auto selectedFormat = static_cast<Format>(
                                  GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-text-format")));
                          self->applyTextFormat(selectedFormat);
                      }));
            auto* formatButton = gtk_widget_get_last_child(row);
            gtk_widget_set_tooltip_text(formatButton, tooltip);
            g_object_set_data(G_OBJECT(formatButton), "floating-text-format",
                              GINT_TO_POINTER(static_cast<int>(*format)));
            g_object_set_data_full(G_OBJECT(formatButton), "gtk4-floating-text-format", g_strdup(token.c_str()),
                                   g_free);
        }
    } else if (token == "FINE" || token == "MEDIUM" || token == "THICK" || token == "VERY_FINE" ||
               token == "VERY_THICK") {
        const guint preset = token == "VERY_FINE" ? 0u :
                             token == "FINE"      ? 1u :
                             token == "MEDIUM"    ? 2u :
                             token == "THICK"     ? 3u :
                                                    4u;
        const char* label = token == "VERY_FINE" ? "Very fine" :
                            token == "FINE"      ? "Fine" :
                            token == "MEDIUM"    ? "Medium" :
                            token == "THICK"     ? "Thick" :
                                                   "Very thick";
        addButton(label, G_CALLBACK(+[](GtkButton* button, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      const auto selectedPreset =
                              GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "floating-toolbox-preset"));
                      if (self->sizeDropdown) {
                          gtk_drop_down_set_selected(GTK_DROP_DOWN(self->sizeDropdown), selectedPreset);
                      }
                  }));
        g_object_set_data(G_OBJECT(gtk_widget_get_last_child(row)), "floating-toolbox-preset",
                          GUINT_TO_POINTER(preset));
    } else if (token.starts_with("COLOR(") && token.back() == ')') {
        unsigned int swatchIndex = 0;
        bool validIndex = true;
        for (std::size_t index = 6; index + 1 < token.size(); ++index) {
            const unsigned int digit = static_cast<unsigned int>(token[index] - '0');
            if (swatchIndex > 100 || digit > 9) {
                validIndex = false;
                break;
            }
            swatchIndex = swatchIndex * 10 + digit;
        }
        if (!validIndex || swatchIndex >= importedConfig.palette.swatches.size()) {
            auto* unsupported = gtk_button_new_with_label(token.c_str());
            gtk_widget_set_sensitive(unsupported, false);
            gtk_widget_set_tooltip_text(unsupported, "This color index is not present in the imported palette");
            gtk_box_append(GTK_BOX(row), unsupported);
        } else {
            auto* swatch = gtk_button_new();
            gtk_widget_add_css_class(swatch, "flat");
            auto* dot = gtk_drawing_area_new();
            gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(dot), 20);
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(dot), 20);
            const auto color = importedConfig.palette.swatches[swatchIndex].color;
            gtk_drawing_area_set_draw_func(
                    GTK_DRAWING_AREA(dot),
                    +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
                        const auto* rgba = static_cast<const std::array<double, 3>*>(data);
                        cairo_arc(cr, width / 2.0, height / 2.0, std::max(2.0, std::min(width, height) / 2.0 - 2.0), 0,
                                  2 * G_PI);
                        cairo_set_source_rgb(cr, (*rgba)[0], (*rgba)[1], (*rgba)[2]);
                        cairo_fill_preserve(cr);
                        cairo_set_source_rgba(cr, 0.45, 0.45, 0.45, 0.65);
                        cairo_set_line_width(cr, 1.0);
                        cairo_stroke(cr);
                    },
                    new std::array<double, 3>(color),
                    +[](gpointer data) { delete static_cast<std::array<double, 3>*>(data); });
            gtk_button_set_child(GTK_BUTTON(swatch), dot);
            gtk_widget_set_tooltip_text(swatch, importedConfig.palette.swatches[swatchIndex].name.c_str());
            gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           importedConfig.palette.swatches[swatchIndex].name.c_str(), -1);
            g_object_set_data(G_OBJECT(swatch), "floating-toolbox-color-index", GUINT_TO_POINTER(swatchIndex));
            g_signal_connect(swatch, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const auto index = GPOINTER_TO_UINT(
                                         g_object_get_data(G_OBJECT(button), "floating-toolbox-color-index"));
                                 self->selectPaletteColor(index);
                             }),
                             this);
            gtk_box_append(GTK_BOX(row), swatch);
        }
    } else if (token == "COLOR_SELECT") {
        addButton("Color…", G_CALLBACK(+[](GtkButton*, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      gtk_widget_activate(self->colorButton);
                  }));
    } else if (token == "ZOOM_IN" || token == "ZOOM_OUT") {
        const bool zoomIn = token == "ZOOM_IN";
        addButton(zoomIn ? "Zoom in" : "Zoom out", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                      auto* self = static_cast<Prototype*>(data);
                      const bool zoomIn = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "zoom-in")) != 0;
                      self->zoom(self->state.steppedZoom(zoomIn, false), gtk_widget_get_width(self->canvas) / 2.0,
                                 gtk_widget_get_height(self->canvas) / 2.0, true);
                  }));
        auto* zoomButton = gtk_widget_get_last_child(row);
        g_object_set_data(G_OBJECT(zoomButton), "zoom-in", GINT_TO_POINTER(zoomIn));
        floatingZoomButtons.push_back(zoomButton);
    } else if (token.starts_with("Plugin::")) {
        const auto item = pluginRuntime ? pluginRuntime->findToolbarItem(token) : std::nullopt;
        const auto fallbackLabel = token.substr(std::string_view("Plugin::").size());
        if (!item) {
            const auto placeholder = pluginRuntime ? pluginRuntime->findToolbarPlaceholder(token) : std::nullopt;
            if (placeholder) {
                std::string displayValue = placeholder->value.empty() ? fallbackLabel : placeholder->value;
                std::replace(displayValue.begin(), displayValue.end(), '\n', ' ');
                std::replace(displayValue.begin(), displayValue.end(), '\r', ' ');
                auto* label = gtk_label_new(displayValue.c_str());
                gtk_widget_set_size_request(label, 200, -1);
                gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
                gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
                const std::string tooltip =
                        placeholder->description.empty() ? placeholder->id : placeholder->description;
                gtk_widget_set_tooltip_text(label, tooltip.c_str());
                gtk_accessible_update_property(GTK_ACCESSIBLE(label), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                               displayValue.c_str(), -1);
                g_object_set_data_full(G_OBJECT(label), "gtk4-plugin-placeholder-id", g_strdup(placeholder->id.c_str()),
                                       g_free);
                gtk_box_append(GTK_BOX(row), label);
                return;
            }
            auto* unavailable = gtk_button_new_with_label(fallbackLabel.c_str());
            gtk_widget_add_css_class(unavailable, "flat");
            gtk_widget_set_sensitive(unavailable, false);
            gtk_widget_set_tooltip_text(unavailable, "Enable the plugin that provides this toolbar action");
            gtk_box_append(GTK_BOX(row), unavailable);
            return;
        }

        const std::string label = item->description.empty() ? fallbackLabel : item->description;
        const std::string tooltip = item->description.empty() ? item->id : item->description;
        auto* action = gtk_button_new();
        gtk_widget_add_css_class(action, "flat");
        gtk_widget_set_tooltip_text(action, tooltip.c_str());
        gtk_accessible_update_property(GTK_ACCESSIBLE(action), GTK_ACCESSIBLE_PROPERTY_LABEL, label.c_str(), -1);
        auto* display = gdk_display_get_default();
        auto* iconTheme = display ? gtk_icon_theme_get_for_display(display) : nullptr;
        if (!item->iconName.empty() && iconTheme && gtk_icon_theme_has_icon(iconTheme, item->iconName.c_str())) {
            gtk_button_set_child(GTK_BUTTON(action), gtk_image_new_from_icon_name(item->iconName.c_str()));
        } else {
            gtk_button_set_label(GTK_BUTTON(action), label.c_str());
        }
        g_object_set_data_full(G_OBJECT(action), "gtk4-plugin-toolbar-id", g_strdup(item->id.c_str()), g_free);
        using PluginToolbarActionContext = std::pair<Prototype*, std::string>;
        auto* context = new PluginToolbarActionContext(this, item->id);
        g_signal_connect_data(
                action, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    const auto* context = static_cast<PluginToolbarActionContext*>(data);
                    if (context->first->pluginRuntime) {
                        context->first->pluginRuntime->invokeToolbarItem(context->second);
                    }
                }),
                context, +[](gpointer data, GClosure*) { delete static_cast<PluginToolbarActionContext*>(data); },
                GConnectFlags(0));
        gtk_box_append(GTK_BOX(row), action);
    } else if (token == "SEPARATOR") {
        gtk_box_append(GTK_BOX(row), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    } else {
        auto* unsupported = gtk_button_new_with_label(token.c_str());
        gtk_widget_set_sensitive(unsupported, false);
        gtk_widget_set_tooltip_text(unsupported, "This toolbar action is not ported yet");
        gtk_box_append(GTK_BOX(row), unsupported);
    }
}

void Prototype::rebuildFloatingToolboxRows() {
    if (!floatingToolboxRowsContainer) {
        return;
    }
    if (floatingLayerRefreshSource) {
        g_source_remove(floatingLayerRefreshSource);
        floatingLayerRefreshSource = 0;
    }
    if (floatingPageSpinTimeout) {
        g_source_remove(floatingPageSpinTimeout);
        floatingPageSpinTimeout = 0;
    }
    pendingFloatingPageSpin = nullptr;
    floatingDrawingModeSelectors.clear();
    floatingFillToggles.clear();
    floatingFillOpacityButtons.clear();
    floatingPageSpinners.clear();
    floatingLayerMenus.clear();
    floatingZoomButtons.clear();
    if (floatingToolboxToolButtons.size() > floatingToolboxStaticToolButtonCount) {
        floatingToolboxToolButtons.erase(
                floatingToolboxToolButtons.begin() + static_cast<std::ptrdiff_t>(floatingToolboxStaticToolButtonCount),
                floatingToolboxToolButtons.end());
    }
    while (auto* child = gtk_widget_get_first_child(floatingToolboxRowsContainer)) {
        gtk_box_remove(GTK_BOX(floatingToolboxRowsContainer), child);
    }

    for (const auto& items: importedConfig.settings.floatingToolboxRows) {
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        for (const auto& token: items) {
            addFloatingToolboxAction(row, token);
        }
        if (gtk_widget_get_first_child(row)) {
            gtk_box_append(GTK_BOX(floatingToolboxRowsContainer), row);
        }
    }
    if (floatingToolbox && gtk_widget_get_visible(floatingToolbox)) {
        positionFloatingToolbox(floatingToolboxAnchor.x, floatingToolboxAnchor.y);
    }
}

void Prototype::selectPaletteColor(size_t index) {
    if (index >= importedConfig.palette.swatches.size() || !colorButton) {
        return;
    }
    const auto& color = importedConfig.palette.swatches[index].color;
    const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]), 1.0f};
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
    if (palettePopover) {
        gtk_popover_popdown(GTK_POPOVER(palettePopover));
    }
    gtk_widget_grab_focus(textEditor && textEditor->active() ? textEditor->widget() : canvas);
}

void Prototype::createFloatingToolbox() {
    floatingToolbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(floatingToolbox, "card");
    gtk_widget_add_css_class(floatingToolbox, "floating-toolbox");
    gtk_widget_set_halign(floatingToolbox, GTK_ALIGN_START);
    gtk_widget_set_valign(floatingToolbox, GTK_ALIGN_START);
    gtk_widget_set_visible(floatingToolbox, false);
    gtk_widget_set_margin_start(floatingToolbox, 10);
    gtk_widget_set_margin_top(floatingToolbox, 10);

    auto* contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(contents, 10);
    gtk_widget_set_margin_bottom(contents, 10);
    gtk_widget_set_margin_start(contents, 10);
    gtk_widget_set_margin_end(contents, 10);
    gtk_box_append(GTK_BOX(floatingToolbox), contents);

    auto* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* title = gtk_label_new("Floating toolbox");
    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(title, true);
    gtk_box_append(GTK_BOX(heading), title);
    auto* close = button("window-close-symbolic", "Hide floating toolbox", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->hideFloatingToolbox();
                         }));
    gtk_box_append(GTK_BOX(contents), heading);

    auto* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    const std::array<std::pair<Tool, const char*>, 14> tools = {{
            {Tool::Pen, "Pen"},
            {Tool::Highlighter, "Highlight"},
            {Tool::Eraser, "Eraser"},
            {Tool::Hand, "Hand"},
            {Tool::SelectRectangle, "Rectangle"},
            {Tool::SelectLasso, "Lasso"},
            {Tool::SelectObject, "Select object"},
            {Tool::Text, "Text"},
            {Tool::Link, "Link"},
            {Tool::PlayObject, "Play audio"},
            {Tool::LaserPointerPen, "Laser pen"},
            {Tool::LaserPointerHighlighter, "Laser marker"},
            {Tool::SelectPdfTextLinear, "PDF text"},
            {Tool::SelectPdfTextRect, "PDF area"},
    }};
    floatingToolboxToolButtons.clear();
    for (size_t index = 0; index < tools.size(); ++index) {
        const auto [tool, label] = tools[index];
        auto* toolButton = gtk_toggle_button_new_with_label(label);
        gtk_widget_add_css_class(toolButton, "flat");
        gtk_widget_set_size_request(toolButton, 82, 38);
        gtk_grid_attach(GTK_GRID(grid), toolButton, static_cast<int>(index % 4), static_cast<int>(index / 4), 1, 1);
        floatingToolboxToolButtons.emplace_back(tool, toolButton);
        g_object_set_data(G_OBJECT(toolButton), "floating-toolbox-tool", GINT_TO_POINTER(static_cast<int>(tool)));
        g_signal_connect(toolButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!gtk_toggle_button_get_active(button) || self->syncingTools) {
                                 return;
                             }
                             const auto tool = static_cast<Tool>(
                                     GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-toolbox-tool")));
                             self->selectTool(tool);
                             self->hideFloatingToolbox();
                         }),
                         this);
    }
    addFloatingToolboxToolAction(heading, Tool::SmartSelect, "Smart Select");
    gtk_box_append(GTK_BOX(heading), close);
    auto* moreTools = gtk_expander_new("More tools");
    gtk_expander_set_child(GTK_EXPANDER(moreTools), grid);
    gtk_box_append(GTK_BOX(contents), moreTools);
    floatingToolboxStaticToolButtonCount = floatingToolboxToolButtons.size();

    floatingToolboxRowsContainer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(contents), floatingToolboxRowsContainer);
    rebuildFloatingToolboxRows();

    gtk_overlay_add_overlay(GTK_OVERLAY(canvasOverlay), floatingToolbox);
    auto* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->floatingToolboxHideSource) {
                             g_source_remove(self->floatingToolboxHideSource);
                             self->floatingToolboxHideSource = 0;
                         }
                     }),
                     this);
    g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
                         static_cast<Prototype*>(data)->scheduleFloatingToolboxHide();
                     }),
                     this);
    gtk_widget_add_controller(floatingToolbox, motion);
}

void Prototype::positionFloatingToolbox(double x, double y) {
    if (!floatingToolbox || !canvas) {
        return;
    }
    int naturalWidth = 0;
    gtk_widget_measure(floatingToolbox, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &naturalWidth, nullptr, nullptr);
    int naturalHeight = 0;
    gtk_widget_measure(floatingToolbox, GTK_ORIENTATION_VERTICAL, naturalWidth, nullptr, &naturalHeight, nullptr,
                       nullptr);
    const int canvasWidth = gtk_widget_get_width(canvas);
    const int canvasHeight = gtk_widget_get_height(canvas);
    const int margin = 10;
    const int maxX = std::max(margin, canvasWidth - naturalWidth - margin);
    const int maxY = std::max(margin, canvasHeight - naturalHeight - margin);
    const int left = std::clamp(static_cast<int>(std::lround(x - naturalWidth / 2.0)), margin, maxX);
    const int top = std::clamp(static_cast<int>(std::lround(y - naturalHeight / 2.0)), margin, maxY);
    gtk_widget_set_margin_start(floatingToolbox, left);
    gtk_widget_set_margin_top(floatingToolbox, top);
}

void Prototype::showFloatingToolbox(double x, double y) {
    if (!floatingToolbox) {
        return;
    }
    if (floatingToolboxHideSource) {
        g_source_remove(floatingToolboxHideSource);
        floatingToolboxHideSource = 0;
    }
    floatingToolboxAnchor = {x, y};
    gtk_widget_set_visible(floatingToolbox, true);
    positionFloatingToolbox(x, y);
    gtk_widget_queue_allocate(canvasOverlay);
}

void Prototype::hideFloatingToolbox() {
    if (floatingToolboxHideSource) {
        g_source_remove(floatingToolboxHideSource);
        floatingToolboxHideSource = 0;
    }
    if (floatingToolbox) {
        gtk_widget_set_visible(floatingToolbox, false);
    }
}

void Prototype::scheduleFloatingToolboxHide() {
    if (!floatingToolbox || !gtk_widget_get_visible(floatingToolbox) || floatingToolboxHideSource) {
        return;
    }
    floatingToolboxHideSource = g_timeout_add(
            180,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                self->floatingToolboxHideSource = 0;
                self->hideFloatingToolbox();
                return G_SOURCE_REMOVE;
            },
            this);
}

}  // namespace prototype::gtk4
