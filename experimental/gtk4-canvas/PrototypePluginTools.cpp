#include "Prototype.h"

namespace prototype::gtk4 {

std::optional<Tool> Prototype::pluginToolFromName(std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (const unsigned char character: name) {
        if (character != '_' && character != '-') {
            normalized += static_cast<char>(std::tolower(character));
        }
    }
    if (normalized == "pen")
        return Tool::Pen;
    if (normalized == "highlighter")
        return Tool::Highlighter;
    if (normalized == "eraser")
        return Tool::Eraser;
    if (normalized == "text")
        return Tool::Text;
    if (normalized == "selectrect")
        return Tool::SelectRectangle;
    if (normalized == "selectregion")
        return Tool::SelectLasso;
    if (normalized == "selectobject")
        return Tool::SelectObject;
    if (normalized == "selectmultilayerrect")
        return Tool::SelectRectangle;
    if (normalized == "selectmultilayerregion")
        return Tool::SelectLasso;
    if (normalized == "selectpdftextlinear")
        return Tool::SelectPdfTextLinear;
    if (normalized == "selectpdftextrect")
        return Tool::SelectPdfTextRect;
    if (normalized == "smartselect")
        return Tool::SmartSelect;
    if (normalized == "hand")
        return Tool::Hand;
    if (normalized == "laserpointerpen")
        return Tool::LaserPointerPen;
    if (normalized == "laserpointerhighlighter")
        return Tool::LaserPointerHighlighter;
    if (normalized == "link")
        return Tool::Link;
    if (normalized == "playobject")
        return Tool::PlayObject;
    if (normalized == "latex")
        return Tool::Latex;
    if (normalized == "verticalspace")
        return Tool::VerticalSpace;
    if (normalized == "showfloatingtoolbox")
        return Tool::FloatingToolbox;
    return std::nullopt;
}

std::string_view Prototype::pluginToolName(Tool tool) {
    switch (tool) {
        case Tool::Pen:
            return "pen";
        case Tool::Highlighter:
            return "highlighter";
        case Tool::Eraser:
            return "eraser";
        case Tool::SelectRectangle:
            return "selectRect";
        case Tool::SelectLasso:
            return "selectRegion";
        case Tool::SmartSelect:
            return "smartSelect";
        case Tool::Text:
            return "text";
        case Tool::SelectObject:
            return "selectObject";
        case Tool::Hand:
            return "hand";
        case Tool::SelectPdfTextLinear:
            return "selectPdfTextLinear";
        case Tool::SelectPdfTextRect:
            return "selectPdfTextRect";
        case Tool::VerticalSpace:
            return "verticalSpace";
        case Tool::Link:
            return "link";
        case Tool::LaserPointerPen:
            return "laserPointerPen";
        case Tool::LaserPointerHighlighter:
            return "laserPointerHighlighter";
        case Tool::PlayObject:
            return "playObject";
        case Tool::FloatingToolbox:
            return "showFloatingToolbox";
        case Tool::Latex:
            return "latex";
    }
    return "none";
}

std::string_view Prototype::pluginDrawingTypeName(prototype::DrawingMode mode) {
    switch (mode) {
        case prototype::DrawingMode::Freehand:
            return "default";
        case prototype::DrawingMode::Line:
            return "line";
        case prototype::DrawingMode::Rectangle:
            return "rectangle";
        case prototype::DrawingMode::Ellipse:
            return "ellipse";
        case prototype::DrawingMode::Arrow:
            return "arrow";
        case prototype::DrawingMode::DoubleArrow:
            return "doubleArrow";
        case prototype::DrawingMode::CoordinateSystem:
            return "drawCoordinateSystem";
        case prototype::DrawingMode::ShapeRecognizer:
            return "strokeRecognizer";
        case prototype::DrawingMode::Spline:
            return "spline";
    }
    return "default";
}

std::string_view Prototype::pluginToolSizeName(const std::array<double, 5>& widths, double width) {
    static constexpr std::array<std::string_view, 5> names{"veryThin", "thin", "medium", "thick", "veryThick"};
    const auto size = prototype::behavior::matchToolSize(widths, width);
    return size ? names[static_cast<size_t>(*size)] : std::string_view{"none"};
}

std::uint32_t Prototype::pluginRgbColor(const std::array<double, 3>& color) {
    const auto channel = [](double component) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(component, 0.0, 1.0) * 255.0));
    };
    return (channel(color[0]) << 16U) | (channel(color[1]) << 8U) | channel(color[2]);
}

