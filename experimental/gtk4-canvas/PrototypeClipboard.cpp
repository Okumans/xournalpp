#include "Prototype.h"

namespace prototype::gtk4 {

guint Prototype::selectionModeIndex(Tool tool) const {
    if (tool == Tool::VerticalSpace) {
        return 8;
    }
    if (state.selectionAcrossLayers && tool == Tool::SelectRectangle) {
        return 3;
    }
    if (state.selectionAcrossLayers && tool == Tool::SelectLasso) {
        return 4;
    }
    switch (tool) {
        case Tool::SelectRectangle:
            return 0;
        case Tool::SelectLasso:
            return 1;
        case Tool::SmartSelect:
            return 2;
        case Tool::SelectObject:
            return 5;
        case Tool::SelectPdfTextLinear:
            return 6;
        case Tool::SelectPdfTextRect:
            return 7;
        default:
            return 0;
    }
}

void Prototype::activateSelection(Tool tool, bool acrossLayers) {
    selectionTool = tool;
    state.selectionAcrossLayers = acrossLayers;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
    selectTool(tool);
}

void Prototype::copyImagesToClipboard(const std::vector<prototype::CanvasImage>& images) {
    if (images.empty()) {
        throw std::runtime_error("No images are selected");
    }
    auto bounds = prototype::canvasImageBounds(images.front());
    std::vector<prototype::EditableObject> objects;
    objects.reserve(images.size());
    for (const auto& image: images) {
        if (image.page != bounds.page || !prototype::validCanvasImage(image)) {
            throw std::runtime_error("The selected images cannot be copied together");
        }
        for (const auto& point: prototype::canvasImageCorners(image)) {
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
        objects.emplace_back(image);
    }
    copyObjectsToClipboard(objects, {bounds.page, bounds.x0, bounds.y0, bounds.x1, bounds.y1});
}

void Prototype::copyObjectsToClipboard(const std::vector<prototype::EditableObject>& objects,
                                       const prototype::SelectionBox& bounds) {
    const auto native = prototype::clipboard::encodeObjects(objects, bounds);
    const auto rendered = prototype::clipboard::renderSelection(objects, bounds);
    std::vector<GdkContentProvider*> providers;
    const auto addBytes = [&](const char* mimeType, const std::string& value) {
        auto* bytes = g_bytes_new(value.data(), value.size());
        providers.push_back(gdk_content_provider_new_for_bytes(mimeType, bytes));
        g_bytes_unref(bytes);
    };
    addBytes(prototype::clipboard::mime, native);
    addBytes("image/png", rendered.png);
    addBytes("image/svg", rendered.svg);
    addBytes("image/svg+xml", rendered.svg);
    if (!rendered.plainText.empty()) {
        addBytes("text/plain;charset=utf-8", rendered.plainText);
    }
    auto* provider = gdk_content_provider_new_union(providers.data(), providers.size());
    const bool copied = gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas), provider);
    g_object_unref(provider);
    if (!copied) {
        throw std::runtime_error("Cannot claim the clipboard");
    }
}

void Prototype::copySelection(bool cut) {
    if (busy)
        return;
    resetInput(true);
    try {
        if (selectedImageIndex && *selectedImageIndex < state.images.size()) {
            copyImagesToClipboard({state.images[*selectedImageIndex]});
            if (cut) {
                state.deleteImage(*selectedImageIndex);
                selectedImageIndex.reset();
            }
            update();
            return;
        }
        if (!state.hasSelection())
            return;
        const auto bounds = state.selectionBox();
        if (!bounds) {
            throw std::runtime_error("The selection has no copyable bounds");
        }
        copyObjectsToClipboard(state.copySelectedObjects(), *bounds);
        if (cut)
            state.deleteSelection();
        update();
    } catch (const std::exception& error) {
        showError(error.what());
    }
}

void Prototype::duplicateSelection() {
    if (busy)
        return;
    resetInput(true);
    if (selectedImageIndex && *selectedImageIndex < state.images.size()) {
        const auto duplicate = state.images[*selectedImageIndex];
        const auto bounds = prototype::canvasImageBounds(duplicate);
        selectedImageIndex.reset();
        if (!state.pasteImages({duplicate}, duplicate.page,
                               {(bounds.x0 / 2.0 + bounds.x1 / 2.0) + 20, (bounds.y0 / 2.0 + bounds.y1 / 2.0) + 20})) {
            showError("Cannot duplicate the selected image");
        }
        update();
        return;
    }
    if (!state.hasSelection())
        return;
    const auto bounds = state.selectionBox();
    if (!bounds ||
        !state.pasteObjects(state.copySelectedObjects(), bounds->page,
                            {(bounds->x0 / 2.0 + bounds->x1 / 2.0) + 20, (bounds->y0 / 2.0 + bounds->y1 / 2.0) + 20})) {
        showError("Cannot duplicate the selected objects");
        return;
    }
    activateSelection(Tool::SmartSelect);
    update();
}

