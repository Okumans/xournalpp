#include "Prototype.h"

namespace prototype::gtk4 {

bool Prototype::transformSelectedImagesFromPlugin(std::int64_t action, std::string& error) {
    if (action < static_cast<int>(prototype::CanvasImageAction::Invert) ||
        action > static_cast<int>(prototype::CanvasImageAction::SplitHorizontal)) {
        error = "Unknown ImageActions mode";
        return false;
    }
    if (!prototype::canvasImageActionsAvailable()) {
        error = "ImageActions requires libvips support in this GTK 4 build";
        return false;
    }
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before transforming images";
        return false;
    }
    if (state.selectedImages.empty()) {
        return true;
    }

    std::vector<std::pair<size_t, std::vector<prototype::CanvasImage>>> replacements;
    replacements.reserve(state.selectedImages.size());
    const auto imageAction = static_cast<prototype::CanvasImageAction>(action);
    for (const auto index: state.selectedImages) {
        if (index >= state.images.size()) {
            error = "The selected image no longer exists";
            return false;
        }
        const auto& image = state.images[index];
        if (image.page < 0 || image.page >= state.pageCount()) {
            error = "The selected image belongs to an invalid page";
            return false;
        }
        const auto page = state.pageSize(image.page);
        std::vector<prototype::CanvasImage> outputs;
        if (!prototype::transformCanvasImage(image, imageAction, page.width, page.height, outputs, error)) {
            return false;
        }
        replacements.emplace_back(index, std::move(outputs));
    }
    if (!state.replaceImageGroup(std::move(replacements))) {
        error = "The selected images could not be replaced as one undoable action";
        return false;
    }
    update();
    return true;
}

template <typename Object, typename Callback>
bool Prototype::forEachPluginObject(std::string_view type, const std::vector<Object>& objects,
                                    const std::vector<size_t>& selectedIndices, Callback&& callback,
                                    std::string& error) {
    if (type != "selection" && type != "layer" && type != "page" && type != "all") {
        error = "Unknown argument (" + std::string(type) + ") for getting selection";
        return false;
    }
    if (type == "selection" && !state.hasSelection()) {
        error = "There is no selection";
        return false;
    }

    if (type != "selection" && state.hasSelection()) {
        state.clearSelection();
        update();
    }

    state.ensurePluginReferences();
    const auto appendLayer = [this, &objects, &callback](int pageIndex, size_t layerId, bool includePage,
                                                         bool includeLayer) {
        std::vector<size_t> indices;
        for (size_t index = 0; index < objects.size(); ++index) {
            const auto& object = objects[index];
            if (object.page == pageIndex && object.layerId == layerId) {
                indices.push_back(index);
            }
        }
        std::stable_sort(indices.begin(), indices.end(), [this, &objects](size_t lhs, size_t rhs) {
            if (objects[lhs].layerOrder != objects[rhs].layerOrder) {
                return objects[lhs].layerOrder < objects[rhs].layerOrder;
            }
            return lhs < rhs;
        });
        for (const size_t index: indices) {
            callback(index, includePage ? std::optional<size_t>(static_cast<size_t>(pageIndex + 1)) : std::nullopt,
                     includeLayer ? std::optional<size_t>(layerId) : std::nullopt);
        }
    };

    if (type == "selection") {
        for (const size_t index: selectedIndices) {
            if (index < objects.size()) {
                callback(index, std::nullopt, std::nullopt);
            }
        }
        return true;
    }
    if (type == "layer") {
        appendLayer(state.page, state.activeLayerId(state.page), false, false);
        return true;
    }
    const int firstPage = type == "all" ? 0 : state.page;
    const int lastPage = type == "all" ? state.pageCount() : state.page + 1;
    for (int pageIndex = firstPage; pageIndex < lastPage; ++pageIndex) {
        const auto& layers = state.layersForPage(pageIndex).layers;
        for (size_t layerId = 1; layerId <= layers.size(); ++layerId) {
            appendLayer(pageIndex, layerId, type == "all", true);
        }
    }
    return true;
}