bool Prototype::changePluginToolColor(const std::optional<std::string>& toolName, std::uint32_t color, bool selection,
                                      std::string& error) {
    auto tool = toolName ? pluginToolFromName(*toolName) : std::optional<Tool>{state.brush.tool};
    if (!tool) {
        error = "Unknown tool name";
        return false;
    }
    if (*tool == Tool::Eraser || *tool == Tool::Hand || *tool == Tool::Link || *tool == Tool::PlayObject ||
        *tool == Tool::VerticalSpace || *tool == Tool::FloatingToolbox) {
        error = "The selected tool does not support color changes";
        return false;
    }

    const std::array<double, 3> rgb{static_cast<double>((color >> 16U) & 0xffU) / 255.0,
                                    static_cast<double>((color >> 8U) & 0xffU) / 255.0,
                                    static_cast<double>(color & 0xffU) / 255.0};
    Tool colorTool = *tool;
    if (prototype::isSelectionTool(colorTool)) {
        colorTool = prototype::isPdfTextSelectionTool(colorTool) ? Tool::Highlighter : Tool::Pen;
    }

    if (colorTool == Tool::Pen) {
        penBrush.color = rgb;
    } else if (colorTool == Tool::Highlighter) {
        highlightBrush.color = rgb;
    } else if (colorTool == Tool::LaserPointerPen) {
        laserPenBrush.color = rgb;
    } else if (colorTool == Tool::LaserPointerHighlighter) {
        laserHighlighterBrush.color = rgb;
    } else if (colorTool == Tool::Text) {
        state.textColor = rgb;
    } else if (colorTool == Tool::Latex) {
        latexBrush.color = rgb;
    } else {
        error = "The selected tool does not support color changes";
        return false;
    }

    if (state.brush.tool == *tool || state.brush.tool == colorTool ||
        (prototype::isSelectionTool(state.brush.tool) &&
         (state.brush.tool == *tool || colorTool == Tool::Pen || colorTool == Tool::Highlighter))) {
        state.brush.color = rgb;
    }
    if (selection && state.hasSelection()) {
        state.recolorSelection(rgb, packTextColor(rgb));
    }
    input.cancel();
    if (colorButton && state.brush.tool != Tool::Eraser) {
        syncingBrush = true;
        const GdkRGBA rgba{static_cast<float>(state.brush.color[0]), static_cast<float>(state.brush.color[1]),
                           static_cast<float>(state.brush.color[2]), 1};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
        syncingBrush = false;
    }
    update();
    return true;
}

std::int64_t Prototype::pluginSelectedToolState() const {
    switch (state.brush.tool) {
        case Tool::Pen:
            // Drawing modes have separate upstream actions; SELECT_TOOL remains TOOL_PEN.
            return 1;
        case Tool::Eraser:
            return 2;
        case Tool::Highlighter:
            return 3;
        case Tool::Text:
            return 4;
        case Tool::SelectRectangle:
            return state.selectionAcrossLayers ? 8 : 6;
        case Tool::SelectLasso:
            return state.selectionAcrossLayers ? 9 : 7;
        case Tool::SelectObject:
            return 10;
        case Tool::PlayObject:
            return 11;
        case Tool::VerticalSpace:
            return 12;
        case Tool::Hand:
            return 13;
        case Tool::SelectPdfTextLinear:
            return 21;
        case Tool::SelectPdfTextRect:
            return 22;
        case Tool::LaserPointerPen:
            return 23;
        case Tool::LaserPointerHighlighter:
            return 24;
        case Tool::Link:
            return 25;
        case Tool::Latex:
            return 26;
        case Tool::SmartSelect:
            return 27;
        case Tool::FloatingToolbox:
            return 19;
    }
    return 0;
}

std::int64_t Prototype::pluginToolSizeState(const std::array<double, 5>& widths, double width) const {
    if (const auto size = prototype::behavior::matchToolSize(widths, width)) {
        return static_cast<std::int64_t>(*size);
    }
    return 5;  // app.C.ToolSize_none also represents custom widths.
}

std::int64_t Prototype::pluginActiveToolSizeState() const {
    switch (state.brush.tool) {
        case Tool::Pen:
        case Tool::Highlighter:
        case Tool::Eraser:
        case Tool::LaserPointerPen:
        case Tool::LaserPointerHighlighter:
            return pluginToolSizeState(prototype::widthsForTool(state.brush.tool), state.brush.width);
        default:
            return 5;
    }
}

bool Prototype::toolSupportsSelectionFill() const {
    return state.brush.tool == Tool::SelectRectangle || state.brush.tool == Tool::SelectLasso ||
           state.brush.tool == Tool::SelectObject || state.brush.tool == Tool::SmartSelect;
}

