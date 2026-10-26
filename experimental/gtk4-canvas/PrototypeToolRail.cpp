#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::syncPenLineStyleDropdown() {
    if (!lineStyleDropdown) {
        return;
    }
    const auto style = selectedPenLineStyleState();
    if (!style) {
        return;
    }
    const guint selected = *style == "plain" || *style == "solid" ? 0u :
                           *style == "dash"                       ? 1u :
                           *style == "dashdot"                    ? 2u :
                           *style == "dot"                        ? 3u :
                           *style == "none"                       ? 5u :
                                                                    4u;
    if (gtk_drop_down_get_selected(GTK_DROP_DOWN(lineStyleDropdown)) != selected) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(lineStyleDropdown), selected);
    }
}

void Prototype::selectTool(Tool tool) {
    cancelScrollMomentum();
    if (tool != Tool::FloatingToolbox) {
        hideFloatingToolbox();
    }
    if (textEditor && textEditor->active()) {
        finishTextEditing();
    }
    if (tool != Tool::SmartSelect && tool != Tool::Hand) {
        selectedImageIndex.reset();
    }
    syncingTools = true;
    if (textButton) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(textButton), tool == Tool::Text);
    }
    if (latexButton) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(latexButton), tool == Tool::Latex);
    }
    if (linkButton) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(linkButton), tool == Tool::Link);
    }
    if (playObjectButton) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(playObjectButton), tool == Tool::PlayObject);
    }
    if (floatingToolboxToolButton) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(floatingToolboxToolButton), tool == Tool::FloatingToolbox);
    }
    for (const auto& [toolButtonTool, toolButton]: floatingToolboxToolButtons) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButton), toolButtonTool == tool);
    }
    if (laserButtons[0]) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(laserButtons[0]), tool == Tool::LaserPointerPen);
    }
    if (laserButtons[1]) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(laserButtons[1]), tool == Tool::LaserPointerHighlighter);
    }
    if (prototype::isSelectionTool(tool) || tool == Tool::VerticalSpace) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
    } else if (tool == Tool::Hand) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[4]), true);
    } else if (tool == Tool::Text || tool == Tool::Latex || tool == Tool::Link || tool == Tool::PlayObject ||
               tool == Tool::FloatingToolbox) {
        for (auto* button: toolButtons) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), false);
        }
    } else if (prototype::isLaserPointerTool(tool)) {
        for (auto* button: toolButtons) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), false);
        }
    } else if (static_cast<size_t>(tool) < 3) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[static_cast<size_t>(tool)]), true);
    }
    syncingTools = false;
    if (state.brush.tool == Tool::Pen) {
        penBrush = state.brush;
    } else if (state.brush.tool == Tool::Highlighter) {
        highlightBrush = state.brush;
    } else if (state.brush.tool == Tool::Eraser) {
        eraserBrush = state.brush;
        state.eraserWidth = eraserBrush.width;
    } else if (state.brush.tool == Tool::Latex) {
        latexBrush = state.brush;
    } else if (state.brush.tool == Tool::LaserPointerPen) {
        laserPenBrush = state.brush;
    } else if (state.brush.tool == Tool::LaserPointerHighlighter) {
        laserHighlighterBrush = state.brush;
    }
    if (textSelectionOwner) {
        stopSelectionEdgePan();
        state.cancelSelection();
    }
    textSelectionOwner.reset();
    pendingStylusTextTap.reset();
    smartSelectPdfGesture.reset();
    textDragHandled = false;
    input.cancel();
    state.cancelVerticalSpace();
    if (!prototype::isSelectionTool(tool) && tool != Tool::Hand) {
        state.clearSelection();
    }
    state.brush = tool == Tool::Highlighter || prototype::isPdfTextSelectionTool(tool) ? highlightBrush :
                  tool == Tool::Eraser                                                 ? eraserBrush :
                  tool == Tool::LaserPointerPen                                        ? laserPenBrush :
                  tool == Tool::LaserPointerHighlighter                                ? laserHighlighterBrush :
                  tool == Tool::Latex                                                  ? latexBrush :
                                                                                         penBrush;
    state.brush.tool = tool;
    selectionFillEnabled = false;
    if (!prototype::isFillableTool(tool)) {
        state.brush.fillEnabled = false;
    }
    if (tool == Tool::Text) {
        state.brush.color = state.textColor;
    }
    if (tool == Tool::Eraser) {
        state.brush.color = prototype::behavior::whiteoutColor;
        state.eraserWidth = state.brush.width;
    }
    syncingBrush = true;
    if (tool != Tool::Eraser) {
        const auto& color = state.brush.color;
        const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]), 1};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
    }
    gtk_widget_set_visible(colorBox, tool != Tool::Eraser && tool != Tool::Hand && tool != Tool::Link &&
                                             tool != Tool::PlayObject && tool != Tool::FloatingToolbox &&
                                             tool != Tool::VerticalSpace && !prototype::isSelectionTool(tool));
    gtk_widget_set_visible(sizeDropdown, !prototype::isSelectionTool(tool) && tool != Tool::Text &&
                                                 tool != Tool::Latex && tool != Tool::Hand &&
                                                 tool != Tool::VerticalSpace && tool != Tool::Link &&
                                                 tool != Tool::PlayObject && tool != Tool::FloatingToolbox);
    gtk_widget_set_visible(widthButton, !prototype::isSelectionTool(tool) && tool != Tool::Text &&
                                                tool != Tool::Latex && tool != Tool::Hand &&
                                                tool != Tool::VerticalSpace && tool != Tool::Link &&
                                                tool != Tool::PlayObject && tool != Tool::FloatingToolbox);
    gtk_widget_set_visible(lineStyleDropdown, tool == Tool::Pen || selectedPenStrokeCount() > 0);
    gtk_widget_set_visible(selectionLabel, prototype::isSelectionTool(tool));
    gtk_widget_set_visible(selectionModeDropdown, prototype::isSelectionTool(tool) || tool == Tool::VerticalSpace);
    gtk_widget_set_visible(selectionActions,
                           prototype::isSelectionTool(tool) && !prototype::isPdfTextSelectionTool(tool));
    gtk_widget_set_tooltip_text(selectionLabel,
                                prototype::isPdfTextSelectionTool(tool) ?
                                        "Drag across selectable PDF text. Choose Linear or Rectangle, then copy "
                                        "or annotate the selected text." :
                                tool == Tool::SelectObject ?
                                        "Click the nearest object on the active layer; Shift-click to add. "
                                        "Drag the selected object or its handles to transform it." :
                                        "Drag to select or move; drag a square handle to resize, or the circle "
                                        "handle to rotate.");
    const bool drawingTool = tool == Tool::Pen || tool == Tool::Highlighter;
    gtk_widget_set_visible(drawingOptionsExpander, drawingTool);
    const bool stabilizeFreehand = drawingTool && (state.drawingMode == prototype::DrawingMode::Freehand ||
                                                   state.drawingMode == prototype::DrawingMode::ShapeRecognizer);
    gtk_widget_set_visible(averagingDropdown, stabilizeFreehand);
    gtk_widget_set_visible(preprocessorDropdown, stabilizeFreehand);
    gtk_widget_set_visible(textFontEntry, tool == Tool::Text);
    gtk_widget_set_visible(textFontSize, tool == Tool::Text);
    gtk_widget_set_visible(textFormatBox, tool == Tool::Text);
    if (prototype::isSelectionTool(tool)) {
        selectionTool = tool;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), selectionModeIndex(tool));
    } else if (tool == Tool::VerticalSpace) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 8);
    }
    gtk_widget_set_sensitive(colorButton, tool != Tool::Eraser && tool != Tool::PlayObject);
    gtk_widget_set_sensitive(paletteButton, tool != Tool::Eraser && tool != Tool::PlayObject);
    if (paletteRailBox) {
        gtk_widget_set_sensitive(paletteRailBox, tool != Tool::Eraser && tool != Tool::PlayObject &&
                                                         tool != Tool::Hand && tool != Tool::Link);
    }
    gtk_widget_set_visible(eraserModeDropdown, tool == Tool::Eraser);
    if (tool != Tool::Link && linkInfoPopover) {
        gtk_popover_popdown(GTK_POPOVER(linkInfoPopover));
        linkInfoIndex.reset();
    }
    updateTextToolbarVisibility();
    gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), static_cast<guint>(state.eraserMode));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown), static_cast<guint>(state.drawingMode));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(averagingDropdown), static_cast<guint>(input.stabilizerConfig.averaging));
    gtk_drop_down_set_selected(GTK_DROP_DOWN(preprocessorDropdown),
                               static_cast<guint>(input.stabilizerConfig.preprocessor));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widthButton), state.brush.width);
    syncSizeDropdown();
    syncingBrush = false;
    if (canvas) {
        update();
    }
}