bool Prototype::getPluginStrokes(std::string_view type, std::vector<prototype::PluginStroke>& strokes,
                                 std::string& error) {
    return forEachPluginObject(
            type, state.ink, state.selected,
            [&](size_t index, auto page, auto layer) {
                if (index >= state.ink.size()) {
                    return;
                }
                const auto& ink = state.ink[index];
                prototype::PluginStroke stroke;
                stroke.x.reserve(ink.points.size());
                stroke.y.reserve(ink.points.size());
                if (ink.pressureSensitive) {
                    stroke.pressure.reserve(ink.points.size());
                }
                for (const auto& point: ink.points) {
                    stroke.x.push_back(point.x);
                    stroke.y.push_back(point.y);
                    if (ink.pressureSensitive) {
                        stroke.pressure.push_back(point.pressure);
                    }
                }
                switch (ink.brush.tool) {
                    case Tool::Highlighter:
                    case Tool::LaserPointerHighlighter:
                        stroke.tool = "highlighter";
                        break;
                    case Tool::Eraser:
                        stroke.tool = "eraser";
                        break;
                    default:
                        stroke.tool = "pen";
                        break;
                }
                stroke.width = ink.brush.width;
                const auto channel = [](double value) {
                    return static_cast<std::uint32_t>(std::clamp(std::lround(value * 255.0), 0L, 255L));
                };
                stroke.color = (channel(ink.brush.color[0]) << 16U) | (channel(ink.brush.color[1]) << 8U) |
                               channel(ink.brush.color[2]);
                stroke.fill = ink.fill;
                stroke.lineStyle = prototype::formatLineStyle(ink.brush.lineDashes);
                stroke.reference = ink.pluginReference;
                stroke.page = page;
                stroke.layer = layer;
                strokes.push_back(std::move(stroke));
            },
            error);
}

bool Prototype::getPluginTexts(std::string_view type, std::vector<prototype::PluginText>& texts, std::string& error) {
    return forEachPluginObject(
            type, state.texts, state.selectedTexts,
            [&](size_t index, auto page, auto layer) {
                if (index >= state.texts.size() || state.texts[index].isLink) {
                    return;
                }
                const auto& item = state.texts[index];
                double x0 = std::numeric_limits<double>::infinity();
                double y0 = std::numeric_limits<double>::infinity();
                double x1 = -std::numeric_limits<double>::infinity();
                double y1 = -std::numeric_limits<double>::infinity();
                for (const prototype::Point point: {
                             prototype::Point{0, 0},
                             prototype::Point{item.layoutWidth, 0},
                             prototype::Point{item.layoutWidth, item.layoutHeight},
                             prototype::Point{0, item.layoutHeight},
                     }) {
                    const double x = item.xx * point.x + item.xy * point.y + item.x;
                    const double y = item.yx * point.x + item.yy * point.y + item.y;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
                const auto channelColor = item.color & 0x00ffffffU;
                texts.push_back({item.text, item.fontName, item.fontSize, channelColor, item.x, item.y,
                                 item.wrapWidth >= 0 ? std::optional<double>(item.wrapWidth) : std::nullopt,
                                 std::max(0.0, x1 - x0), std::max(0.0, y1 - y0), item.pluginReference, page, layer});
            },
            error);
}

bool Prototype::getPluginLinks(std::string_view type, std::vector<prototype::PluginLink>& links, std::string& error) {
    return forEachPluginObject(
            type, state.texts, state.selectedTexts,
            [&](size_t index, auto page, auto layer) {
                if (index >= state.texts.size() || !state.texts[index].isLink) {
                    return;
                }
                const auto& item = state.texts[index];
                double x0 = std::numeric_limits<double>::infinity();
                double y0 = std::numeric_limits<double>::infinity();
                double x1 = -std::numeric_limits<double>::infinity();
                double y1 = -std::numeric_limits<double>::infinity();
                for (const prototype::Point point: {
                             prototype::Point{0, 0},
                             prototype::Point{item.layoutWidth, 0},
                             prototype::Point{item.layoutWidth, item.layoutHeight},
                             prototype::Point{0, item.layoutHeight},
                     }) {
                    const double x = item.xx * point.x + item.xy * point.y + item.x;
                    const double y = item.yx * point.x + item.yy * point.y + item.y;
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
                links.push_back({item.text, item.linkUrl, item.alignment, item.fontName, item.fontSize,
                                 item.color & 0x00ffffffU, item.x, item.y, std::max(0.0, x1 - x0),
                                 std::max(0.0, y1 - y0), item.pluginReference, page, layer});
            },
            error);
}

bool Prototype::getPluginImages(std::string_view type, std::vector<prototype::PluginImage>& images,
                                std::string& error) {
    return forEachPluginObject(
            type, state.images, state.selectedImages,
            [&](size_t index, auto page, auto layer) {
                if (index >= state.images.size()) {
                    return;
                }
                const auto& item = state.images[index];
                const auto bounds = prototype::canvasImageBounds(item);
                std::string format;
                if (item.kind == prototype::CanvasImageKind::Latex) {
                    format = prototype::isCanvasTexPdf(item.encodedData) ? "pdf" : "png";
                } else {
                    auto* loader = gdk_pixbuf_loader_new();
                    GError* loadError = nullptr;
                    const bool wrote =
                            gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(item.encodedData.data()),
                                                    item.encodedData.size(), &loadError);
                    const bool closed = wrote && gdk_pixbuf_loader_close(loader, &loadError);
                    if (auto* pixbufFormat = closed ? gdk_pixbuf_loader_get_format(loader) : nullptr) {
                        gchar* formatName = gdk_pixbuf_format_get_name(pixbufFormat);
                        if (formatName) {
                            format = formatName;
                            g_free(formatName);
                        }
                    }
                    g_clear_error(&loadError);
                    g_object_unref(loader);
                }
                if (format.empty()) {
                    format = "unknown";
                }
                const int naturalWidth = item.kind == prototype::CanvasImageKind::Latex ?
                                                 static_cast<int>(std::lround(item.nativeWidth)) :
                                                 item.pixelWidth;
                const int naturalHeight = item.kind == prototype::CanvasImageKind::Latex ?
                                                  static_cast<int>(std::lround(item.nativeHeight)) :
                                                  item.pixelHeight;
                images.push_back({bounds.x0, bounds.y0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0, item.encodedData,
                                  std::move(format), naturalWidth, naturalHeight, item.pluginReference, page, layer});
            },
            error);
}

bool Prototype::addPluginReferencesToSelection(const std::vector<std::uintptr_t>& references, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before changing the selection";
        return false;
    }
    state.addPluginReferencesToSelection(references);
    update();
    return true;
}

