#include "Prototype.h"

namespace prototype::gtk4 {

GtkWidget* Prototype::createSidebar() {
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(box, "background");
    auto* title = gtk_label_new("Pages");
    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_margin_top(title, 12);
    gtk_box_append(GTK_BOX(box), title);
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, true);
    auto* pages = gtk_string_list_new(nullptr);
    for (int i = 0; i < pageCount; ++i) {
        const auto label = "Page " + std::to_string(i + 1);
        gtk_string_list_append(pages, label.c_str());
    }
    auto* selection = gtk_single_selection_new(G_LIST_MODEL(pages));
    gtk_single_selection_set_autoselect(selection, false);
    g_signal_connect(selection, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto page = gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(object));
                         if (!self->syncingPage && page != GTK_INVALID_LIST_POSITION &&
                             static_cast<int>(page) != self->state.page) {
                             self->navigateToPage(static_cast<int>(page));
                             if (adw_overlay_split_view_get_collapsed(self->splitView)) {
                                 adw_overlay_split_view_set_show_sidebar(self->splitView, false);
                             }
                             gtk_widget_grab_focus(self->canvas);
                         }
                     }),
                     this);
    auto* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(+[](GtkSignalListItemFactory*, GtkListItem* item, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         auto* contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
                         auto* preview = gtk_drawing_area_new();
                         gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(preview), maxPageThumbnailWidth);
                         gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(preview), maxPageThumbnailHeight);
                         gtk_widget_set_halign(preview, GTK_ALIGN_CENTER);
                         gtk_widget_add_css_class(preview, "page-thumbnail");
                         gtk_drawing_area_set_draw_func(
                                 GTK_DRAWING_AREA(preview),
                                 +[](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer owner) {
                                     auto* self = static_cast<Prototype*>(owner);
                                     self->prioritizeVisibleSidebarThumbnails();
                                     auto* item = GTK_LIST_ITEM(g_object_get_data(G_OBJECT(area), "list-item"));
                                     self->drawThumbnail(cr, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "page")),
                                                         width, height, item && gtk_list_item_get_selected(item));
                                 },
                                 self, nullptr);
                         auto* label = gtk_label_new("");
                         gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
                         gtk_widget_add_css_class(label, "page-thumbnail-number");
                         gtk_box_append(GTK_BOX(contents), preview);
                         gtk_box_append(GTK_BOX(contents), label);
                         gtk_widget_set_margin_top(contents, 8);
                         gtk_widget_set_margin_bottom(contents, 8);
                         g_object_set_data(G_OBJECT(item), "thumbnail", preview);
                         g_object_set_data(G_OBJECT(item), "page-label", label);
                         g_signal_connect(item, "notify::selected",
                                          G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer) {
                                              auto* preview = GTK_WIDGET(g_object_get_data(object, "thumbnail"));
                                              if (preview) {
                                                  gtk_widget_queue_draw(preview);
                                              }
                                          }),
                                          nullptr);
                         gtk_list_item_set_child(item, contents);
                     }),
                     this);
    g_signal_connect(factory, "bind", G_CALLBACK(+[](GtkSignalListItemFactory*, GtkListItem* item, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         auto* preview = GTK_WIDGET(g_object_get_data(G_OBJECT(item), "thumbnail"));
                         auto* label = GTK_LABEL(g_object_get_data(G_OBJECT(item), "page-label"));
                         const auto page = gtk_list_item_get_position(item);
                         const auto pageSize = self->state.pageSize(static_cast<int>(page));
                         const double thumbnailScale = std::min(maxPageThumbnailWidth / pageSize.width,
                                                                maxPageThumbnailHeight / pageSize.height);
                         gtk_drawing_area_set_content_width(
                                 GTK_DRAWING_AREA(preview),
                                 std::max(1, static_cast<int>(std::ceil(pageSize.width * thumbnailScale))));
                         gtk_drawing_area_set_content_height(
                                 GTK_DRAWING_AREA(preview),
                                 std::max(1, static_cast<int>(std::ceil(pageSize.height * thumbnailScale))));
                         g_object_set_data(G_OBJECT(preview), "page", GUINT_TO_POINTER(page));
                         g_object_set_data(G_OBJECT(preview), "list-item", item);
                         const auto pageNumber = std::to_string(page + 1);
                         gtk_label_set_text(label, pageNumber.c_str());
                         self->thumbnailWidgets.push_back(preview);
                         self->thumbnailRenderer.bindThumbnailWidget(static_cast<int>(page), preview);
                         self->prioritizeVisibleSidebarThumbnails();
                         self->requestSidebarThumbnail(static_cast<int>(page));
                         gtk_widget_queue_draw(preview);
                     }),
                     this);
    g_signal_connect(factory, "unbind", G_CALLBACK(+[](GtkSignalListItemFactory*, GtkListItem* item, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         auto* preview = GTK_WIDGET(g_object_get_data(G_OBJECT(item), "thumbnail"));
                         const int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(preview), "page"));
                         self->thumbnailRenderer.unbindThumbnailWidget(page, preview);
                         g_object_set_data(G_OBJECT(preview), "list-item", nullptr);
                         const auto found =
                                 std::find(self->thumbnailWidgets.begin(), self->thumbnailWidgets.end(), preview);
                         if (found != self->thumbnailWidgets.end()) {
                             self->thumbnailWidgets.erase(found);
                         }
                         g_object_set_data(G_OBJECT(preview), "page", GINT_TO_POINTER(-1));
                     }),
                     this);
    pageList = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(selection), factory));
    thumbnailRenderer.setRedrawWidget(GTK_WIDGET(pageList));
    gtk_widget_add_css_class(GTK_WIDGET(pageList), "navigation-sidebar");
    gtk_widget_add_css_class(GTK_WIDGET(pageList), "page-thumbnails");
    auto* canvasCss = gtk_css_provider_new();
    constexpr char canvasCssData[] =
            "listview.page-thumbnails row:selected { background-color: transparent; box-shadow: none; }\n"
            "listview.page-thumbnails row:selected .page-thumbnail-number { color: #78aeed; font-weight: bold; }\n"
            ".canvas-text-editor { background-color: transparent; border: 1px solid #1c71d8; padding: 0; caret-color: "
            "#1c71d8; }\n"
            ".canvas-text-editor text { background-color: transparent; padding: 0; }\n"
            ".canvas-text-editor text { caret-color: #1c71d8; }\n";
    gtk_css_provider_load_from_string(canvasCss, canvasCssData);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(canvasCss),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(canvasCss);
    gtk_list_view_set_show_separators(pageList, false);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(pageList));
    g_signal_connect(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), "value-changed",
                     G_CALLBACK(+[](GtkAdjustment*, gpointer data) {
                         static_cast<Prototype*>(data)->prioritizeVisibleSidebarThumbnails();
                     }),
                     this);
    gtk_box_append(GTK_BOX(box), scroll);
    auto* layerPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(layerPanel, 8);
    gtk_widget_set_margin_end(layerPanel, 8);
    auto* layerHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    auto* layerTitle = gtk_label_new("Layers");
    gtk_widget_add_css_class(layerTitle, "heading");
    gtk_widget_set_hexpand(layerTitle, true);
    gtk_widget_set_halign(layerTitle, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(layerHeader), layerTitle);
    auto* layerActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(layerActions, "linked");
    layerAddButton = button("list-add-symbolic", "Add layer above current", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                auto* self = static_cast<Prototype*>(data);
                                self->finishTextEditing();
                                self->resetInput(true);
                                if (self->state.addLayerAboveCurrent()) {
                                    self->selectedImageIndex.reset();
                                    self->rebuildLayerStack();
                                    self->update();
                                }
                            }));
    layerDeleteButton =
            button("list-remove-symbolic", "Delete current layer", G_CALLBACK(+[](GtkButton*, gpointer data) {
                       auto* self = static_cast<Prototype*>(data);
                       self->finishTextEditing();
                       self->resetInput(true);
                       if (self->state.deleteCurrentLayer()) {
                           self->selectedImageIndex.reset();
                           self->rebuildLayerStack();
                           self->update();
                       }
                   }));
    layerMergeButton = gtk_button_new_with_label("Merge");
    gtk_widget_set_tooltip_text(layerMergeButton, "Merge current layer down");
    gtk_widget_add_css_class(layerMergeButton, "flat");
    g_signal_connect(layerMergeButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->finishTextEditing();
                         self->resetInput(true);
                         if (self->state.mergeCurrentLayerDown()) {
                             self->selectedImageIndex.reset();
                             self->rebuildLayerStack();
                             self->update();
                         }
                     }),
                     this);
    layerMenuButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(layerMenuButton), "view-more-symbolic");
    gtk_widget_set_tooltip_text(layerMenuButton, "More layer actions");
    layerPopover = gtk_popover_new();
    auto* layerMenu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(layerMenu, 6);
    gtk_widget_set_margin_bottom(layerMenu, 6);
    gtk_widget_set_margin_start(layerMenu, 6);
    gtk_widget_set_margin_end(layerMenu, 6);
    const auto appendLayerAction = [this, layerMenu](const char* title, GCallback callback) {
        auto* action = gtk_button_new_with_label(title);
        gtk_widget_set_halign(action, GTK_ALIGN_FILL);
        gtk_widget_add_css_class(action, "flat");
        g_signal_connect(action, "clicked", callback, this);
        gtk_box_append(GTK_BOX(layerMenu), action);
        return action;
    };
    layerRenameButton =
            appendLayerAction("Rename Layer…", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                  auto* self = static_cast<Prototype*>(data);
                                  gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                                  const auto& pageLayers = self->state.layersForPage(self->state.page);
                                  if (pageLayers.selectedLayer > 0) {
                                      self->openLayerRenameDialog(self->state.page, pageLayers.selectedLayer);
                                  }
                              }));
    layerRenameBackgroundButton = appendLayerAction("Rename Background…", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                                        auto* self = static_cast<Prototype*>(data);
                                                        gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                                                        self->openLayerRenameDialog(self->state.page, 0);
                                                    }));
    layerDuplicateButton = appendLayerAction("Duplicate Layer", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                                 auto* self = static_cast<Prototype*>(data);
                                                 gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                                                 self->finishTextEditing();
                                                 self->resetInput(true);
                                                 if (self->state.duplicateCurrentLayer()) {
                                                     self->refreshAfterLayerMutation();
                                                 }
                                             }));
    auto* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(separator, 4);
    gtk_widget_set_margin_bottom(separator, 4);
    gtk_box_append(GTK_BOX(layerMenu), separator);
    layerMoveUpButton = appendLayerAction("Move Layer Up", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                              auto* self = static_cast<Prototype*>(data);
                                              gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                                              self->finishTextEditing();
                                              self->resetInput(true);
                                              if (self->state.moveCurrentLayer(true)) {
                                                  self->refreshAfterLayerMutation();
                                              }
                                          }));
    layerMoveDownButton = appendLayerAction("Move Layer Down", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                                auto* self = static_cast<Prototype*>(data);
                                                gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                                                self->finishTextEditing();
                                                self->resetInput(true);
                                                if (self->state.moveCurrentLayer(false)) {
                                                    self->refreshAfterLayerMutation();
                                                }
                                            }));
    auto* visibilitySeparator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(visibilitySeparator, 4);
    gtk_widget_set_margin_bottom(visibilitySeparator, 4);
    gtk_box_append(GTK_BOX(layerMenu), visibilitySeparator);
    layerShowAllButton = appendLayerAction("Show All Layers", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                               auto* self = static_cast<Prototype*>(data);
                                               gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                                               self->finishTextEditing();
                                               self->resetInput(true);
                                               if (self->state.setAllLayersVisible(self->state.page, true)) {
                                                   self->refreshAfterLayerMutation();
                                               }
                                           }));
    layerHideAllButton = appendLayerAction("Hide All Layers", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                               auto* self = static_cast<Prototype*>(data);
                                               gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                                               self->finishTextEditing();
                                               self->resetInput(true);
                                               if (self->state.setAllLayersVisible(self->state.page, false)) {
                                                   self->refreshAfterLayerMutation();
                                               }
                                           }));
    gtk_popover_set_child(GTK_POPOVER(layerPopover), layerMenu);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(layerMenuButton), layerPopover);
    gtk_box_append(GTK_BOX(layerActions), layerAddButton);
    gtk_box_append(GTK_BOX(layerActions), layerDeleteButton);
    gtk_box_append(GTK_BOX(layerActions), layerMergeButton);
    gtk_box_append(GTK_BOX(layerActions), layerMenuButton);
    gtk_box_append(GTK_BOX(layerHeader), layerActions);
    gtk_box_append(GTK_BOX(layerPanel), layerHeader);
    layerRows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_size_request(layerRows, -1, 112);
    gtk_box_append(GTK_BOX(layerPanel), layerRows);
    gtk_box_append(GTK_BOX(box), layerPanel);
    rebuildLayerStack();
    return box;
}

