#include "Prototype.h"

namespace prototype::gtk4 {

uint32_t Prototype::packTextColor(const std::array<double, 3>& color) {
    const auto red = static_cast<uint32_t>(std::lround(std::clamp(color[0], 0.0, 1.0) * 255));
    const auto green = static_cast<uint32_t>(std::lround(std::clamp(color[1], 0.0, 1.0) * 255));
    const auto blue = static_cast<uint32_t>(std::lround(std::clamp(color[2], 0.0, 1.0) * 255));
    return 0xff000000U | (red << 16U) | (green << 8U) | blue;
}

void Prototype::syncTextColorControl(uint32_t color) {
    state.textColor = {static_cast<double>((color >> 16U) & 0xffU) / 255.0,
                       static_cast<double>((color >> 8U) & 0xffU) / 255.0, static_cast<double>(color & 0xffU) / 255.0};
    if (state.brush.tool == Tool::Text) {
        state.brush.color = state.textColor;
    }
    if (colorButton) {
        const GdkRGBA rgba{static_cast<float>(state.textColor[0]), static_cast<float>(state.textColor[1]),
                           static_cast<float>(state.textColor[2]), 1};
        syncingBrush = true;
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
        syncingBrush = false;
    }
}

void Prototype::updateTextToolbarVisibility() {
    const bool editingText = textEditor && textEditor->active();
    const bool textContext = state.brush.tool == Tool::Text || editingText;
    const bool regularColorContext = state.brush.tool != Tool::Eraser && state.brush.tool != Tool::Hand &&
                                     state.brush.tool != Tool::Link && state.brush.tool != Tool::VerticalSpace &&
                                     !prototype::isSelectionTool(state.brush.tool);
    if (textFontEntry) {
        gtk_widget_set_visible(textFontEntry, textContext);
    }
    if (textFontSize) {
        gtk_widget_set_visible(textFontSize, textContext);
    }
    if (textFormatBox) {
        gtk_widget_set_visible(textFormatBox, textContext);
    }
    if (colorBox) {
        gtk_widget_set_visible(colorBox, textContext || regularColorContext);
    }
    if (colorButton) {
        gtk_widget_set_tooltip_text(colorButton, textContext ? "Text color" : "Ink color");
    }
}

bool Prototype::beginTextTransform(Device device, double x, double y) {
    if (state.brush.tool != Tool::Text || !nativeSession || (textEditor && textEditor->active()) || busy ||
        choosingFile || messageDialog || input.spaceHeld || input.panning() ||
        (device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
        return false;
    }

    auto hit = state.hitPage(x, y);
    if (!hit) {
        hit = state.hitSelectionHandle(x, y);
    }
    if (!hit) {
        return false;
    }
    if (state.pickLink(hit->page, hit->local)) {
        return false;
    }

    const bool textOnlySelection =
            !state.selectedTexts.empty() && state.selected.empty() && state.selectedImages.empty();
    const auto bounds = state.selectionBox();
    bool insideSelectedBounds = false;
    bool onSelectedHandle = false;
    if (textOnlySelection && bounds && bounds->page == hit->page) {
        const auto padding = 5 / state.zoom;
        insideSelectedBounds = hit->local.x >= bounds->x0 - padding && hit->local.x <= bounds->x1 + padding &&
                               hit->local.y >= bounds->y0 - padding && hit->local.y <= bounds->y1 + padding;
        for (const auto& handle: state.selectionHandles()) {
            if (std::hypot(handle.x - hit->local.x, handle.y - hit->local.y) <= 5 / state.zoom) {
                onSelectedHandle = true;
                break;
            }
        }
    }

    const auto pickedText = state.pickText(hit->page, hit->local);
    const bool pickedTextSelected =
            pickedText && textOnlySelection &&
            std::find(state.selectedTexts.begin(), state.selectedTexts.end(), *pickedText) != state.selectedTexts.end();
    if (!onSelectedHandle && pickedText && !pickedTextSelected) {
        state.clearSelection();
        state.selectedTexts = {*pickedText};
    } else if (!onSelectedHandle && !pickedText && !insideSelectedBounds) {
        return false;
    } else if (!textOnlySelection && pickedText) {
        state.clearSelection();
        state.selectedTexts = {*pickedText};
    } else if (!textOnlySelection && !pickedText) {
        return false;
    }

    state.selectActivePage(hit->page);
    selectedImageIndex.reset();
    const auto tool = state.brush.tool;
    state.brush.tool = Tool::SmartSelect;
    const bool started = state.beginSelection(x, y);
    state.brush.tool = tool;
    if (!started || !state.movingSelection()) {
        if (started) {
            state.cancelSelection();
        }
        return false;
    }

    textSelectionOwner = device;
    if (device == Device::Mouse) {
        textDragHandled = true;
    }
    return true;
}

void Prototype::applyTextColor(uint32_t color) {
    if (textEditor && textEditor->active()) {
        textEditor->setTextColor(color);
        gtk_widget_grab_focus(textEditor->widget());
    } else if (state.brush.tool == Tool::Text && !state.selectedTexts.empty()) {
        state.recolorSelectedTexts(color);
    }
}

void Prototype::startTextEditing(double x, double y) {
    if (!textEditor || !nativeSession || busy || choosingFile || messageDialog) {
        return;
    }
    const auto hit = state.hitPage(x, y);
    if (!hit) {
        return;
    }
    if (state.pickLink(hit->page, hit->local)) {
        return;
    }
    if (state.brush.tool == Tool::SmartSelect) {
        fitMode = false;
    }
    finishTextEditing();
    resetInput();
    state.clearSelection();
    state.selectActivePage(hit->page);
    const auto index = textAt(hit->page, hit->local);
    prototype::TextItem item;
    if (index) {
        item = state.texts[*index];
        syncTextColorControl(item.color);
    } else {
        item.page = hit->page;
        item.x = hit->local.x;
        item.y = hit->local.y;
        item.fontName = state.textFontName;
        item.fontSize = state.textFontSize;
        item.alignment = state.textAlignment;
        item.justify = state.textJustify;
        item.color = packTextColor(state.textColor);
    }
    textEditor->begin(item, index, state.zoom);
    textEditor->setWrap(item.wrapWidth >= 0);
    gtk_editable_set_text(GTK_EDITABLE(textFontEntry), item.fontName.c_str());
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), item.fontSize);
    updateTextToolbarVisibility();
    updateTextEditorPosition();
    update();
}