bool Prototype::addPluginStrokes(const std::vector<prototype::PluginStrokeInput>& inputs,
                                 prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                                 std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before adding strokes";
        return false;
    }
    if (inputs.empty()) {
        return true;
    }

    std::vector<prototype::Ink> additions;
    additions.reserve(inputs.size());
    for (const auto& inputStroke: inputs) {
        const bool highlighter = inputStroke.tool && *inputStroke.tool == "highlighter";
        const auto& defaults = highlighter ? highlightBrush : penBrush;
        prototype::Ink stroke{};
        stroke.page = state.page;
        stroke.brush.tool = highlighter ? Tool::Highlighter : Tool::Pen;
        stroke.brush.color = defaults.color;
        stroke.brush.width = defaults.width;
        stroke.brush.lineDashes = defaults.lineDashes;
        if (inputStroke.width) {
            stroke.brush.width = *inputStroke.width;
        }
        if (inputStroke.color) {
            const auto color = *inputStroke.color;
            stroke.brush.color = {static_cast<double>((color >> 16U) & 0xffU) / 255.0,
                                  static_cast<double>((color >> 8U) & 0xffU) / 255.0,
                                  static_cast<double>(color & 0xffU) / 255.0};
        }
        stroke.fill = inputStroke.fill.value_or(-1);
        if (inputStroke.lineStyle) {
            const auto lineDashes = prototype::parseLineStyle(*inputStroke.lineStyle);
            if (!lineDashes) {
                error = "The stroke has an invalid line style";
                return false;
            }
            stroke.brush.lineDashes = *lineDashes;
        }
        stroke.pressureSensitive = !inputStroke.pressure.empty() && !highlighter;
        stroke.points.reserve(inputStroke.x.size());
        for (size_t index = 0; index < inputStroke.x.size(); ++index) {
            const double pressure = inputStroke.pressure.empty() ? 1.0 : inputStroke.pressure[index];
            stroke.points.push_back({inputStroke.x[index], inputStroke.y[index], pressure});
        }
        additions.push_back(std::move(stroke));
    }

    int undoPolicyValue = 0;
    switch (policy) {
        case prototype::PluginStrokeUndoPolicy::Grouped:
            undoPolicyValue = 0;
            break;
        case prototype::PluginStrokeUndoPolicy::Individual:
            undoPolicyValue = 1;
            break;
        case prototype::PluginStrokeUndoPolicy::None:
            undoPolicyValue = 2;
            break;
    }
    const auto first = state.ink.size();
    if (!state.addPluginStrokes(std::move(additions), undoPolicyValue)) {
        error = "The stroke batch could not be added to the current layer";
        return false;
    }
    state.ensurePluginReferences();
    references.clear();
    references.reserve(inputs.size());
    for (size_t index = first; index < state.ink.size(); ++index) {
        references.push_back(state.ink[index].pluginReference);
    }
    update();
    return true;
}

