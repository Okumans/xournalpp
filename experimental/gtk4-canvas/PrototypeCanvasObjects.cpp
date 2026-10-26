#include "Prototype.h"

namespace prototype::gtk4 {

prototype::CanvasImage Prototype::displayedImage(size_t index) const {
    if (imageGesture && imageGesture->index == index) {
        return imageGesture->preview;
    }
    return state.displayImage(index);
}

std::optional<size_t> Prototype::imageHandleAt(const prototype::CanvasImage& image,
                                               const prototype::Point& point) const {
    const auto corners = prototype::canvasImageCorners(image);
    const double radius = 8.0 / std::max(0.1, state.zoom);
    for (size_t index = 0; index < corners.size(); ++index) {
        if (std::hypot(corners[index].x - point.x, corners[index].y - point.y) <= radius) {
            return index;
        }
    }
    const auto rotationHandle = prototype::canvasImageRotationHandle(image, state.zoom);
    if (std::hypot(rotationHandle.x - point.x, rotationHandle.y - point.y) <= radius) {
        return corners.size();
    }
    return std::nullopt;
}

bool Prototype::beginImageGesture(Device device, double x, double y) {
    if (state.hasSelection() || state.brush.tool != Tool::SmartSelect || input.spaceHeld || input.shiftHeld ||
        input.panning() || (device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
        return false;
    }
    const auto hit = state.hitPage(x, y);
    if (!hit) {
        return false;
    }

    std::optional<size_t> picked = state.pickImage(hit->page, hit->local);
    std::optional<size_t> handle;
    if (selectedImageIndex && *selectedImageIndex < state.images.size()) {
        const auto& selectedImage = state.images[*selectedImageIndex];
        if (selectedImage.page == hit->page && selectedImage.layerId == state.activeLayerId(hit->page) &&
            state.layerVisible(hit->page, selectedImage.layerId)) {
            handle = imageHandleAt(selectedImage, hit->local);
            if (handle) {
                picked = selectedImageIndex;
            }
        }
    }
    if (!picked) {
        selectedImageIndex.reset();
        return false;
    }

    if (!handle) {
        const auto pickedStroke = state.pickStroke(hit->page, hit->local);
        const auto pickedText = state.pickText(hit->page, hit->local);
        const auto imageOrder = state.images[*picked].layerOrder;
        if ((pickedStroke && state.ink[*pickedStroke].layerOrder > imageOrder) ||
            (pickedText && state.texts[*pickedText].layerOrder > imageOrder)) {
            return false;
        }
    }

    const auto& image = state.images[*picked];
    if (image.page != hit->page || image.layerId != state.activeLayerId(hit->page) ||
        !state.layerVisible(hit->page, image.layerId)) {
        return false;
    }
    if (*picked != selectedImageIndex.value_or(state.images.size())) {
        state.clearSelection();
    }
    state.selectActivePage(hit->page);
    selectedImageIndex = *picked;
    ImageGesture gesture;
    gesture.owner = device;
    gesture.index = *picked;
    gesture.mode = !handle      ? ImageGesture::Mode::Move :
                   *handle == 4 ? ImageGesture::Mode::Rotate :
                                  ImageGesture::Mode::Resize;
    gesture.corner = handle.value_or(0);
    gesture.start = hit->local;
    gesture.center = prototype::canvasImageCenter(image);
    gesture.startAngle = std::atan2(hit->local.y - gesture.center.y, hit->local.x - gesture.center.x);
    gesture.original = image;
    gesture.preview = image;
    imageGesture = std::move(gesture);
    return true;
}

void Prototype::updateImageGesture(double x, double y) {
    if (!imageGesture) {
        return;
    }
    auto& gesture = *imageGesture;
    const auto point = state.pageLocalPoint(gesture.original.page, x, y);
    if (gesture.mode == ImageGesture::Mode::Move) {
        gesture.preview.transform.x = gesture.original.transform.x + point.x - gesture.start.x;
        gesture.preview.transform.y = gesture.original.transform.y + point.y - gesture.start.y;
    } else if (gesture.mode == ImageGesture::Mode::Rotate) {
        const double currentAngle = std::atan2(point.y - gesture.center.y, point.x - gesture.center.x);
        const double angle = std::remainder(currentAngle - gesture.startAngle, 2 * G_PI);
        gesture.rotationDelta = angle;
        prototype::rotateCanvasImage(gesture.preview, angle);
    } else {
        constexpr std::array<prototype::Point, 4> localCorners = {prototype::Point{0, 0}, prototype::Point{1, 0},
                                                                  prototype::Point{1, 1}, prototype::Point{0, 1}};
        const auto corners = prototype::canvasImageCorners(gesture.original);
        const size_t opposite = (gesture.corner + 2) % corners.size();
        const auto anchor = corners[opposite];
        const double diagonalX = corners[gesture.corner].x - anchor.x;
        const double diagonalY = corners[gesture.corner].y - anchor.y;
        const double denominator = diagonalX * diagonalX + diagonalY * diagonalY;
        if (denominator > 1e-8) {
            const double factor = std::clamp(
                    ((point.x - anchor.x) * diagonalX + (point.y - anchor.y) * diagonalY) / denominator, 0.05, 10.0);
            const auto& source = gesture.original.transform;
            auto& target = gesture.preview.transform;
            target.xx = source.xx * factor;
            target.yx = source.yx * factor;
            target.xy = source.xy * factor;
            target.yy = source.yy * factor;
            const double anchorX = localCorners[opposite].x * prototype::canvasImageWidth(gesture.original);
            const double anchorY = localCorners[opposite].y * prototype::canvasImageHeight(gesture.original);
            target.x = anchor.x - target.xx * anchorX - target.xy * anchorY;
            target.y = anchor.y - target.yx * anchorX - target.yy * anchorY;
        }
    }
    const auto& before = gesture.original.transform;
    const auto& after = gesture.preview.transform;
    gesture.changed = before.xx != after.xx || before.yx != after.yx || before.xy != after.xy ||
                      before.yy != after.yy || before.x != after.x || before.y != after.y;
}

void Prototype::finishImageGesture() {
    if (!imageGesture) {
        return;
    }
    auto gesture = std::move(*imageGesture);
    imageGesture.reset();
    if (gesture.mode == ImageGesture::Mode::Rotate) {
        gesture.preview = gesture.original;
        const double angle = prototype::canvasImageRotationDelta(
                gesture.startAngle, gesture.startAngle + gesture.rotationDelta,
                state.snapRotationEnabled != input.altHeld, state.snapRotationTolerance);
        prototype::rotateCanvasImage(gesture.preview, angle);
        const auto& before = gesture.original.transform;
        const auto& after = gesture.preview.transform;
        gesture.changed = before.xx != after.xx || before.yx != after.yx || before.xy != after.xy ||
                          before.yy != after.yy || before.x != after.x || before.y != after.y;
    }
    if (gesture.changed && state.replaceImage(gesture.index, std::move(gesture.preview))) {
        selectedImageIndex = gesture.index;
    }
    update();
}

LatexTarget Prototype::latexTargetAt(int page, const prototype::Point& point, bool includeText) const {
    LatexTarget target;
    const auto activeLayer = state.activeLayerId(page);
    if (!state.layerVisible(page, activeLayer)) {
        return target;
    }

    size_t bestOrder = 0;
    bool found = false;
    const auto contains = [&point](double xx, double yx, double xy, double yy, double x, double y, double width,
                                   double height) {
        const double determinant = xx * yy - xy * yx;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-10) {
            return false;
        }
        const double dx = point.x - x;
        const double dy = point.y - y;
        const double localX = (yy * dx - xy * dy) / determinant;
        const double localY = (-yx * dx + xx * dy) / determinant;
        return localX >= 0 && localX <= width && localY >= 0 && localY <= height;
    };

    for (size_t index = 0; index < state.images.size(); ++index) {
        const auto& image = state.images[index];
        if (image.page != page || image.layerId != activeLayer || image.kind != prototype::CanvasImageKind::Latex ||
            !prototype::validCanvasImage(image) ||
            !contains(image.transform.xx, image.transform.yx, image.transform.xy, image.transform.yy, image.transform.x,
                      image.transform.y, prototype::canvasImageWidth(image), prototype::canvasImageHeight(image))) {
            continue;
        }
        if (!found || image.layerOrder >= bestOrder) {
            target = {index, std::nullopt};
            bestOrder = image.layerOrder;
            found = true;
        }
    }

    if (includeText) {
        for (size_t index = 0; index < state.texts.size(); ++index) {
            const auto& item = state.texts[index];
            if (item.page != page || item.layerId != activeLayer || item.text.empty() ||
                !contains(item.xx, item.yx, item.xy, item.yy, item.x, item.y, item.layoutWidth, item.layoutHeight)) {
                continue;
            }
            if (!found || item.layerOrder >= bestOrder) {
                target = {std::nullopt, index};
                bestOrder = item.layerOrder;
                found = true;
            }
        }
    }
    return target;
}

prototype::Point Prototype::textTopLeft(const prototype::TextItem& item) const {
    double width = item.layoutWidth;
    double height = item.layoutHeight;
    if (width <= 0 || height <= 0) {
        const auto natural = prototype::textNaturalSize(item);
        width = std::max(width, natural.first);
        height = std::max(height, natural.second);
    }
    const std::array<prototype::Point, 4> corners = {
            prototype::Point{item.x, item.y},
            prototype::Point{item.x + item.xx * width, item.y + item.yx * width},
            prototype::Point{item.x + item.xy * height, item.y + item.yy * height},
            prototype::Point{item.x + item.xx * width + item.xy * height, item.y + item.yx * width + item.yy * height},
    };
    prototype::Point topLeft{corners[0].x, corners[0].y};
    for (const auto& corner: corners) {
        topLeft.x = std::min(topLeft.x, corner.x);
        topLeft.y = std::min(topLeft.y, corner.y);
    }
    return topLeft;
}

void Prototype::openLatexEditorAt(double x, double y, bool existingLatexOnly) {
    if (!nativeSession || !window || busy || choosingFile || messageDialog || latexEditor) {
        return;
    }

    finishTextEditing();
    const auto hit = state.hitPage(x, y);
    if (!hit) {
        return;
    }
    const auto target = latexTargetAt(hit->page, hit->local, !existingLatexOnly);
    if (existingLatexOnly && !target.imageIndex) {
        return;
    }

    const int page = hit->page;
    auto position = hit->local;
    std::string source = importedConfig.settings.latexDefaultText;
    std::optional<prototype::CanvasImage> originalImage;
    std::optional<prototype::TextItem> originalText;
    if (target.imageIndex) {
        originalImage = state.images[*target.imageIndex];
        source = originalImage->latexSource;
    } else if (target.textIndex) {
        originalText = state.texts[*target.textIndex];
        source = "\\text{" + originalText->text + "}";
        position = textTopLeft(*originalText);
    }

    resetInput(true);
    state.selectActivePage(page);
    const auto documentSession = nativeSession;
    const auto textIndex = target.textIndex;
    const auto imageIndex = target.imageIndex;
    const auto insertedAt = position;
    const bool selectInitialSource = !originalImage && !originalText;
    const auto& latexSettings = importedConfig.settings;
    prototype::LatexEditorPreferences editorPreferences;
    editorPreferences.sourceViewThemeId = latexSettings.latexSourceViewThemeId;
    editorPreferences.sourceViewAutoIndent = latexSettings.latexSourceViewAutoIndent;
    editorPreferences.sourceViewSyntaxHighlight = latexSettings.latexSourceViewSyntaxHighlight;
    editorPreferences.sourceViewShowLineNumbers = latexSettings.latexSourceViewShowLineNumbers;
    editorPreferences.editorFont = latexSettings.latexEditorFont;
    editorPreferences.useCustomEditorFont = latexSettings.latexUseCustomEditorFont;
    editorPreferences.editorWordWrap = latexSettings.latexEditorWordWrap;
    editorPreferences.useExternalEditor = latexSettings.latexUseExternalEditor;
    editorPreferences.externalEditorAutoConfirm = latexSettings.latexExternalEditorAutoConfirm;
    editorPreferences.externalEditorCommand = latexSettings.latexExternalEditorCommand;
    editorPreferences.temporaryFileExtension = latexSettings.latexTemporaryFileExtension;
    latexEditor = prototype::LatexEditor::create(
            GTK_WINDOW(window), latexTemplatePath, importedConfig.settings.latexGeneratorCommand, std::move(source),
            state.brush.color, originalImage, selectInitialSource, std::move(editorPreferences),
            [this, documentSession, page, textIndex, imageIndex, originalImage, originalText,
             insertedAt](std::optional<prototype::CanvasImage> result) {
                latexEditor.reset();
                if (!result) {
                    update();
                    return;
                }
                if (nativeSession != documentSession || !nativeSession || page < 0 || page >= state.pageCount()) {
                    return;
                }

                result->page = page;
                size_t insertedIndex = state.images.size();
                bool changed = false;
                if (imageIndex && originalImage) {
                    result->transform = originalImage->transform;
                    result->layerId = originalImage->layerId;
                    result->layerOrder = originalImage->layerOrder;
                    result->originLayer = originalImage->originLayer;
                    changed = state.replaceImage(*imageIndex, std::move(*result));
                    insertedIndex = *imageIndex;
                } else if (textIndex && originalText) {
                    const auto topLeft = textTopLeft(*originalText);
                    result->transform = {1, 0, 0, 1, topLeft.x, topLeft.y};
                    changed = state.replaceTextWithImage(*textIndex, std::move(*result));
                    insertedIndex = state.images.empty() ? 0 : state.images.size() - 1;
                } else {
                    result->transform = {1, 0, 0, 1, insertedAt.x, insertedAt.y};
                    changed = state.insertImage(std::move(*result));
                    insertedIndex = state.images.empty() ? 0 : state.images.size() - 1;
                }

                if (!changed) {
                    showError("The LaTeX formula could not be added to the active page.");
                    update();
                    return;
                }
                activateSelection(Tool::SmartSelect);
                selectedImageIndex = insertedIndex;
                update();
            });
    latexEditor->present();
}

void Prototype::insertImageFromPath(const std::string& path) {
    if (!nativeSession) {
        return;
    }
    try {
        auto image = prototype::readCanvasImageFile(path);
        const auto page = state.pageSize(state.page);
        image.page = state.page;
        const double scale = std::min(page.width * 0.6 / image.pixelWidth, page.height * 0.6 / image.pixelHeight);
        const double width = image.pixelWidth * scale;
        const double height = image.pixelHeight * scale;
        image.transform = {scale, 0, 0, scale, (page.width - width) / 2, (page.height - height) / 2};
        selectedImageIndex.reset();
        selectTool(Tool::SmartSelect);
        if (!state.insertImage(std::move(image))) {
            showError("The image could not be inserted into the active page.");
            return;
        }
        update();
    } catch (const std::exception& error) {
        showError(std::string("Cannot insert image: ") + error.what());
    }
}

void Prototype::chooseImage() {
    if (!nativeSession || busy || choosingFile || messageDialog) {
        return;
    }
    if (autosaving) {
        waitForAutosave([this] { chooseImage(); });
        return;
    }
    finishTextEditing();
    resetInput(true);
    choosingFile = true;
    auto* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Insert Image");
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    for (const char* mime: {"image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp", "image/tiff"}) {
        gtk_file_filter_add_mime_type(filter, mime);
    }
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter);
    gtk_file_dialog_open(
            dialog, GTK_WINDOW(window), nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                self->choosingFile = false;
                GError* error = nullptr;
                auto* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
                self->finishFileChoice(file, error,
                                       [self](const std::string& path) { self->insertImageFromPath(path); });
            },
            this);
    g_object_unref(dialog);
}

}  // namespace prototype::gtk4