bool Prototype::fillToggleAvailable() const {
    return prototype::isFillableTool(state.brush.tool) ||
           (toolSupportsSelectionFill() && state.hasSelectedFillableInk());
}

bool Prototype::fillToggleActive() const {
    return toolSupportsSelectionFill() ? selectionFillEnabled : state.brush.fillEnabled;
}

bool Prototype::setFillEnabled(bool enabled, std::string& error) {
    const Tool tool = state.brush.tool;
    if (prototype::isFillableTool(tool)) {
        const auto previousSettings = importedConfig.settings;
        const auto previousBrush = state.brush;
        const auto previousPenBrush = penBrush;
        const auto previousHighlightBrush = highlightBrush;
        input.cancel();
        state.brush.fillEnabled = enabled;
        if (tool == Tool::Pen) {
            state.brush.fillOpacity = importedConfig.settings.penFillOpacity;
            penBrush = state.brush;
            importedConfig.settings.penFillEnabled = enabled;
        } else {
            state.brush.fillOpacity = importedConfig.settings.highlighterFillOpacity;
            highlightBrush = state.brush;
            importedConfig.settings.highlighterFillEnabled = enabled;
        }
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
            importedConfig.settings = previousSettings;
            state.brush = previousBrush;
            penBrush = previousPenBrush;
            highlightBrush = previousHighlightBrush;
            update();
            return false;
        }
    } else if (toolSupportsSelectionFill() && state.hasSelectedFillableInk()) {
        input.cancel();
        state.setSelectedInkFill(enabled ? importedConfig.settings.penFillOpacity : -1,
                                 enabled ? importedConfig.settings.highlighterFillOpacity : -1);
        selectionFillEnabled = enabled;
    } else {
        error = "Fill is available for Pen, Highlighter, or selected Pen/Highlighter strokes";
        return false;
    }
    update();
    return true;
}

bool Prototype::fillOpacityAvailable() const { return prototype::isFillableTool(state.brush.tool); }

bool Prototype::setFillOpacity(Tool tool, int opacity, std::string& error) {
    if (!prototype::isFillableTool(tool) || opacity < 0 || opacity > 255) {
        error = "Fill opacity is available for Pen or Highlighter and must be between 0 and 255";
        return false;
    }

    const auto previousSettings = importedConfig.settings;
    const auto previousBrush = state.brush;
    const auto previousPenBrush = penBrush;
    const auto previousHighlightBrush = highlightBrush;
    if (tool == Tool::Pen) {
        importedConfig.settings.penFillOpacity = opacity;
        penBrush.fillOpacity = opacity;
    } else {
        importedConfig.settings.highlighterFillOpacity = opacity;
        highlightBrush.fillOpacity = opacity;
    }
    if (state.brush.tool == tool) {
        state.brush.fillOpacity = opacity;
    }

    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        importedConfig.settings = previousSettings;
        state.brush = previousBrush;
        penBrush = previousPenBrush;
        highlightBrush = previousHighlightBrush;
        update();
        return false;
    }

    update();
    return true;
}

void Prototype::closeFillOpacityDialog(bool apply) {
    auto* dialog = fillOpacityDialog;
    if (!dialog) {
        return;
    }
    const Tool tool = fillOpacityTarget;
    const int percent = fillOpacityScale ? static_cast<int>(std::lround(gtk_range_get_value(fillOpacityScale))) : 50;
    const int opacity = static_cast<int>(std::lround(percent * 2.55));
    fillOpacityDialog = nullptr;
    fillOpacityScale = nullptr;
    fillOpacityPreview = nullptr;
    fillOpacityCancelButton = nullptr;
    fillOpacityApplyButton = nullptr;
    adw_dialog_force_close(dialog);
    if (apply) {
        std::string error;
        if (!setFillOpacity(tool, opacity, error)) {
            showAlert("Could not change fill opacity", error);
        }
    }
}

