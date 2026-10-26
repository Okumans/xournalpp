#include "Prototype.h"

namespace prototype::gtk4 {

PopplerPage* Prototype::pdfPageForPage(int page) {
    PopplerDocument* source = document;
    int sourcePage = page;
    if (nativeSession) {
        if (page < 0 || static_cast<size_t>(page) >= nativeSession->pdfPageMap.size()) {
            return nullptr;
        }
        sourcePage = nativeSession->pdfPageMap[static_cast<size_t>(page)];
        if (sourcePage < 0) {
            return nullptr;
        }
        const auto textSourcePath = nativeSession->backgroundPdfPath().string();
        if (textSourcePath != pdfTextDocumentPath) {
            g_clear_object(&pdfTextDocument);
            if (std::filesystem::is_regular_file(nativeSession->backgroundPdfPath())) {
                std::vector<prototype::PageSize> ignoredSizes;
                std::tie(pdfTextDocument, ignoredSizes) = openPreviewPdf(textSourcePath);
                pdfTextDocumentPath = textSourcePath;
            }
        }
        source = pdfTextDocument;
    }
    if (!source || sourcePage < 0 || sourcePage >= poppler_document_get_n_pages(source)) {
        return nullptr;
    }
    return poppler_document_get_page(source, sourcePage);
}

bool Prototype::smartSelectHasSelectedObjectAt(int page, prototype::Point point) const {
    if (!state.hasSelection()) {
        return false;
    }
    const auto bounds = state.selectionBox();
    if (!bounds || bounds->page != page) {
        return false;
    }
    const double padding = 5 / std::max(0.1, state.zoom);
    return point.x >= bounds->x0 - padding && point.x <= bounds->x1 + padding && point.y >= bounds->y0 - padding &&
           point.y <= bounds->y1 + padding;
}

bool Prototype::smartSelectHasEditableObjectAt(int page, prototype::Point point) const {
    return state.pickSmartSelectObject(page, point).has_value();
}

bool Prototype::pdfTextAtCanvasPoint(int pageIndex, prototype::Point point) {
    auto* page = pdfPageForPage(pageIndex);
    if (!page) {
        return false;
    }
    const bool found = prototype::pdfTextAt(page, point.x, point.y);
    g_object_unref(page);
    return found;
}

bool Prototype::beginSmartSelectPdfGesture(Device device, double x, double y) {
    if (state.brush.tool != Tool::SmartSelect || input.shiftHeld || input.spaceHeld ||
        (device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
        return false;
    }
    const auto hit = state.hitPage(x, y);
    if (!hit || smartSelectHasSelectedObjectAt(hit->page, hit->local)) {
        return false;
    }

    const bool altPressed = input.altHeld;
    if (!altPressed && smartSelectHasEditableObjectAt(hit->page, hit->local)) {
        return false;
    }
    if (!altPressed && !pdfTextAtCanvasPoint(hit->page, hit->local)) {
        return false;
    }

    smartSelectPdfGesture = SmartSelectPdfGesture{device, {x, y}, {x, y}, altPressed};
    return true;
}

void Prototype::updatePdfActionsPosition(bool popup) {
    if (!pdfActionsPopover || !pdfTextSelection || !pdfTextSelection->finalized || !pdfTextSelection->region ||
        cairo_region_num_rectangles(pdfTextSelection->region.get()) == 0) {
        return;
    }
    constexpr int gap = 8;
    const int viewportWidth = gtk_widget_get_width(canvasOverlay);
    const int viewportHeight = gtk_widget_get_height(canvasOverlay);
    int minimumWidth = 0;
    int naturalWidth = 0;
    int minimumHeight = 0;
    int naturalHeight = 0;
    int minimumBaseline = -1;
    int naturalBaseline = -1;
    gtk_widget_measure(GTK_WIDGET(pdfActionsPopover), GTK_ORIENTATION_HORIZONTAL, -1, &minimumWidth, &naturalWidth,
                       &minimumBaseline, &naturalBaseline);
    gtk_widget_measure(GTK_WIDGET(pdfActionsPopover), GTK_ORIENTATION_VERTICAL, std::max(1, naturalWidth),
                       &minimumHeight, &naturalHeight, &minimumBaseline, &naturalBaseline);
    const int width = std::max(naturalWidth, gtk_widget_get_width(GTK_WIDGET(pdfActionsPopover)));
    const int height = std::max(naturalHeight, gtk_widget_get_height(GTK_WIDGET(pdfActionsPopover)));
    const bool rightFits = pdfTextSelection->toolbarAnchor.x + width + gap <= viewportWidth;
    const bool aboveFits = pdfTextSelection->toolbarAnchor.y >= height + gap;
    int left = static_cast<int>(std::lround(rightFits ? pdfTextSelection->toolbarAnchor.x + gap :
                                                        pdfTextSelection->toolbarAnchor.x - width - gap));
    int top = static_cast<int>(std::lround(aboveFits ? pdfTextSelection->toolbarAnchor.y - height - gap :
                                                       pdfTextSelection->toolbarAnchor.y + gap));
    left = std::clamp(left, 0, std::max(0, viewportWidth - width));
    top = std::clamp(top, 0, std::max(0, viewportHeight - height));

    gtk_popover_set_position(pdfActionsPopover, aboveFits ? GTK_POS_TOP : GTK_POS_BOTTOM);
    GdkRectangle pointing{left + width / 2, aboveFits ? top + height : top, 1, 1};
    gtk_popover_set_pointing_to(pdfActionsPopover, &pointing);
    if (pdfSelectionStyleButton) {
        const char* label =
                pdfTextSelection->style == prototype::PdfTextSelectionStyle::Linear ? "Rectangle" : "Linear";
        gtk_button_set_label(GTK_BUTTON(pdfSelectionStyleButton), label);
    }
    if (popup) {
        gtk_popover_popup(pdfActionsPopover);
    }
}

void Prototype::hidePdfActionsPreservingSelection() {
    if (pdfActionsPopover) {
        gtk_popover_popdown(pdfActionsPopover);
    }
}

bool Prototype::reopenPdfActionsForSelection(double x, double y) {
    if (!pdfTextSelection || !pdfTextSelection->finalized || !pdfTextSelection->region) {
        return false;
    }
    const auto hit = state.hitPage(x, y);
    if (!hit || hit->page != pdfTextSelection->page ||
        !cairo_region_contains_point(pdfTextSelection->region.get(), static_cast<int>(hit->local.x),
                                     static_cast<int>(hit->local.y))) {
        return false;
    }
    pdfTextSelection->toolbarAnchor = {x, y};
    updatePdfActionsPosition(true);
    return true;
}

void Prototype::clearPdfTextSelection(bool update) {
    pdfTextSelection.reset();
    if (pdfActionsPopover) {
        gtk_popover_popdown(pdfActionsPopover);
    }
    if (update && canvas) {
        this->update();
    }
}

void Prototype::beginPdfTextSelection(Device device, double x, double y) {
    clearPdfTextSelection(false);
    state.clearSelection();
    selectedImageIndex.reset();
    const auto hit = state.hitPage(x, y);
    if (!hit) {
        update();
        return;
    }
    state.selectActivePage(hit->page);
    auto* page = pdfPageForPage(hit->page);
    if (!page) {
        update();
        return;
    }
    PdfTextSelectionState selection;
    selection.owner = device;
    selection.page = hit->page;
    selection.tool = state.brush.tool;
    selection.style = pdfSelectionStyleForTool(selection.tool);
    selection.bounds = {hit->local.x, hit->local.y, hit->local.x, hit->local.y};
    selection.toolbarAnchor = {x, y};
    selection.dragging = true;
    selection.region = prototype::pdfTextSelectionPreview(page, selection.bounds, selection.style);
    g_object_unref(page);
    pdfTextSelection = std::move(selection);
    update();
}

void Prototype::updatePdfTextSelection(Device device, double x, double y) {
    if (!pdfTextSelection || !pdfTextSelection->dragging || pdfTextSelection->owner != device) {
        return;
    }
    const auto size = state.pageSize(pdfTextSelection->page);
    const auto local = state.pageLocalPoint(pdfTextSelection->page, x, y);
    pdfTextSelection->toolbarAnchor = {x, y};
    pdfTextSelection->bounds.x2 = std::clamp(local.x, 0.0, size.width);
    pdfTextSelection->bounds.y2 = std::clamp(local.y, 0.0, size.height);
    auto* page = pdfPageForPage(pdfTextSelection->page);
    if (!page) {
        clearPdfTextSelection(false);
        update();
        return;
    }
    pdfTextSelection->region =
            prototype::pdfTextSelectionPreview(page, pdfTextSelection->bounds, pdfTextSelection->style);
    g_object_unref(page);
    update();
}

void Prototype::finishPdfTextSelection(Device device, std::optional<prototype::Point> toolbarAnchor) {
    if (!pdfTextSelection || !pdfTextSelection->dragging || pdfTextSelection->owner != device) {
        return;
    }
    if (toolbarAnchor) {
        pdfTextSelection->toolbarAnchor = *toolbarAnchor;
    }
    pdfTextSelection->dragging = false;
    auto* page = pdfPageForPage(pdfTextSelection->page);
    if (!page) {
        clearPdfTextSelection(false);
        update();
        return;
    }
    auto result = prototype::selectPdfText(page, pdfTextSelection->bounds, pdfTextSelection->style);
    g_object_unref(page);
    if (result.text.empty() || result.textRects.empty()) {
        clearPdfTextSelection(false);
        update();
        return;
    }
    pdfTextSelection->text = std::move(result.text);
    pdfTextSelection->textRects = std::move(result.textRects);
    pdfTextSelection->region = std::move(result.region);
    pdfTextSelection->finalized = true;
    if (auto* primary = gdk_display_get_primary_clipboard(gtk_widget_get_display(canvas))) {
        gdk_clipboard_set_text(primary, pdfTextSelection->text.c_str());
    }
    updatePdfActionsPosition(true);
    update();
}

void Prototype::switchPdfTextSelectionStyle() {
    const bool smartSelectPdfResult =
            state.brush.tool == Tool::SmartSelect && pdfTextSelection && pdfTextSelection->finalized;
    if (!prototype::isPdfTextSelectionTool(state.brush.tool) && !smartSelectPdfResult) {
        return;
    }
    const auto currentStyle = pdfTextSelection ? pdfTextSelection->style : pdfSelectionStyleForTool(state.brush.tool);
    const Tool next = currentStyle == prototype::PdfTextSelectionStyle::Linear ? Tool::SelectPdfTextRect :
                                                                                 Tool::SelectPdfTextLinear;
    if (!smartSelectPdfResult) {
        state.brush.tool = next;
        selectionTool = next;
        if (selectionModeDropdown) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), selectionModeIndex(next));
        }
    }
    if (pdfTextSelection && pdfTextSelection->finalized) {
        if (!smartSelectPdfResult) {
            pdfTextSelection->tool = next;
        }
        const auto nextStyle = pdfSelectionStyleForTool(next);
        if (nextStyle == prototype::PdfTextSelectionStyle::Rectangle &&
            currentStyle != prototype::PdfTextSelectionStyle::Rectangle && !pdfTextSelection->textRects.empty()) {
            auto bounds = pdfTextSelection->textRects.front();
            for (const auto& rect: pdfTextSelection->textRects) {
                bounds.x1 = std::min(bounds.x1, std::min(rect.x1, rect.x2));
                bounds.y1 = std::min(bounds.y1, std::min(rect.y1, rect.y2));
                bounds.x2 = std::max(bounds.x2, std::max(rect.x1, rect.x2));
                bounds.y2 = std::max(bounds.y2, std::max(rect.y1, rect.y2));
            }
            pdfTextSelection->bounds = bounds;
        }
        pdfTextSelection->style = nextStyle;
        auto* page = pdfPageForPage(pdfTextSelection->page);
        if (page) {
            auto result = prototype::selectPdfText(page, pdfTextSelection->bounds, pdfTextSelection->style);
            g_object_unref(page);
            pdfTextSelection->text = std::move(result.text);
            pdfTextSelection->textRects = std::move(result.textRects);
            pdfTextSelection->region = std::move(result.region);
            if (pdfTextSelection->text.empty() || pdfTextSelection->textRects.empty()) {
                clearPdfTextSelection(false);
            }
        }
    }
    updatePdfActionsPosition(false);
    update();
}

