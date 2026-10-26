#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::createLinkInfoPopover() {
    linkInfoPopover = gtk_popover_new();
    gtk_popover_set_autohide(GTK_POPOVER(linkInfoPopover), false);
    linkInfoLabel = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(linkInfoLabel), true);
    gtk_label_set_wrap(GTK_LABEL(linkInfoLabel), true);
    gtk_widget_set_margin_top(linkInfoLabel, 8);
    gtk_widget_set_margin_bottom(linkInfoLabel, 8);
    gtk_widget_set_margin_start(linkInfoLabel, 10);
    gtk_widget_set_margin_end(linkInfoLabel, 10);
    gtk_popover_set_child(GTK_POPOVER(linkInfoPopover), linkInfoLabel);
    gtk_overlay_add_overlay(GTK_OVERLAY(canvasOverlay), linkInfoPopover);
    gtk_widget_set_can_target(linkInfoPopover, false);
}

std::optional<size_t> Prototype::textAt(int page, prototype::Point point) const {
    std::optional<size_t> best;
    size_t bestOrder = 0;
    for (size_t index = 0; index < state.texts.size(); ++index) {
        const auto& item = state.texts[index];
        if (!item.isLink && item.page == page && prototype::textContainsPoint(item, point.x, point.y) &&
            (!best || item.layerOrder >= bestOrder)) {
            best = index;
            bestOrder = item.layerOrder;
        }
    }
    return best;
}

void Prototype::closeLinkEditor() {
    if (messageDialog) {
        adw_dialog_close(ADW_DIALOG(messageDialog));
    }
    messageDialog = nullptr;
    linkTextInput = nullptr;
    linkUrlInput = nullptr;
    linkPrefixDropdown = nullptr;
    linkFontEntry = nullptr;
    linkFontSize = nullptr;
    linkAlignmentDropdown = nullptr;
    editingLinkIndex.reset();
    linkEditorPage = -1;
}