void Prototype::selectTextWordAt(double x, double y) {
    if (!textEditor || !textEditor->active()) {
        return;
    }
    const auto* item = textEditor->item();
    if (!item) {
        return;
    }
    const auto origin = state.pageScreenPoint(item->page, item->x, item->y);
    const auto left = std::max(0.0, std::floor(origin.x));
    const auto top = std::max(0.0, std::floor(origin.y));
    textEditor->selectWordAt(static_cast<int>(std::lround(x - left)), static_cast<int>(std::lround(y - top)));
}

void Prototype::finishTextEditing() {
    if (!textEditor || !textEditor->active()) {
        return;
    }
    auto completed = textEditor->finish();
    if (completed) {
        const auto [layoutWidth, layoutHeight] = prototype::textNaturalSize(completed->item);
        completed->item.layoutWidth = layoutWidth;
        completed->item.layoutHeight = layoutHeight;
        if (completed->existingIndex) {
            if (completed->hasContent) {
                state.replaceText(*completed->existingIndex, completed->item);
            } else {
                state.deleteText(*completed->existingIndex);
            }
        } else if (completed->hasContent) {
            state.insertText(completed->item);
        }
    }
    updateTextToolbarVisibility();
    if (canvas) {
        gtk_widget_grab_focus(canvas);
    }
    update();
}

void Prototype::updateTextEditorPosition() {
    if (!textEditor || !textEditor->active() || !canvas) {
        return;
    }
    const auto* item = textEditor->item();
    if (!item) {
        return;
    }
    textEditor->setZoom(state.zoom);
    const auto screen = state.pageScreenPoint(item->page, item->x, item->y);
    const auto [measuredWidth, measuredHeight] = prototype::measureText(*item);
    const double availableWidth = std::max(40.0, gtk_widget_get_width(canvas) - std::max(0.0, screen.x) - 8.0);
    const double requestedWidth = item->wrapWidth >= 0 ? item->wrapWidth * state.zoom + 16.0 :
                                                         std::max(180.0, (measuredWidth + 20.0) * state.zoom);
    const int width = static_cast<int>(std::clamp(requestedWidth, 40.0, availableWidth));
    const int height = static_cast<int>(std::max(36.0, (measuredHeight + 16.0) * state.zoom));
    textEditor->position(screen.x, screen.y, width, height);
}

void Prototype::applyTextFormat(prototype::TextEditorOverlay::Format format) {
    if (textEditor && textEditor->active()) {
        textEditor->applyFormat(format);
        gtk_widget_grab_focus(textEditor->widget());
    } else if (state.brush.tool == Tool::Text && !state.selectedTexts.empty()) {
        if (state.updateSelectedTexts([&](prototype::TextItem& item) {
                prototype::TextEditorOverlay::applyFormatToItem(item, format);
                const auto [width, height] = prototype::textNaturalSize(item);
                item.layoutWidth = width;
                item.layoutHeight = height;
            })) {
            update();
        }
    }
}

void Prototype::applyTextAlignment(int alignment) {
    if (alignment < 0 || alignment > 2) {
        return;
    }
    const int previous = importedConfig.settings.textAlignment;
    importedConfig.settings.textAlignment = alignment;
    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        importedConfig.settings.textAlignment = previous;
        showAlert("Could not save text preferences", error);
        return;
    }
    state.textAlignment = alignment;
    if (textEditor && textEditor->active()) {
        textEditor->setAlignment(alignment);
        gtk_widget_grab_focus(textEditor->widget());
        updateTextEditorPosition();
        update();
    } else if (state.brush.tool == Tool::Text && !state.selectedTexts.empty()) {
        if (state.updateSelectedTexts([&](prototype::TextItem& item) {
                item.alignment = alignment;
                const auto [width, height] = prototype::textNaturalSize(item);
                item.layoutWidth = width;
                item.layoutHeight = height;
            })) {
            update();
        }
    }
}

void Prototype::applyTextJustify(bool justify) {
    const bool previous = importedConfig.settings.textJustify;
    importedConfig.settings.textJustify = justify;
    std::string error;
    if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
        importedConfig.settings.textJustify = previous;
        showAlert("Could not save text preferences", error);
        return;
    }
    state.textJustify = justify;
    if (textEditor && textEditor->active()) {
        textEditor->setJustify(justify);
        gtk_widget_grab_focus(textEditor->widget());
        updateTextEditorPosition();
        update();
    } else if (state.brush.tool == Tool::Text && !state.selectedTexts.empty()) {
        if (state.updateSelectedTexts([&](prototype::TextItem& item) {
                item.justify = justify;
                const auto [width, height] = prototype::textNaturalSize(item);
                item.layoutWidth = width;
                item.layoutHeight = height;
            })) {
            update();
        }
    }
}

}  // namespace prototype::gtk4