void Prototype::selectPdfTextAtPoint(double x, double y, prototype::PdfTextSelectionStyle style) {
    const auto hit = state.hitPage(x, y);
    if (!hit) {
        clearPdfTextSelection(false);
        update();
        return;
    }
    state.selectActivePage(hit->page);
    auto* page = pdfPageForPage(hit->page);
    if (!page) {
        clearPdfTextSelection(false);
        update();
        return;
    }

    constexpr double clickRadius = 1.0;
    PdfTextSelectionState selection;
    selection.page = hit->page;
    selection.tool = state.brush.tool;
    selection.style = style;
    selection.bounds = {hit->local.x - clickRadius, hit->local.y - clickRadius, hit->local.x + clickRadius,
                        hit->local.y + clickRadius};
    selection.toolbarAnchor = {x, y};
    auto result = prototype::selectPdfText(page, selection.bounds, style);
    g_object_unref(page);
    if (result.text.empty() || result.textRects.empty()) {
        clearPdfTextSelection(false);
        update();
        return;
    }
    selection.text = std::move(result.text);
    selection.textRects = std::move(result.textRects);
    selection.region = std::move(result.region);
    selection.finalized = true;
    pdfTextSelection = std::move(selection);
    if (auto* primary = gdk_display_get_primary_clipboard(gtk_widget_get_display(canvas))) {
        gdk_clipboard_set_text(primary, pdfTextSelection->text.c_str());
    }
    updatePdfActionsPosition(true);
    update();
}