GtkWidget* Prototype::createPaletteSwatch(size_t index) {
    const auto& entry = importedConfig.palette.swatches[index];
    auto* swatch = gtk_button_new();
    gtk_widget_add_css_class(swatch, "flat");
    gtk_widget_set_size_request(swatch, 42, 34);
    auto* dot = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(dot), 20);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(dot), 20);
    g_object_set_data(G_OBJECT(dot), "swatch-index", GUINT_TO_POINTER(static_cast<guint>(index)));
    gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(dot),
            +[](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                const auto swatchIndex =
                        static_cast<size_t>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(area), "swatch-index")));
                if (swatchIndex >= self->importedConfig.palette.swatches.size()) {
                    return;
                }
                const auto& color = self->importedConfig.palette.swatches[swatchIndex].color;
                const double radius = std::max(2.0, std::min(width, height) / 2.0 - 2.0);
                cairo_arc(cr, width / 2.0, height / 2.0, radius, 0, 2 * G_PI);
                cairo_set_source_rgb(cr, color[0], color[1], color[2]);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 0.45, 0.45, 0.45, 0.65);
                cairo_set_line_width(cr, 1.0);
                cairo_stroke(cr);
            },
            this, nullptr);
    gtk_button_set_child(GTK_BUTTON(swatch), dot);
    gtk_widget_set_tooltip_text(swatch, entry.name.c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL, entry.name.c_str(), -1);
    g_object_set_data(G_OBJECT(swatch), "swatch-index", GUINT_TO_POINTER(static_cast<guint>(index)));
    g_signal_connect(
            swatch, "clicked", G_CALLBACK((+[](GtkButton* button, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                const auto swatchIndex =
                        static_cast<size_t>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "swatch-index")));
                if (swatchIndex < self->importedConfig.palette.swatches.size()) {
                    const auto& color = self->importedConfig.palette.swatches[swatchIndex].color;
                    const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]),
                                       static_cast<float>(color[2]), 1.0f};
                    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(self->colorButton), &rgba);
                }
                if (self->palettePopover) {
                    gtk_popover_popdown(GTK_POPOVER(self->palettePopover));
                }
                gtk_widget_grab_focus(self->textEditor && self->textEditor->active() ? self->textEditor->widget() :
                                                                                       self->canvas);
            })),
            this);
    return swatch;
}