void Prototype::rebuildLayerStack() {
    if (!layerRows) {
        return;
    }
    const auto& pageLayers = state.layersForPage(state.page);
    std::vector<GtkWidget*> existingRows;
    for (auto* child = gtk_widget_get_first_child(layerRows); child; child = gtk_widget_get_next_sibling(child)) {
        existingRows.push_back(child);
    }
    const bool sameRows = existingRows.size() == pageLayers.layers.size() + 1 &&
                          std::all_of(existingRows.begin(), existingRows.end(),
                                      [&](GtkWidget* row) {
                                          auto* visibility = gtk_widget_get_first_child(row);
                                          auto* select = visibility ? gtk_widget_get_next_sibling(visibility) : nullptr;
                                          return visibility && select && gtk_widget_get_next_sibling(select) == nullptr;
                                      }) &&
                          [&] {
                              for (size_t index = 0; index < existingRows.size(); ++index) {
                                  const size_t expectedId = index == 0 ? 0 : pageLayers.layers.size() - index + 1;
                                  auto* visibility = gtk_widget_get_first_child(existingRows[index]);
                                  const size_t actualId = static_cast<size_t>(
                                          GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(visibility), "layer-id")));
                                  if (actualId != expectedId) {
                                      return false;
                                  }
                              }
                              return true;
                          }();
    if (sameRows) {
        for (size_t index = 0; index < existingRows.size(); ++index) {
            const size_t layerId = index == 0 ? 0 : pageLayers.layers.size() - index + 1;
            const bool visible = layerId == 0 ? pageLayers.backgroundVisible : pageLayers.layers[layerId - 1].visible;
            const bool active = layerId != 0 && layerId == pageLayers.selectedLayer;
            const auto& layer = layerId == 0 ? prototype::CanvasLayer{} : pageLayers.layers[layerId - 1];
            const auto title = layerId == 0 ? pageLayers.backgroundName.value_or("Background") :
                                              (layer.name.empty() ? "Layer " + std::to_string(layerId) : layer.name);
            auto* visibility = GTK_TOGGLE_BUTTON(gtk_widget_get_first_child(existingRows[index]));
            auto* select = GTK_BUTTON(gtk_widget_get_next_sibling(GTK_WIDGET(visibility)));
            gtk_toggle_button_set_active(visibility, visible);
            gtk_button_set_icon_name(GTK_BUTTON(visibility),
                                     visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
            gtk_widget_set_tooltip_text(GTK_WIDGET(visibility), visible ? "Hide layer" : "Show layer");
            gtk_button_set_label(select, title.c_str());
            gtk_widget_set_tooltip_text(GTK_WIDGET(select), title.c_str());
            if (active) {
                gtk_widget_remove_css_class(GTK_WIDGET(select), "flat");
                gtk_widget_add_css_class(GTK_WIDGET(select), "suggested-action");
            } else {
                gtk_widget_add_css_class(GTK_WIDGET(select), "flat");
                gtk_widget_remove_css_class(GTK_WIDGET(select), "suggested-action");
            }
        }
        gtk_widget_set_sensitive(layerAddButton, bool(nativeSession) && pageLayers.layers.size() < 1000);
        gtk_widget_set_sensitive(layerDeleteButton, bool(nativeSession) && !pageLayers.layers.empty());
        gtk_widget_set_sensitive(layerMergeButton, bool(nativeSession) && pageLayers.selectedLayer > 1);
        updateLayerActionSensitivity(pageLayers);
        layerPanelPage = state.page;
        layerPanelSnapshot = pageLayers;
        return;
    }
    for (auto* child = gtk_widget_get_first_child(layerRows); child;) {
        auto* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(layerRows), child);
        child = next;
    }
    const auto appendVisibilityRow = [this](size_t layerId, const std::string& title, bool visible, bool active) {
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        auto* visibility = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
        gtk_button_set_icon_name(GTK_BUTTON(visibility), visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
        gtk_toggle_button_set_active(visibility, visible);
        gtk_widget_add_css_class(GTK_WIDGET(visibility), "flat");
        gtk_widget_set_tooltip_text(GTK_WIDGET(visibility), visible ? "Hide layer" : "Show layer");
        g_object_set_data(G_OBJECT(visibility), "layer-id", GUINT_TO_POINTER(static_cast<guint>(layerId)));
        g_signal_connect(
                visibility, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    const auto id =
                            static_cast<size_t>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "layer-id")));
                    const bool shown = gtk_toggle_button_get_active(button);
                    self->finishTextEditing();
                    self->resetInput(true);
                    gtk_button_set_icon_name(GTK_BUTTON(button),
                                             shown ? "view-reveal-symbolic" : "view-conceal-symbolic");
                    gtk_widget_set_tooltip_text(GTK_WIDGET(button), shown ? "Hide layer" : "Show layer");
                    if (self->state.setLayerVisible(self->state.page, id, shown)) {
                        if (self->selectedImageIndex && *self->selectedImageIndex < self->state.images.size() &&
                            self->state.images[*self->selectedImageIndex].page == self->state.page &&
                            self->state.images[*self->selectedImageIndex].layerId == id) {
                            self->selectedImageIndex.reset();
                        }
                        self->thumbnailRenderer.invalidate();
                        self->update();
                    }
                }),
                this);
        auto* select = gtk_button_new_with_label(title.c_str());
        gtk_widget_set_hexpand(select, true);
        gtk_widget_set_halign(select, GTK_ALIGN_FILL);
        gtk_widget_add_css_class(select, "flat");
        if (active) {
            gtk_widget_remove_css_class(select, "flat");
            gtk_widget_add_css_class(select, "suggested-action");
        }
        gtk_widget_set_tooltip_text(select, title.c_str());
        g_object_set_data(G_OBJECT(select), "layer-id", GUINT_TO_POINTER(static_cast<guint>(layerId)));
        g_signal_connect(select, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto id = static_cast<size_t>(
                                     GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "layer-id")));
                             self->finishTextEditing();
                             self->resetInput(true);
                             if (self->state.selectLayer(self->state.page, id)) {
                                 self->selectedImageIndex.reset();
                                 self->rebuildLayerStack();
                                 self->update();
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(row), GTK_WIDGET(visibility));
        gtk_box_append(GTK_BOX(row), select);
        gtk_box_append(GTK_BOX(layerRows), row);
    };

    appendVisibilityRow(0, pageLayers.backgroundName.value_or("Background"), pageLayers.backgroundVisible, false);
    for (size_t layerId = pageLayers.layers.size(); layerId > 0; --layerId) {
        const auto& layer = pageLayers.layers[layerId - 1];
        const auto title = layer.name.empty() ? "Layer " + std::to_string(layerId) : layer.name;
        appendVisibilityRow(layerId, title, layer.visible, layerId == pageLayers.selectedLayer);
    }
    gtk_widget_set_sensitive(layerAddButton, bool(nativeSession) && pageLayers.layers.size() < 1000);
    gtk_widget_set_sensitive(layerDeleteButton, bool(nativeSession) && !pageLayers.layers.empty());
    gtk_widget_set_sensitive(layerMergeButton, bool(nativeSession) && pageLayers.selectedLayer > 1);
    updateLayerActionSensitivity(pageLayers);
    layerPanelPage = state.page;
    layerPanelSnapshot = pageLayers;
}