void Prototype::openLinkEditor(int page, prototype::Point position, std::optional<size_t> index) {
    if (busy || choosingFile || messageDialog) {
        return;
    }
    if (!nativeSession) {
        showError("Link editing requires an open document worker.");
        return;
    }
    if (index && (*index >= state.texts.size() || !state.texts[*index].isLink)) {
        return;
    }
    resetInput(true);
    if (linkInfoPopover) {
        gtk_popover_popdown(GTK_POPOVER(linkInfoPopover));
    }

    const auto* existing = index ? &state.texts[*index] : nullptr;
    messageDialog = ADW_ALERT_DIALOG(
            adw_alert_dialog_new(existing ? "Edit Link" : "Add Link", "Link text is shown underlined on the page."));
    auto* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 8);
    gtk_widget_set_margin_start(grid, 8);
    gtk_widget_set_margin_end(grid, 8);

    auto attachRow = [&](int row, const char* title, GtkWidget* control) {
        auto* label = gtk_label_new(title);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
        gtk_widget_set_hexpand(control, true);
        gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
    };

    auto* textView = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView), GTK_WRAP_WORD_CHAR);
    auto* textScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(textScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(textScroll), textView);
    gtk_widget_set_size_request(textScroll, 340, 76);
    attachRow(0, "Text", textScroll);
    linkTextInput = GTK_TEXT_VIEW(textView);

    const char* prefixOptions[] = {"https://", "http://", "mailto:", "file://", "Custom", nullptr};
    linkPrefixDropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(prefixOptions));
    auto* urlBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(urlBox), GTK_WIDGET(linkPrefixDropdown));
    linkUrlInput = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(linkUrlInput), true);
    gtk_box_append(GTK_BOX(urlBox), GTK_WIDGET(linkUrlInput));
    attachRow(1, "URL", urlBox);

    linkFontEntry = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(linkFontEntry), 18);
    attachRow(2, "Font", GTK_WIDGET(linkFontEntry));
    linkFontSize = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 200, 0.5));
    gtk_spin_button_set_digits(linkFontSize, 1);
    attachRow(3, "Size", GTK_WIDGET(linkFontSize));
    const char* alignmentOptions[] = {"Left", "Center", "Right", nullptr};
    linkAlignmentDropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(alignmentOptions));
    attachRow(4, "Alignment", GTK_WIDGET(linkAlignmentDropdown));

    if (existing) {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), existing->text.c_str(), -1);
        std::string urlValue = existing->linkUrl;
        guint prefix = 4;
        const std::array<std::string, 4> prefixes{"https://", "http://", "mailto:", "file://"};
        for (guint candidate = 0; candidate < prefixes.size(); ++candidate) {
            if (urlValue.starts_with(prefixes[candidate])) {
                prefix = candidate;
                urlValue.erase(0, prefixes[candidate].size());
                break;
            }
        }
        gtk_drop_down_set_selected(linkPrefixDropdown, prefix);
        gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), urlValue.c_str());
        gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), existing->fontName.c_str());
        gtk_spin_button_set_value(linkFontSize, existing->fontSize);
        gtk_drop_down_set_selected(linkAlignmentDropdown, static_cast<guint>(existing->alignment));
    } else {
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), "Link", -1);
        gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), "xournalpp.github.io");
        gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), state.textFontName.c_str());
        gtk_spin_button_set_value(linkFontSize, state.textFontSize);
        gtk_drop_down_set_selected(linkPrefixDropdown, 0);
        gtk_drop_down_set_selected(linkAlignmentDropdown, 0);
    }

    adw_alert_dialog_set_extra_child(messageDialog, grid);
    adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "save", existing ? "Save" : "Add", nullptr);
    adw_alert_dialog_set_response_appearance(messageDialog, "save", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(messageDialog, "cancel");
    adw_alert_dialog_set_default_response(messageDialog, "save");
    editingLinkIndex = index;
    linkEditorPage = page;
    linkEditorPosition = position;
    g_signal_connect(
            messageDialog, "response", G_CALLBACK((+[](AdwAlertDialog*, const char* response, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                const bool save = response && std::string_view(response) == "save";
                std::optional<prototype::TextItem> edited;
                const auto oldIndex = self->editingLinkIndex;
                if (save && self->linkTextInput && self->linkUrlInput && self->linkPrefixDropdown &&
                    self->linkFontEntry && self->linkFontSize && self->linkAlignmentDropdown) {
                    GtkTextIter start, end;
                    auto* buffer = gtk_text_view_get_buffer(self->linkTextInput);
                    gtk_text_buffer_get_bounds(buffer, &start, &end);
                    gchar* rawText = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
                    std::string text = rawText ? rawText : "";
                    g_free(rawText);
                    if (std::all_of(text.begin(), text.end(), [](unsigned char ch) { return g_ascii_isspace(ch); })) {
                        text = "Link";
                    }

                    static constexpr std::array<const char*, 4> prefixes{"https://", "http://", "mailto:", "file://"};
                    const guint prefix = gtk_drop_down_get_selected(self->linkPrefixDropdown);
                    std::string url = gtk_editable_get_text(GTK_EDITABLE(self->linkUrlInput));
                    if (url.empty()) {
                        url = "xournalpp.github.io";
                    }
                    if (prefix < prefixes.size()) {
                        url.insert(0, prefixes[prefix]);
                    }
                    auto item = oldIndex && *oldIndex < self->state.texts.size() ? self->state.texts[*oldIndex] :
                                                                                   prototype::TextItem{};
                    item.isLink = true;
                    item.linkUrl = std::move(url);
                    item.text = std::move(text);
                    const char* font = gtk_editable_get_text(GTK_EDITABLE(self->linkFontEntry));
                    item.fontName = font && *font ? font : "Sans";
                    item.fontSize = gtk_spin_button_get_value(self->linkFontSize);
                    item.alignment = std::min<int>(2, gtk_drop_down_get_selected(self->linkAlignmentDropdown));
                    item.justify = false;
                    item.wrapWidth = -1;
                    item.styleRuns.clear();
                    if (!oldIndex) {
                        item.page = self->linkEditorPage;
                        item.x = self->linkEditorPosition.x;
                        item.y = self->linkEditorPosition.y;
                        item.color = 0xffff00ffU;
                    }
                    const auto [width, height] = prototype::textNaturalSize(item);
                    item.layoutWidth = width;
                    item.layoutHeight = height;
                    edited = std::move(item);
                }
                self->messageDialog = nullptr;
                self->linkTextInput = nullptr;
                self->linkUrlInput = nullptr;
                self->linkPrefixDropdown = nullptr;
                self->linkFontEntry = nullptr;
                self->linkFontSize = nullptr;
                self->linkAlignmentDropdown = nullptr;
                self->editingLinkIndex.reset();
                self->linkEditorPage = -1;
                if (edited) {
                    const bool changed = oldIndex ? self->state.replaceText(*oldIndex, std::move(*edited)) :
                                                    self->state.insertText(std::move(*edited));
                    if (!changed) {
                        self->showError("The link could not be added to the active layer.");
                    }
                }
                if (self->canvas) {
                    gtk_widget_grab_focus(self->canvas);
                    self->update();
                }
            })),
            this);
    adw_dialog_present(ADW_DIALOG(messageDialog), window);
    gtk_widget_grab_focus(GTK_WIDGET(linkUrlInput));
}