GtkWidget* Prototype::mainToolWidget(prototype::MainToolAction action) const {
    switch (action) {
        case prototype::MainToolAction::SmartSelect:
            return toolButtons[3];
        case prototype::MainToolAction::Pen:
            return toolButtons[0];
        case prototype::MainToolAction::Highlighter:
            return toolButtons[1];
        case prototype::MainToolAction::Eraser:
            return toolButtons[2];
        case prototype::MainToolAction::Text:
            return textButton;
        case prototype::MainToolAction::Latex:
            return latexButton;
        case prototype::MainToolAction::Hand:
            return toolButtons[4];
        case prototype::MainToolAction::Link:
            return linkButton;
        case prototype::MainToolAction::LaserPointerPen:
            return laserButtons[0];
        case prototype::MainToolAction::LaserPointerHighlighter:
            return laserButtons[1];
        case prototype::MainToolAction::PlayObject:
            return playObjectButton;
        case prototype::MainToolAction::FloatingToolbox:
            return floatingToolboxToolButton;
    }
    return nullptr;
}

void Prototype::rebuildMainToolRail() {
    std::vector<GtkWidget*> retainedWidgets;
    const auto clearBox = [&retainedWidgets](GtkWidget* box) {
        for (auto* child = gtk_widget_get_first_child(box); child;) {
            auto* next = gtk_widget_get_next_sibling(child);
            g_object_ref(child);
            retainedWidgets.push_back(child);
            gtk_box_remove(GTK_BOX(box), child);
            child = next;
        }
    };
    clearBox(mainToolGroup);
    clearBox(moreToolsGroup);

    const auto& actions = importedConfig.settings.mainToolRail;
    for (const auto action: actions) {
        if (auto* widget = mainToolWidget(action)) {
            gtk_box_append(GTK_BOX(mainToolGroup), widget);
        }
    }
    bool hasMore = false;
    for (const auto& entry: prototype::mainToolRailNames) {
        if (std::find(actions.begin(), actions.end(), entry.first) != actions.end()) {
            continue;
        }
        if (auto* widget = mainToolWidget(entry.first)) {
            gtk_box_append(GTK_BOX(moreToolsGroup), widget);
            hasMore = true;
        }
    }
    gtk_widget_set_visible(moreToolsButton, hasMore);
    for (auto* widget: retainedWidgets) {
        g_object_unref(widget);
    }
}