bool Prototype::addPluginTexts(const std::vector<prototype::PluginTextInput>& inputs,
                               prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                               std::string& error) {
    std::vector<prototype::TextItem> additions;
    additions.reserve(inputs.size());
    for (const auto& input: inputs) {
        prototype::TextItem item;
        item.text = input.text;
        item.fontName = input.fontName.value_or(state.textFontName);
        item.fontSize = input.fontSize.value_or(state.textFontSize);
        item.color = input.color ? (0xff000000U | *input.color) : packTextColor(state.textColor);
        item.x = input.x;
        item.y = input.y;
        item.wrapWidth = input.wrap.value_or(-1);
        const auto [width, height] = prototype::textNaturalSize(item);
        item.layoutWidth = width;
        item.layoutHeight = height;
        additions.push_back(std::move(item));
    }
    return addPluginTextItems(std::move(additions), policy, references, error);
}

bool Prototype::addPluginLinks(const std::vector<prototype::PluginLinkInput>& inputs,
                               prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                               std::string& error) {
    std::vector<prototype::TextItem> additions;
    additions.reserve(inputs.size());
    for (const auto& input: inputs) {
        prototype::TextItem item;
        item.text = input.text;
        item.linkUrl = input.url;
        item.isLink = true;
        item.alignment = input.alignment.value_or(0);
        item.fontName = input.fontName.value_or(state.textFontName);
        item.fontSize = input.fontSize.value_or(state.textFontSize);
        item.color = input.color ? (0xff000000U | *input.color) : packTextColor(state.textColor);
        item.x = input.x;
        item.y = input.y;
        const auto [width, height] = prototype::textNaturalSize(item);
        item.layoutWidth = width;
        item.layoutHeight = height;
        additions.push_back(std::move(item));
    }
    return addPluginTextItems(std::move(additions), policy, references, error);
}

bool Prototype::addPluginTextItems(std::vector<prototype::TextItem> additions, prototype::PluginStrokeUndoPolicy policy,
                                   std::vector<std::uintptr_t>& references, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before adding text";
        return false;
    }
    if (additions.empty()) {
        references.clear();
        return true;
    }

    int undoPolicyValue = 0;
    switch (policy) {
        case prototype::PluginStrokeUndoPolicy::Grouped:
            undoPolicyValue = 0;
            break;
        case prototype::PluginStrokeUndoPolicy::Individual:
            undoPolicyValue = 1;
            break;
        case prototype::PluginStrokeUndoPolicy::None:
            undoPolicyValue = 2;
            break;
    }
    const auto first = state.texts.size();
    const auto count = additions.size();
    if (!state.addPluginTexts(std::move(additions), undoPolicyValue)) {
        error = "The text batch could not be added to the current layer";
        return false;
    }
    references.clear();
    references.reserve(count);
    for (std::size_t index = first; index < first + count; ++index) {
        references.push_back(state.texts[index].pluginReference);
    }
    update();
    return true;
}