void Prototype::copyPdfTextSelection(bool dismiss) {
    if (!pdfTextSelection || !pdfTextSelection->finalized || pdfTextSelection->text.empty()) {
        return;
    }
    auto* display = gtk_widget_get_display(canvas);
    if (auto* clipboard = gdk_display_get_clipboard(display)) {
        gdk_clipboard_set_text(clipboard, pdfTextSelection->text.c_str());
    }
    if (auto* primary = gdk_display_get_primary_clipboard(display)) {
        gdk_clipboard_set_text(primary, pdfTextSelection->text.c_str());
    }
    if (dismiss) {
        clearPdfTextSelection(false);
        update();
    }
}

bool Prototype::samePdfHighlight(const Ink& stroke, int page, size_t layerId, size_t originLayer,
                                 const PopplerRectangle& rect, int fill) {
    constexpr double tolerance = 1.0;
    if (stroke.page != page || stroke.layerId != layerId || stroke.originLayer != originLayer ||
        stroke.brush.tool != Tool::Highlighter || stroke.pressureSensitive || stroke.fill != fill ||
        stroke.capStyle != 1 || stroke.points.size() != 2) {
        return false;
    }
    const double left = std::min(rect.x1, rect.x2);
    const double right = std::max(rect.x1, rect.x2);
    const double top = std::min(rect.y1, rect.y2);
    const double bottom = std::max(rect.y1, rect.y2);
    const double y = (top + bottom) / 2;
    const double width = bottom - top;
    const double strokeLeft = std::min(stroke.points[0].x, stroke.points[1].x);
    const double strokeRight = std::max(stroke.points[0].x, stroke.points[1].x);
    return std::abs(stroke.points[0].y - stroke.points[1].y) <= tolerance && std::abs(strokeLeft - left) <= tolerance &&
           std::abs(strokeRight - right) <= tolerance && std::abs(stroke.points[0].y - y) <= tolerance &&
           std::abs(stroke.brush.width - width) <= tolerance;
}