GtkWidget* Prototype::createMainToolRail() {
    auto* contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    mainToolGroup = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(mainToolGroup, "linked");
    moreToolsButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(moreToolsButton), "view-more-symbolic");
    gtk_widget_set_tooltip_text(moreToolsButton, "More tools");
    auto* moreToolsPopover = gtk_popover_new();
    moreToolsGroup = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(moreToolsGroup, "linked");
    gtk_widget_set_margin_top(moreToolsGroup, 6);
    gtk_widget_set_margin_bottom(moreToolsGroup, 6);
    gtk_widget_set_margin_start(moreToolsGroup, 6);
    gtk_widget_set_margin_end(moreToolsGroup, 6);
    gtk_popover_set_child(GTK_POPOVER(moreToolsPopover), moreToolsGroup);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(moreToolsButton), moreToolsPopover);
    gtk_box_append(GTK_BOX(contents), mainToolGroup);
    gtk_box_append(GTK_BOX(contents), moreToolsButton);
    gtk_box_append(GTK_BOX(contents), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    paletteRailBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    paletteSwatchButtons.clear();
    for (size_t index = 0; index < importedConfig.palette.swatches.size(); ++index) {
        auto* swatch = createPaletteSwatch(index);
        paletteSwatchButtons.push_back(swatch);
        gtk_box_append(GTK_BOX(paletteRailBox), swatch);
    }
    gtk_box_append(GTK_BOX(contents), paletteRailBox);
    rebuildMainToolRail();

    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), contents);
    gtk_widget_set_size_request(scroll, 58, -1);
    gtk_widget_set_vexpand(scroll, true);
    gtk_widget_add_css_class(scroll, "sidebar");
    return scroll;
}

GtkWidget* Prototype::createPalette() {
    paletteButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(paletteButton), "view-more-symbolic");
    const auto buttonTip = "Quick colors (" + importedConfig.palette.paletteName + ")";
    gtk_widget_set_tooltip_text(paletteButton, buttonTip.c_str());
    palettePopover = gtk_popover_new();

    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    paletteContent = box;
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);

    auto* title = gtk_label_new(importedConfig.palette.paletteName.c_str());
    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), title);

    auto* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);

    const int columns = std::clamp(importedConfig.palette.columns, 4, 8);
    for (size_t i = 0; i < importedConfig.palette.swatches.size(); ++i) {
        auto* swatch = createPaletteSwatch(i);
        gtk_grid_attach(GTK_GRID(grid), swatch, static_cast<int>(i) % columns, static_cast<int>(i) / columns, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    gtk_popover_set_child(GTK_POPOVER(palettePopover), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(paletteButton), palettePopover);
    return paletteButton;
}

void Prototype::syncSizeDropdown() {
    if (!sizeDropdown) {
        return;
    }
    const auto matched =
            prototype::behavior::matchToolSize(prototype::widthsForTool(state.brush.tool), state.brush.width);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), matched ? static_cast<guint>(*matched) : 5u);
}

}  // namespace prototype::gtk4