GtkWidget* Prototype::findLayerControl(size_t layerId, bool visibility) const {
    for (auto* row = gtk_widget_get_first_child(layerRows); row; row = gtk_widget_get_next_sibling(row)) {
        for (auto* child = gtk_widget_get_first_child(row); child; child = gtk_widget_get_next_sibling(child)) {
            const auto id = static_cast<size_t>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "layer-id")));
            if (id == layerId && GTK_IS_TOGGLE_BUTTON(child) == visibility) {
                return child;
            }
        }
    }
    return nullptr;
}

void Prototype::updateLayerActionSensitivity(const prototype::PageLayers& pageLayers) {
    const bool hasSelection = pageLayers.selectedLayer > 0 && pageLayers.selectedLayer <= pageLayers.layers.size();
    gtk_widget_set_sensitive(layerRenameButton, bool(nativeSession) && hasSelection);
    gtk_widget_set_sensitive(layerRenameBackgroundButton, bool(nativeSession));
    gtk_widget_set_sensitive(layerDuplicateButton,
                             bool(nativeSession) && hasSelection && pageLayers.layers.size() < 1000);
    gtk_widget_set_sensitive(layerMoveUpButton, bool(nativeSession) && hasSelection &&
                                                        pageLayers.selectedLayer < pageLayers.layers.size());
    gtk_widget_set_sensitive(layerMoveDownButton, bool(nativeSession) && hasSelection && pageLayers.selectedLayer > 1);
    gtk_widget_set_sensitive(layerShowAllButton, bool(nativeSession) && !pageLayers.layers.empty());
    gtk_widget_set_sensitive(layerHideAllButton, bool(nativeSession) && !pageLayers.layers.empty());
}