void Prototype::openLinkUrl(const std::string& url) {
    if (!linkVerifyMode.empty()) {
        openedLinkForTest = url;
        return;
    }
    g_app_info_launch_default_for_uri_async(
            url.c_str(), nullptr, nullptr,
            +[](GObject*, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                GError* error = nullptr;
                if (!g_app_info_launch_default_for_uri_finish(result, &error)) {
                    const std::string message = error ? error->message : "No application can open this link.";
                    self->showAlert("Unable to open link", message);
                }
                g_clear_error(&error);
            },
            this);
}

void Prototype::showLinkInfo(size_t index) {
    if (!linkInfoPopover || index >= state.texts.size() || !state.texts[index].isLink) {
        return;
    }
    linkInfoIndex = index;
    const auto& item = state.texts[index];
    const auto message = item.linkUrl + "\nCtrl-click to open · Double-click to edit";
    gtk_label_set_text(GTK_LABEL(linkInfoLabel), message.c_str());

    std::array<prototype::Point, 4> corners{
            {{0, 0}, {item.layoutWidth, 0}, {item.layoutWidth, item.layoutHeight}, {0, item.layoutHeight}}};
    for (auto& corner: corners) {
        const auto local =
                prototype::Point{item.xx * corner.x + item.xy * corner.y, item.yx * corner.x + item.yy * corner.y};
        corner = state.pageScreenPoint(item.page, local.x + item.x, local.y + item.y);
    }
    double left = corners[0].x, right = corners[0].x, top = corners[0].y, bottom = corners[0].y;
    for (const auto& corner: corners) {
        left = std::min(left, corner.x);
        right = std::max(right, corner.x);
        top = std::min(top, corner.y);
        bottom = std::max(bottom, corner.y);
    }
    GdkRectangle pointing{static_cast<int>(std::floor(left)), static_cast<int>(std::floor(top)),
                          std::max(1, static_cast<int>(std::ceil(right - left))),
                          std::max(1, static_cast<int>(std::ceil(bottom - top)))};
    gtk_popover_set_pointing_to(GTK_POPOVER(linkInfoPopover), &pointing);
    if (!gtk_widget_get_visible(linkInfoPopover)) {
        gtk_popover_popup(GTK_POPOVER(linkInfoPopover));
    }
}

void Prototype::updateLinkHover(double x, double y) {
    if (state.brush.tool != Tool::Link || !canvas) {
        return;
    }
    const auto hit = state.hitPage(x, y);
    const auto link = hit ? state.pickLink(hit->page, hit->local) : std::nullopt;
    gtk_widget_set_cursor_from_name(canvas, link ? "pointer" : nullptr);
    if (link) {
        if (linkInfoIndex != link) {
            showLinkInfo(*link);
        }
    } else {
        linkInfoIndex.reset();
        if (linkInfoPopover) {
            gtk_popover_popdown(GTK_POPOVER(linkInfoPopover));
        }
    }
}

void Prototype::handleLinkClick(double x, double y, int presses, bool controlDown) {
    const auto hit = state.hitPage(x, y);
    if (!hit) {
        return;
    }
    state.selectActivePage(hit->page);
    if (const auto index = state.pickLink(hit->page, hit->local)) {
        if (controlDown) {
            openLinkUrl(state.texts[*index].linkUrl);
        } else if (presses >= 2) {
            openLinkEditor(hit->page, {state.texts[*index].x, state.texts[*index].y}, index);
        } else {
            showLinkInfo(*index);
        }
    } else {
        openLinkEditor(hit->page, hit->local, std::nullopt);
    }
}

}  // namespace prototype::gtk4