void Prototype::finishClipboardRead(const std::string& error) {
    if (clipboardTimer)
        g_source_remove(clipboardTimer);
    clipboardTimer = 0;
    g_clear_object(&clipboardCancellation);
    busy = false;
    gtk_widget_set_sensitive(contentView, true);
    if (!error.empty())
        showError(error);
    update();
    g_application_release(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
}

bool Prototype::placePastedImages(std::vector<prototype::CanvasImage> images) {
    if (images.empty()) {
        return false;
    }
    const auto pageSize = state.pageSize(state.page);
    auto center =
            state.pageLocalPoint(state.page, gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0);
    center.x = std::clamp(center.x, 0.0, pageSize.width);
    center.y = std::clamp(center.y, 0.0, pageSize.height);

    selectedImageIndex.reset();
    if (!state.pasteImages(std::move(images), state.page, center)) {
        return false;
    }
    activateSelection(Tool::SmartSelect);
    return true;
}

bool Prototype::placePastedImage(prototype::CanvasImage image) { return placePastedImages({std::move(image)}); }

bool Prototype::placePastedTexts(std::vector<prototype::TextItem> texts) {
    if (texts.empty()) {
        return false;
    }
    const auto page = state.pageSize(state.page);
    auto center =
            state.pageLocalPoint(state.page, gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0);
    center.x = std::clamp(center.x, 0.0, page.width);
    center.y = std::clamp(center.y, 0.0, page.height);
    if (!state.pasteTexts(std::move(texts), state.page, center)) {
        return false;
    }
    activateSelection(Tool::SmartSelect);
    return true;
}

bool Prototype::placePastedObjects(std::vector<prototype::EditableObject> objects) {
    if (objects.empty()) {
        return false;
    }
    const auto page = state.pageSize(state.page);
    auto center =
            state.pageLocalPoint(state.page, gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0);
    center.x = std::clamp(center.x, 0.0, page.width);
    center.y = std::clamp(center.y, 0.0, page.height);
    selectedImageIndex.reset();
    if (!state.pasteObjects(std::move(objects), state.page, center)) {
        return false;
    }
    activateSelection(Tool::SmartSelect);
    return true;
}

void Prototype::pasteExternalText() {
    gdk_clipboard_read_text_async(
            gtk_widget_get_clipboard(canvas), clipboardCancellation,
            +[](GObject* object, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                GError* error = nullptr;
                char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(object), result, &error);
                if (!text) {
                    g_clear_error(&error);
                    self->pasteExternalImage();
                    return;
                }
                std::string failure;
                const size_t length = std::strlen(text);
                if (length == 0 || length > prototype::bridge::maxEditableTextBytes ||
                    !g_utf8_validate(text, static_cast<gssize>(length), nullptr)) {
                    failure = "Clipboard text is empty or exceeds the 4 MiB UTF-8 limit";
                } else {
                    prototype::TextItem item;
                    item.page = self->state.page;
                    item.text.assign(text, length);
                    item.fontName = self->state.textFontName;
                    item.fontSize = self->state.textFontSize;
                    item.alignment = self->state.textAlignment;
                    item.justify = self->state.textJustify;
                    item.color = 0xff000000U | (uint32_t(std::lround(self->state.textColor[0] * 255)) << 16U) |
                                 (uint32_t(std::lround(self->state.textColor[1] * 255)) << 8U) |
                                 uint32_t(std::lround(self->state.textColor[2] * 255));
                    const auto [width, height] = prototype::textNaturalSize(item);
                    item.layoutWidth = width;
                    item.layoutHeight = height;
                    const auto size = self->state.pageSize(self->state.page);
                    auto center = self->state.pageLocalPoint(self->state.page, gtk_widget_get_width(self->canvas) / 2.0,
                                                             gtk_widget_get_height(self->canvas) / 2.0);
                    center.x = std::clamp(center.x, 0.0, size.width);
                    center.y = std::clamp(center.y, 0.0, size.height);
                    if (!self->state.pasteTexts({std::move(item)}, self->state.page, center)) {
                        failure = "Cannot place clipboard text";
                    } else {
                        self->activateSelection(Tool::SmartSelect);
                    }
                }
                g_free(text);
                if (!failure.empty()) {
                    self->finishClipboardRead(failure);
                } else {
                    self->finishClipboardRead({});
                }
            },
            this);
}