bool Prototype::addPluginImages(const std::vector<prototype::PluginImageInput>& inputs,
                                prototype::PluginStrokeUndoPolicy policy,
                                std::vector<prototype::PluginImageResult>& results, std::string& error) {
    if (busy || choosingFile || messageDialog || closed || autosaving) {
        error = "Wait for the current document operation to finish before adding images";
        return false;
    }

    results.assign(inputs.size(), {});
    if (inputs.empty()) {
        return true;
    }

    std::vector<prototype::CanvasImage> additions;
    std::vector<std::size_t> inputIndices;
    additions.reserve(std::min(inputs.size(), prototype::maxCanvasImageObjects));
    inputIndices.reserve(additions.capacity());
    const std::size_t remainingImageSlots = state.images.size() >= prototype::maxCanvasImageObjects ?
                                                    0 :
                                                    prototype::maxCanvasImageObjects - state.images.size();
    const auto pageSize = state.pageSize(state.page);

    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        try {
            if (input.path.has_value() == input.data.has_value()) {
                throw std::runtime_error("exactly one of 'path' or image 'data' is required");
            }
            if (!std::isfinite(input.x) || !std::isfinite(input.y) || !std::isfinite(input.scale) || input.scale <= 0 ||
                (input.maxWidth <= 0 && input.maxWidth != -1) || (input.maxHeight <= 0 && input.maxHeight != -1)) {
                throw std::runtime_error("image dimensions, coordinates, or scale are invalid");
            }
            if (additions.size() >= remainingImageSlots) {
                throw std::runtime_error("the document image limit has been reached");
            }

            prototype::CanvasImage image;
            if (input.path) {
                image = prototype::readCanvasImageFile(std::filesystem::path(*input.path));
            } else {
                image.encodedData = *input.data;
                image.surface = prototype::decodeCanvasImage(image.encodedData, image.pixelWidth, image.pixelHeight);
            }

            double width = image.pixelWidth;
            double height = image.pixelHeight;
            if (input.maxWidth != -1 && input.maxHeight != -1) {
                if (input.aspectRatio) {
                    const double scaleY = static_cast<double>(input.maxHeight) / height;
                    const double scaleX = static_cast<double>(input.maxWidth) / width;
                    const double manualScale = std::min(scaleY, scaleX);
                    width *= manualScale;
                    height *= manualScale;
                } else {
                    width = input.maxWidth;
                    height = input.maxHeight;
                }
            } else if (input.maxWidth != -1) {
                if (input.aspectRatio) {
                    height = height / width * input.maxWidth;
                }
                width = input.maxWidth;
            } else if (input.maxHeight != -1) {
                if (input.aspectRatio) {
                    width = width / height * input.maxHeight;
                }
                height = input.maxHeight;
            }
            width *= input.scale;
            height *= input.scale;

            double automaticScale = 1;
            if (input.x + width > pageSize.width || input.y + height > pageSize.height) {
                const double scaleX = (pageSize.width - input.x) / width;
                const double scaleY = (pageSize.height - input.y) / height;
                automaticScale = std::min(scaleX, scaleY);
            }
            width *= automaticScale;
            height *= automaticScale;
            if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
                throw std::runtime_error("image does not fit on the current page");
            }

            image.page = state.page;
            image.transform = {width / image.pixelWidth, 0, 0, height / image.pixelHeight, input.x, input.y};
            if (!prototype::validCanvasImage(image)) {
                throw std::runtime_error("the decoded image has invalid dimensions or transform");
            }
            additions.push_back(std::move(image));
            inputIndices.push_back(index);
        } catch (const std::exception& imageError) {
            results[index].error = "Error: " + std::string(imageError.what());
        }
    }

    if (additions.empty()) {
        return true;
    }

    int undoPolicyValue = 0;
    switch (policy) {
        case prototype::PluginStrokeUndoPolicy::Grouped:
            undoPolicyValue = 0;
            break;
        case prototype::PluginStrokeUndoPolicy::Individual:
            undoPolicyValue = 1;
            break;
        case prototype::PluginStrokeUndoPolicy::None:
            undoPolicyValue = 2;
            break;
    }
    const auto first = state.images.size();
    if (!state.addPluginImages(std::move(additions), undoPolicyValue)) {
        for (const auto inputIndex: inputIndices) {
            results[inputIndex].error = "Error: inserting the image into the current layer failed";
        }
        return true;
    }
    for (std::size_t index = 0; index < inputIndices.size(); ++index) {
        results[inputIndices[index]].reference = state.images[first + index].pluginReference;
    }
    update();
    return true;
}

}  // namespace prototype::gtk4