bool Prototype::openFillOpacityDialog(std::string& error) {
    if (closed || busy || choosingFile || messageDialog) {
        error = "Wait for the current document operation to finish before changing fill opacity";
        return false;
    }
    if (!fillOpacityAvailable()) {
        error = "Fill opacity is available for Pen and Highlighter";
        return false;
    }
    if (fillOpacityDialog) {
        adw_dialog_present(fillOpacityDialog, window);
        return true;
    }

    fillOpacityTarget = state.brush.tool;
    const int initialOpacity = fillOpacityTarget == Tool::Pen ? importedConfig.settings.penFillOpacity :
                                                                importedConfig.settings.highlighterFillOpacity;
    fillOpacityDialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(fillOpacityDialog, "Opacity settings");
    adw_dialog_set_content_width(fillOpacityDialog, 360);
    adw_dialog_set_presentation_mode(fillOpacityDialog, ADW_DIALOG_FLOATING);

    auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(content, 12);
    gtk_widget_set_margin_bottom(content, 12);
    gtk_widget_set_margin_start(content, 12);
    gtk_widget_set_margin_end(content, 12);
    adw_dialog_set_child(fillOpacityDialog, content);

    auto* description = gtk_label_new(nullptr);
    const char* toolName = fillOpacityTarget == Tool::Pen ? "Pen" : "Highlighter";
    const std::string descriptionMarkup =
            std::string("<b>") + toolName + " Options</b>\nSelect opacity for: Fill color";
    gtk_label_set_markup(GTK_LABEL(description), descriptionMarkup.c_str());
    gtk_label_set_xalign(GTK_LABEL(description), 0);
    gtk_box_append(GTK_BOX(content), description);

    auto* previewRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    fillOpacityPreview = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_content_width(fillOpacityPreview, 70);
    gtk_drawing_area_set_content_height(fillOpacityPreview, 50);
    gtk_drawing_area_set_draw_func(
            fillOpacityPreview,
            +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                cairo_set_source_rgb(cr, 1, 1, 1);
                cairo_paint(cr);
                const int percent =
                        self->fillOpacityScale ?
                                static_cast<int>(std::lround(gtk_range_get_value(GTK_RANGE(self->fillOpacityScale)))) :
                                50;
                const double alpha = std::lround(percent * 2.55) / 255.0;
                cairo_set_source_rgba(cr, 0, 128.0 / 255.0, 0, alpha);
                cairo_rectangle(cr, 10, 10, std::max(0, width - 20), std::max(0, height - 20));
                cairo_fill(cr);
                if (self->fillOpacityTarget == Tool::Pen) {
                    cairo_set_source_rgb(cr, 0, 128.0 / 255.0, 0);
                    cairo_set_line_width(cr, 5);
                    cairo_rectangle(cr, 10, 10, std::max(0, width - 20), std::max(0, height - 20));
                    cairo_stroke(cr);
                }
            },
            this, nullptr);
    gtk_box_append(GTK_BOX(previewRow), GTK_WIDGET(fillOpacityPreview));

    fillOpacityScale = GTK_RANGE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 5, 100, 1));
    gtk_widget_set_hexpand(GTK_WIDGET(fillOpacityScale), true);
    gtk_widget_set_size_request(GTK_WIDGET(fillOpacityScale), 190, -1);
    gtk_scale_set_draw_value(GTK_SCALE(fillOpacityScale), true);
    gtk_scale_set_digits(GTK_SCALE(fillOpacityScale), 0);
    gtk_scale_set_format_value_func(
            GTK_SCALE(fillOpacityScale),
            +[](GtkScale*, double value, gpointer) { return g_strdup_printf("%.0f%%", value); }, nullptr, nullptr);
    gtk_accessible_update_property(GTK_ACCESSIBLE(fillOpacityScale), GTK_ACCESSIBLE_PROPERTY_LABEL, "Fill opacity", -1);
    gtk_range_set_value(fillOpacityScale, initialOpacity / 2.55);
    g_signal_connect(fillOpacityScale, "value-changed", G_CALLBACK(+[](GtkRange*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->fillOpacityPreview) {
                             gtk_widget_queue_draw(GTK_WIDGET(self->fillOpacityPreview));
                         }
                     }),
                     this);
    gtk_box_append(GTK_BOX(previewRow), GTK_WIDGET(fillOpacityScale));
    gtk_box_append(GTK_BOX(content), previewRow);
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    fillOpacityCancelButton = GTK_BUTTON(gtk_button_new_with_label("Cancel"));
    fillOpacityApplyButton = GTK_BUTTON(gtk_button_new_with_label("OK"));
    gtk_widget_add_css_class(GTK_WIDGET(fillOpacityApplyButton), "suggested-action");
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(fillOpacityCancelButton));
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(fillOpacityApplyButton));
    gtk_box_append(GTK_BOX(content), actions);
    adw_dialog_set_default_widget(fillOpacityDialog, GTK_WIDGET(fillOpacityApplyButton));
    g_signal_connect(fillOpacityCancelButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Prototype*>(data)->closeFillOpacityDialog(false);
                     }),
                     this);
    g_signal_connect(fillOpacityApplyButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Prototype*>(data)->closeFillOpacityDialog(true);
                     }),
                     this);
    g_signal_connect(fillOpacityDialog, "closed", G_CALLBACK(+[](AdwDialog* dialog, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->fillOpacityDialog == dialog) {
                             self->fillOpacityDialog = nullptr;
                             self->fillOpacityScale = nullptr;
                             self->fillOpacityPreview = nullptr;
                             self->fillOpacityCancelButton = nullptr;
                             self->fillOpacityApplyButton = nullptr;
                         }
                     }),
                     this);
    adw_dialog_present(fillOpacityDialog, window);
    return true;
}