void Prototype::refreshAfterLayerMutation() {
    selectedImageIndex.reset();
    canvasRenderer.invalidate();
    thumbnailRenderer.invalidate();
    rebuildLayerStack();
    update();
}

void Prototype::prioritizeVisibleSidebarThumbnails() {
    if (!pageList) {
        return;
    }
    const int viewportHeight = gtk_widget_get_height(GTK_WIDGET(pageList));
    if (viewportHeight <= 0) {
        return;
    }
    const double viewportCenter = viewportHeight / 2.0;
    std::map<int, double> visiblePages;
    for (auto* thumbnail: thumbnailWidgets) {
        const int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(thumbnail), "page"));
        if (page < 0 || !gtk_widget_get_mapped(thumbnail)) {
            continue;
        }
        graphene_rect_t bounds{};
        if (!gtk_widget_compute_bounds(thumbnail, GTK_WIDGET(pageList), &bounds) ||
            bounds.origin.y + bounds.size.height <= 0 || bounds.origin.y >= viewportHeight) {
            continue;
        }
        const double center = bounds.origin.y + bounds.size.height / 2.0;
        visiblePages.emplace(page, std::abs(center - viewportCenter));
    }
    thumbnailRenderer.setVisiblePages(visiblePages, state.page);
    for (const auto& [page, distance]: visiblePages) {
        requestSidebarThumbnail(page);
    }
}