void Prototype::pasteExternalImage() {
    gdk_clipboard_read_texture_async(
            gtk_widget_get_clipboard(canvas), clipboardCancellation,
            +[](GObject* object, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                GError* error = nullptr;
                auto* texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(object), result, &error);
                if (!texture) {
                    const std::string message = error ? error->message : "Clipboard has no image";
                    g_clear_error(&error);
                    self->finishClipboardRead(message);
                    return;
                }

                const int width = gdk_texture_get_width(texture);
                const int height = gdk_texture_get_height(texture);
                if (width <= 0 || height <= 0 ||
                    static_cast<size_t>(width) > prototype::maxCanvasImagePixels / static_cast<size_t>(height)) {
                    g_object_unref(texture);
                    self->finishClipboardRead("Clipboard image exceeds the 32 megapixel limit");
                    return;
                }
                GBytes* png = gdk_texture_save_to_png_bytes(texture);
                g_object_unref(texture);
                if (!png) {
                    self->finishClipboardRead("Cannot encode the clipboard image");
                    return;
                }
                gsize length = 0;
                const auto* bytes = static_cast<const char*>(g_bytes_get_data(png, &length));
                if (length == 0 || length > prototype::maxCanvasImageBytes) {
                    g_bytes_unref(png);
                    self->finishClipboardRead("Clipboard image exceeds the 64 MiB limit");
                    return;
                }
                auto image = std::make_shared<prototype::CanvasImage>();
                image->encodedData.assign(bytes, length);
                g_bytes_unref(png);
                auto* cancellation = self->clipboardCancellation;
                prototype::bridge::documentJob(
                        [image, width, height, cancellation] {
                            if (g_cancellable_is_cancelled(cancellation)) {
                                throw std::runtime_error("Clipboard read cancelled");
                            }
                            int decodedWidth = 0;
                            int decodedHeight = 0;
                            image->surface =
                                    prototype::decodeCanvasImage(image->encodedData, decodedWidth, decodedHeight);
                            if (decodedWidth != width || decodedHeight != height) {
                                throw std::runtime_error("Clipboard image dimensions changed while decoding");
                            }
                            image->pixelWidth = decodedWidth;
                            image->pixelHeight = decodedHeight;
                        },
                        [self, image](const std::string& decodeError) {
                            std::string failure = decodeError;
                            if (failure.empty() && !self->placePastedImage(std::move(*image))) {
                                failure = "Cannot place clipboard image";
                            }
                            self->finishClipboardRead(failure);
                        });
            },
            this);
}

void Prototype::pasteSelection() {
    if (busy || choosingFile || messageDialog)
        return;
    resetInput(true);
    // Freeze document actions through completion, so a delayed clipboard
    // cannot insert into a newly opened document. Bound both time and size.
    busy = true;
    busyText = "Reading clipboard…";
    gtk_widget_set_sensitive(contentView, false);
    updateTitle();
    g_application_hold(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
    clipboardCancellation = g_cancellable_new();
    clipboardTimer = g_timeout_add_seconds(
            5,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                self->clipboardTimer = 0;
                g_cancellable_cancel(self->clipboardCancellation);
                return G_SOURCE_REMOVE;
            },
            this);
    const char* types[] = {prototype::clipboard::mime, nullptr};
    gdk_clipboard_read_async(
            gtk_widget_get_clipboard(canvas), types, G_PRIORITY_DEFAULT, clipboardCancellation,
            +[](GObject* object, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                GError* error = nullptr;
                auto* rawStream = gdk_clipboard_read_finish(GDK_CLIPBOARD(object), result, nullptr, &error);
                if (!rawStream) {
                    g_clear_error(&error);
                    self->pasteExternalText();
                    return;
                }
                auto stream =
                        std::shared_ptr<GInputStream>(rawStream, [](GInputStream* stream) { g_object_unref(stream); });
                auto decoded = std::make_shared<prototype::clipboard::DecodedPayload>();
                auto* cancellation = self->clipboardCancellation;
                prototype::bridge::documentJob(
                        [stream, decoded, cancellation] {
                            std::string bytes;
                            char buffer[16384];
                            while (true) {
                                GError* error = nullptr;
                                const auto read =
                                        g_input_stream_read(stream.get(), buffer, sizeof(buffer), cancellation, &error);
                                if (read < 0) {
                                    const std::string message = error ? error->message : "Cannot read clipboard";
                                    g_clear_error(&error);
                                    throw std::runtime_error(message);
                                }
                                if (!read)
                                    break;
                                if (size_t(read) > prototype::clipboard::maxBytes - bytes.size())
                                    throw std::runtime_error("Clipboard exceeds the 64 MiB limit");
                                bytes.append(buffer, size_t(read));
                            }
                            *decoded = prototype::clipboard::decodePayload(bytes);
                            for (auto& object: decoded->objects) {
                                auto* image = std::get_if<prototype::CanvasImage>(&object);
                                if (!image) {
                                    continue;
                                }
                                if (image->kind == prototype::CanvasImageKind::Latex) {
                                    *image = prototype::hydrateCanvasImage(std::move(*image));
                                    continue;
                                }
                                const int expectedWidth = image->pixelWidth;
                                const int expectedHeight = image->pixelHeight;
                                int width = 0;
                                int height = 0;
                                image->surface = prototype::decodeCanvasImage(image->encodedData, width, height);
                                const double scaleX = static_cast<double>(expectedWidth) / width;
                                const double scaleY = static_cast<double>(expectedHeight) / height;
                                image->transform.xx *= scaleX;
                                image->transform.yx *= scaleX;
                                image->transform.xy *= scaleY;
                                image->transform.yy *= scaleY;
                                image->pixelWidth = width;
                                image->pixelHeight = height;
                            }
                        },
                        [self, decoded](const std::string& error) {
                            std::string failure = error;
                            if (failure.empty()) {
                                if (!self->placePastedObjects(std::move(decoded->objects))) {
                                    failure = "Cannot place clipboard objects";
                                }
                            }
                            self->finishClipboardRead(failure);
                        });
            },
            this);
}

}  // namespace prototype::gtk4