void Prototype::createPdfTextAnnotation(PdfTextAnnotation annotation) {
    if (!pdfTextSelection || !pdfTextSelection->finalized || pdfTextSelection->textRects.empty()) {
        return;
    }
    const bool isHighlight = annotation == PdfTextAnnotation::Highlight;
    const auto color = isHighlight ? highlightBrush.color : pdfTextColor;
    const int fill = isHighlight ? pdfTextMarkerOpacity : 230;
    const size_t layerId = state.activeLayerId(pdfTextSelection->page);
    const size_t originLayer = state.activeLayerOrigin(pdfTextSelection->page);
    std::vector<std::pair<size_t, Ink>> replacements;
    std::vector<Ink> additions;
    std::vector<size_t> used;

    for (const auto& rect: pdfTextSelection->textRects) {
        const double left = std::min(rect.x1, rect.x2);
        const double right = std::max(rect.x1, rect.x2);
        const double top = std::min(rect.y1, rect.y2);
        const double bottom = std::max(rect.y1, rect.y2);
        const double height = bottom - top;
        if (!(right > left) || !(height > 0)) {
            continue;
        }
        const double y = annotation == PdfTextAnnotation::Underline ? bottom : (top + bottom) / 2;
        Ink stroke{pdfTextSelection->page,
                   {{left, y, 1}, {right, y, 1}},
                   {Tool::Highlighter, color, isHighlight ? height : 1.0}};
        stroke.fill = fill;
        stroke.capStyle = 1;

        bool matched = false;
        if (isHighlight) {
            for (size_t i = 0; i < state.ink.size(); ++i) {
                if (std::find(used.begin(), used.end(), i) != used.end() ||
                    !samePdfHighlight(state.ink[i], pdfTextSelection->page, layerId, originLayer, rect, fill)) {
                    continue;
                }
                used.push_back(i);
                matched = true;
                if (state.ink[i].brush.color != color) {
                    auto replacement = state.ink[i];
                    replacement.brush.color = color;
                    replacements.emplace_back(i, std::move(replacement));
                }
                break;
            }
        }
        if (!matched) {
            additions.push_back(std::move(stroke));
        }
    }
    if (!replacements.empty() || !additions.empty()) {
        state.applyInkEdits(std::move(replacements), std::move(additions));
    }
    clearPdfTextSelection(false);
    update();
}