void Prototype::requestSidebarThumbnail(int number) {
    if (number < 0 || number >= pageCount) {
        return;
    }
    if (thumbnailRenderer.find(number)) {
        return;
    }
    const auto& pageLayers = state.layersForPage(number);
    PageRasterRequest request;
    request.page = number;
    request.pageSize = state.pageSize(number);
    if (document && pageLayers.backgroundVisible) {
        request.backgroundPath = nativeSession ? nativeSession->previewPath().string() : sourcePath;
    }
    if (nativeSession) {
        for (const auto& layer: pageLayers.layers) {
            if (!layer.visible) {
                continue;
            }
            for (const size_t origin: layer.origins) {
                if (origin < nativeSession->layerPreviewPaths.size() &&
                    std::none_of(
                            request.layers.begin(), request.layers.end(),
                            [origin](const PageRasterLayerSource& source) { return source.origin == origin; })) {
                    request.layers.push_back({origin, nativeSession->layerPreviewPaths[origin].string()});
                }
            }
        }
    }
    thumbnailRenderer.request(std::move(request));
}

void Prototype::drawThumbnail(cairo_t* cr, int number, int width, int height, bool selected) {
    if (number < 0 || number >= pageCount || width <= 0 || height <= 0) {
        return;
    }
    requestSidebarThumbnail(number);
    const auto& pageLayers = state.layersForPage(number);
    const auto* preview = thumbnailRenderer.find(number);
    if (options.verifySidebarStartup && number == 0 && preview && preview->background) {
        ++sidebarStartupReadyDraws;
    }
    const auto pageSize = state.pageSize(number);
    const double scale = std::min(width / pageSize.width, height / pageSize.height);
    const double pageWidth = pageSize.width * scale;
    const double pageHeight = pageSize.height * scale;
    const double pageX = (width - pageWidth) / 2.0;
    const double pageY = (height - pageHeight) / 2.0;
    cairo_save(cr);
    cairo_rectangle(cr, pageX + 1, pageY + 2, pageWidth, pageHeight);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22);
    cairo_fill(cr);
    cairo_rectangle(cr, pageX, pageY, pageWidth, pageHeight);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_fill(cr);
    cairo_save(cr);
    cairo_translate(cr, pageX, pageY);
    if (preview && preview->background) {
        cairo_save(cr);
        cairo_scale(cr, scale / preview->scale, scale / preview->scale);
        cairo_set_source_surface(cr, preview->background.get(), 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    enum class EditableKind { Ink, Text, Image };
    struct LayerItem {
        size_t order;
        EditableKind kind;
        size_t index;
    };
    for (size_t layerIndex = 0; layerIndex < pageLayers.layers.size(); ++layerIndex) {
        const size_t layerId = layerIndex + 1;
        const auto& layer = pageLayers.layers[layerIndex];
        if (!layer.visible) {
            continue;
        }
        for (size_t origin: layer.origins) {
            if (preview) {
                const auto nativePreview = preview->layers.find(origin);
                if (nativePreview != preview->layers.end()) {
                    cairo_save(cr);
                    cairo_scale(cr, scale / preview->scale, scale / preview->scale);
                    cairo_set_source_surface(cr, nativePreview->second.get(), 0, 0);
                    cairo_paint(cr);
                    cairo_restore(cr);
                }
            }
            std::vector<LayerItem> items;
            items.reserve(state.ink.size() + state.texts.size());
            for (size_t index = 0; index < state.ink.size(); ++index) {
                if (state.displayPage(index) == number && state.displayLayerId(index) == layerId &&
                    state.displayLayerOrigin(index) == origin) {
                    items.push_back({state.ink[index].layerOrder, EditableKind::Ink, index});
                }
            }
            for (size_t index = 0; index < state.texts.size(); ++index) {
                if (state.texts[index].page == number && state.texts[index].layerId == layerId &&
                    state.texts[index].originLayer == origin) {
                    items.push_back({state.texts[index].layerOrder, EditableKind::Text, index});
                }
            }
            for (size_t index = 0; index < state.images.size(); ++index) {
                const auto& image = state.images[index];
                if (image.page == number && image.layerId == layerId && image.originLayer == origin) {
                    items.push_back({image.layerOrder, EditableKind::Image, index});
                }
            }
            std::stable_sort(items.begin(), items.end(),
                             [](const LayerItem& a, const LayerItem& b) { return a.order < b.order; });
            cairo_save(cr);
            cairo_scale(cr, scale, scale);
            for (const auto& item: items) {
                if (item.kind == EditableKind::Text) {
                    prototype::drawText(cr, state.texts[item.index]);
                } else if (item.kind == EditableKind::Image) {
                    prototype::drawCanvasImage(cr, displayedImage(item.index));
                } else {
                    prototype::drawInk(cr, state.ink[item.index], state.selectionMatrix(item.index));
                }
            }
            if (state.current && state.current->page == number && state.current->layerId == layerId &&
                state.current->originLayer == origin) {
                prototype::drawInk(cr, *state.current);
            }
            cairo_restore(cr);
        }
    }
    cairo_restore(cr);
    cairo_rectangle(cr, pageX + 0.5, pageY + 0.5, pageWidth - 1.0, pageHeight - 1.0);
    cairo_set_line_width(cr, selected ? 2.0 : 0.75);
    if (selected) {
        cairo_set_source_rgb(cr, 0.20, 0.52, 0.89);
    } else {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

void Prototype::scrollPageSidebarToCurrent() {
    if (!pageList || state.page < 0 || state.page >= pageCount) {
        return;
    }
#if GTK_CHECK_VERSION(4, 12, 0)
    auto* model = gtk_list_view_get_model(pageList);
    if (!model || static_cast<guint>(state.page) >= g_list_model_get_n_items(G_LIST_MODEL(model))) {
        return;
    }
    gtk_list_view_scroll_to(pageList, static_cast<guint>(state.page), GTK_LIST_SCROLL_NONE, nullptr);
#else
    auto* scroll = gtk_widget_get_parent(GTK_WIDGET(pageList));
    if (!GTK_IS_SCROLLED_WINDOW(scroll)) {
        return;
    }
    auto* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    const double lower = gtk_adjustment_get_lower(adjustment);
    const double upper = gtk_adjustment_get_upper(adjustment);
    const double pageSize = gtk_adjustment_get_page_size(adjustment);
    const double scrollable = upper - lower - pageSize;
    if (scrollable > 0 && pageCount > 1) {
        const double fraction = static_cast<double>(state.page) / static_cast<double>(pageCount - 1);
        gtk_adjustment_set_value(adjustment, lower + fraction * scrollable);
    }
#endif
}

}  // namespace prototype::gtk4