size_t Prototype::selectedPenStrokeCount() const {
    return static_cast<size_t>(std::count_if(state.selected.begin(), state.selected.end(), [&](size_t index) {
        return index < state.ink.size() && state.ink[index].brush.tool == Tool::Pen;
    }));
}

std::optional<std::string> Prototype::selectedPenLineStyleState() const {
    std::optional<prototype::LineDashPattern> selectedStyle;
    bool hasSelectedPenStroke = false;
    bool mixedStyles = false;
    for (const size_t index: state.selected) {
        if (index >= state.ink.size() || state.ink[index].brush.tool != Tool::Pen) {
            continue;
        }
        const auto& lineDashes = state.ink[index].brush.lineDashes;
        if (!selectedStyle) {
            selectedStyle = lineDashes;
        } else if (*selectedStyle != lineDashes) {
            mixedStyles = true;
        }
        hasSelectedPenStroke = true;
    }
    if (hasSelectedPenStroke) {
        return mixedStyles ? std::string{"none"} : prototype::formatLineStyle(*selectedStyle);
    }
    if (state.brush.tool == Tool::Pen) {
        return prototype::formatLineStyle(penBrush.lineDashes);
    }
    return std::nullopt;
}

bool Prototype::getPluginToolInfo(std::string_view tool, prototype::PluginToolInfo& info, std::string& error) const {
    using prototype::PluginToolInfo;

    const auto setBrush = [&](const prototype::Brush& brush, const std::array<double, 5>& widths) {
        info.sizeName = pluginToolSizeName(widths, brush.width);
        info.sizeValue = brush.width;
        info.color = pluginRgbColor(brush.color);
    };
    const auto setShapeStyle = [&] { info.drawingType = pluginDrawingTypeName(state.drawingMode); };

    if (tool == "active") {
        info.type = pluginToolName(state.brush.tool);
        setBrush(state.brush, prototype::widthsForTool(state.brush.tool));
        setShapeStyle();
        info.lineStyle = prototype::formatLineStyle(state.brush.lineDashes);
        info.filled = state.brush.fillEnabled;
        info.fillOpacity = state.brush.fillOpacity;
        return true;
    }
    if (tool == "pen") {
        setBrush(penBrush, prototype::behavior::penWidths);
        setShapeStyle();
        info.lineStyle = prototype::formatLineStyle(penBrush.lineDashes);
        info.filled = penBrush.fillEnabled;
        info.fillOpacity = penBrush.fillOpacity;
        return true;
    }
    if (tool == "highlighter") {
        setBrush(highlightBrush, prototype::behavior::highlighterWidths);
        setShapeStyle();
        info.filled = highlightBrush.fillEnabled;
        info.fillOpacity = highlightBrush.fillOpacity;
        return true;
    }
    if (tool == "eraser") {
        info.eraserType = state.eraserMode == prototype::EraserMode::Standard ? "default" :
                          state.eraserMode == prototype::EraserMode::Whiteout ? "whiteout" :
                                                                                "deleteStroke";
        setBrush(eraserBrush, prototype::behavior::eraserWidths);
        return true;
    }
    if (tool == "text") {
        info.fontName = state.textFontName;
        info.fontSize = state.textFontSize;
        info.color = pluginRgbColor(state.textColor);
        return true;
    }
    if (tool == "selection") {
        if (!state.hasSelection()) {
            error = "There is no selection!";
            return false;
        }
        const auto bounds = state.selectionBox();
        if (!bounds) {
            error = "Selection bounds are unavailable";
            return false;
        }
        const prototype::PluginToolRectangle rectangle{bounds->x0, bounds->y0, bounds->x1 - bounds->x0,
                                                       bounds->y1 - bounds->y0};
        info.boundingBox = rectangle;
        info.originalBounds = rectangle;
        info.snappedBounds = rectangle;
        info.rotation = state.selectionRotation();
        return true;
    }

    // The upstream API returns an empty table for an unknown query mode.
    return true;
}

}  // namespace prototype::gtk4