void Prototype::setPdfHighlightColor(const std::array<double, 3>& color) {
    highlightBrush.color = color;
    if (prototype::isPdfTextSelectionTool(state.brush.tool)) {
        state.brush.color = color;
    }
    if (pdfHighlightColorButton) {
        const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]), 1};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pdfHighlightColorButton), &rgba);
    }
    update();
}

void Prototype::createPdfActionsPopover() {
    pdfActionsPopover = GTK_POPOVER(gtk_popover_new());
    gtk_popover_set_position(pdfActionsPopover, GTK_POS_TOP);
    gtk_popover_set_has_arrow(pdfActionsPopover, false);
    gtk_popover_set_autohide(pdfActionsPopover, false);
    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(actions, "toolbar");
    gtk_widget_add_css_class(actions, "linked");
    gtk_widget_set_margin_top(actions, 4);
    gtk_widget_set_margin_bottom(actions, 4);
    gtk_widget_set_margin_start(actions, 4);
    gtk_widget_set_margin_end(actions, 4);

    pdfCopyButton = button("edit-copy-symbolic", "Copy selected PDF text", G_CALLBACK(+[](GtkButton*, gpointer data) {
                               static_cast<Prototype*>(data)->copyPdfTextSelection();
                           }));
    gtk_box_append(GTK_BOX(actions), pdfCopyButton);
    pdfHighlightButton =
            button("format-text-highlight-symbolic", "Highlight selected PDF text",
                   G_CALLBACK((+[](GtkButton*, gpointer data) {
                       static_cast<Prototype*>(data)->createPdfTextAnnotation(PdfTextAnnotation::Highlight);
                   })));
    gtk_box_append(GTK_BOX(actions), pdfHighlightButton);
    pdfUnderlineButton =
            button("format-text-underline-symbolic", "Underline selected PDF text",
                   G_CALLBACK((+[](GtkButton*, gpointer data) {
                       static_cast<Prototype*>(data)->createPdfTextAnnotation(PdfTextAnnotation::Underline);
                   })));
    gtk_box_append(GTK_BOX(actions), pdfUnderlineButton);
    pdfStrikethroughButton =
            button("format-text-strikethrough-symbolic", "Strikethrough selected PDF text",
                   G_CALLBACK((+[](GtkButton*, gpointer data) {
                       static_cast<Prototype*>(data)->createPdfTextAnnotation(PdfTextAnnotation::Strikethrough);
                   })));
    gtk_box_append(GTK_BOX(actions), pdfStrikethroughButton);

    pdfSelectionStyleButton = gtk_button_new_with_label("Rectangle");
    gtk_widget_set_tooltip_text(pdfSelectionStyleButton, "Switch PDF text selection style");
    g_signal_connect(pdfSelectionStyleButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<Prototype*>(data)->switchPdfTextSelectionStyle();
                     }),
                     this);
    gtk_box_append(GTK_BOX(actions), pdfSelectionStyleButton);
    gtk_box_append(GTK_BOX(actions), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    auto* markerDialog = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(markerDialog, false);
    pdfHighlightColorButton = gtk_color_dialog_button_new(markerDialog);
    gtk_widget_set_tooltip_text(pdfHighlightColorButton, "PDF highlight color");
    gtk_accessible_update_property(GTK_ACCESSIBLE(pdfHighlightColorButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "PDF highlight color", -1);
    const auto& markerColor = highlightBrush.color;
    const GdkRGBA markerRgba{static_cast<float>(markerColor[0]), static_cast<float>(markerColor[1]),
                             static_cast<float>(markerColor[2]), 1};
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pdfHighlightColorButton), &markerRgba);
    g_signal_connect(pdfHighlightColorButton, "notify::rgba", G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto* rgba = gtk_color_dialog_button_get_rgba(
                                 GTK_COLOR_DIALOG_BUTTON(self->pdfHighlightColorButton));
                         self->setPdfHighlightColor({rgba->red, rgba->green, rgba->blue});
                     })),
                     this);
    gtk_box_append(GTK_BOX(actions), pdfHighlightColorButton);

    pdfHighlightPaletteButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pdfHighlightPaletteButton), "view-grid-symbolic");
    gtk_widget_set_tooltip_text(pdfHighlightPaletteButton, "Highlight color presets");
    auto* presetPopover = gtk_popover_new();
    auto* presetGrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(presetGrid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(presetGrid), 2);
    gtk_widget_set_margin_top(presetGrid, 6);
    gtk_widget_set_margin_bottom(presetGrid, 6);
    gtk_widget_set_margin_start(presetGrid, 6);
    gtk_widget_set_margin_end(presetGrid, 6);
    for (size_t index = 0; index < pdfHighlightPresets.size(); ++index) {
        const auto& preset = pdfHighlightPresets[index];
        auto* swatch = gtk_button_new();
        pdfHighlightPresetButtons[index] = swatch;
        gtk_widget_add_css_class(swatch, "flat");
        gtk_widget_set_tooltip_text(swatch, preset.name);
        gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL, preset.name, -1);
        auto* dot = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(dot), 24);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(dot), 24);
        auto* color = g_new(GdkRGBA, 1);
        *color = {static_cast<float>(preset.color[0]), static_cast<float>(preset.color[1]),
                  static_cast<float>(preset.color[2]), 1};
        g_object_set_data_full(G_OBJECT(dot), "pdf-highlight-preset-color", color, g_free);
        gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(dot),
                +[](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer) {
                    auto* rgba = static_cast<GdkRGBA*>(g_object_get_data(G_OBJECT(area), "pdf-highlight-preset-color"));
                    if (!rgba) {
                        return;
                    }
                    cairo_arc(cr, width / 2.0, height / 2.0, std::min(width, height) / 2.0 - 2, 0, 2 * G_PI);
                    gdk_cairo_set_source_rgba(cr, rgba);
                    cairo_fill_preserve(cr);
                    cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.75);
                    cairo_set_line_width(cr, 1);
                    cairo_stroke(cr);
                },
                nullptr, nullptr);
        gtk_button_set_child(GTK_BUTTON(swatch), dot);
        g_object_set_data(G_OBJECT(swatch), "pdf-highlight-preset-index",
                          GUINT_TO_POINTER(static_cast<guint>(index + 1)));
        g_signal_connect(
                swatch, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    const auto index = static_cast<size_t>(
                            GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "pdf-highlight-preset-index")) - 1);
                    if (index < pdfHighlightPresets.size()) {
                        self->setPdfHighlightColor(pdfHighlightPresets[index].color);
                    }
                    auto* popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(self->pdfHighlightPaletteButton));
                    if (popover) {
                        gtk_popover_popdown(GTK_POPOVER(popover));
                    }
                }),
                this);
        gtk_grid_attach(GTK_GRID(presetGrid), swatch, static_cast<int>(index % 3), static_cast<int>(index / 3), 1, 1);
    }
    gtk_popover_set_child(GTK_POPOVER(presetPopover), presetGrid);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(pdfHighlightPaletteButton), presetPopover);
    gtk_box_append(GTK_BOX(actions), pdfHighlightPaletteButton);

    pdfMarkerOpacityButton = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pdfMarkerOpacityButton), "applications-graphics-symbolic");
    gtk_widget_set_tooltip_text(pdfMarkerOpacityButton, "PDF highlight marker opacity");
    auto* opacityPopover = gtk_popover_new();
    auto* opacityBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(opacityBox, 8);
    gtk_widget_set_margin_bottom(opacityBox, 8);
    gtk_widget_set_margin_start(opacityBox, 8);
    gtk_widget_set_margin_end(opacityBox, 8);
    auto* opacityLabel = gtk_label_new("Marker opacity");
    gtk_widget_set_halign(opacityLabel, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(opacityBox), opacityLabel);
    pdfMarkerOpacityScale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
    gtk_widget_set_size_request(pdfMarkerOpacityScale, 160, -1);
    gtk_scale_set_draw_value(GTK_SCALE(pdfMarkerOpacityScale), true);
    gtk_range_set_value(GTK_RANGE(pdfMarkerOpacityScale), pdfTextMarkerOpacity);
    gtk_accessible_update_property(GTK_ACCESSIBLE(pdfMarkerOpacityScale), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "PDF highlight marker opacity", -1);
    g_signal_connect(pdfMarkerOpacityScale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->pdfTextMarkerOpacity = static_cast<int>(std::lround(gtk_range_get_value(range)));
                     }),
                     this);
    gtk_box_append(GTK_BOX(opacityBox), pdfMarkerOpacityScale);
    gtk_popover_set_child(GTK_POPOVER(opacityPopover), opacityBox);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(pdfMarkerOpacityButton), opacityPopover);
    gtk_box_append(GTK_BOX(actions), pdfMarkerOpacityButton);

    auto* textDialog = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(textDialog, false);
    pdfTextColorButton = gtk_color_dialog_button_new(textDialog);
    gtk_widget_set_tooltip_text(pdfTextColorButton, "PDF underline and strikethrough color");
    gtk_accessible_update_property(GTK_ACCESSIBLE(pdfTextColorButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "PDF annotation text color", -1);
    const GdkRGBA textRgba{static_cast<float>(pdfTextColor[0]), static_cast<float>(pdfTextColor[1]),
                           static_cast<float>(pdfTextColor[2]), 1};
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pdfTextColorButton), &textRgba);
    g_signal_connect(pdfTextColorButton, "notify::rgba", G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto* rgba =
                                 gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(self->pdfTextColorButton));
                         self->pdfTextColor = {rgba->red, rgba->green, rgba->blue};
                     })),
                     this);
    gtk_box_append(GTK_BOX(actions), pdfTextColorButton);

    gtk_popover_set_child(pdfActionsPopover, actions);
    gtk_widget_set_parent(GTK_WIDGET(pdfActionsPopover), canvasOverlay);
}

}  // namespace prototype::gtk4
