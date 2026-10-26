#include <algorithm>
#include <charconv>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <cairo-pdf.h>
#include <glib.h>

#include "control/pagetype/PageTypeHandler.h"
#include "control/ExportHelper.h"
#include "control/jobs/ImageExport.h"
#include "control/jobs/ProgressListener.h"
#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/AudioContent.h"
#include "model/BackgroundConfig.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/Font.h"
#include "model/Image.h"
#include "model/Layer.h"
#include "model/Link.h"
#include "model/PageType.h"
#include "model/Point.h"
#include "model/RectangularElement.h"
#include "model/Stroke.h"
#include "model/Text.h"
#include "model/TextAlignment.h"
#include "model/TexImage.h"
#include "model/XojPage.h"
#include "pdf/base/XojCairoPdfExport.h"
#include "util/Matrix.h"
#include "util/ElementRange.h"
#include "util/Util.h"
#include "util/raii/CairoWrappers.h"
#include "util/serializing/BinObjectEncoding.h"
#include "util/serializing/ObjectInputStream.h"
#include "util/serializing/ObjectOutputStream.h"
#include "view/DocumentView.h"
#include "view/LayerView.h"
#include "view/View.h"

#include "AudioTarget.h"
#include "DocumentAutosave.h"
#include "DocumentImageTransfer.h"
#include "DocumentLayers.h"
#include "DocumentPathTransfer.h"
#include "DocumentPdfPages.h"
#include "DocumentPublish.h"
#include "PageSizeTransfer.h"
#include "PdfPageLabels.h"
#include "DocumentTextTransfer.h"
#include "DocumentTransfer.h"
#include "StrokeClipboard.h"

class GTK4DocumentLayerAccess {
public:
    static void setBackgroundVisible(XojPage& page, bool visible) { page.setLayerVisible(0, visible); }
};

namespace {

namespace fs = std::filesystem;

struct LoadContext {
    DocumentHandler handler;
    std::vector<std::string> warnings;
    LoadHandler loader{&warnings};
};

std::unique_ptr<Document> load(const fs::path& input, LoadContext& context) {
    if (input.empty()) {
        auto document = std::make_unique<Document>(&context.handler);
        document->addPage(std::make_shared<XojPage>(595, 842));
        return document;
    }
    if (g_ascii_strcasecmp(input.extension().string().c_str(), ".pdf") == 0) {
        auto document = std::make_unique<Document>(&context.handler);
        if (!document->readPdf(input, true, false)) {
            throw std::runtime_error(document->getLastErrorMsg());
        }
        return document;
    }
    auto& loader = context.loader;
    auto document = loader.loadDocument(input);
    if (!context.warnings.empty()) {
        throw std::runtime_error("Document requires recovery: " + context.warnings.front());
    }
    if (loader.isAttachedPdfMissing() || !loader.getMissingPdfFilename().empty()) {
        throw std::runtime_error("The document's PDF background is missing");
    }
    return document;
}

std::vector<prototype::bridge::StrokePacket> extractEditableInk(
        Document& document, std::vector<prototype::TextItem>* extractedTexts,
        std::vector<prototype::bridge::ImagePacket>* extractedImages);

void appendStroke(Layer& layer, const prototype::bridge::StrokePacket& packet) {
    auto stroke = std::make_unique<Stroke>();
    stroke->setToolType(packet.whiteout ? StrokeTool::ERASER :
                                          (packet.highlighter ? StrokeTool::HIGHLIGHTER : StrokeTool::PEN));
    stroke->setWidth(packet.width);
    stroke->setFill(packet.fill);
    stroke->setStrokeCapStyle(static_cast<StrokeCapStyle::Value>(packet.capStyle));
    if (packet.whiteout) {
        stroke->setColor(Color{255, 255, 255});
    } else {
        stroke->setColor(Color{static_cast<uint8_t>(std::lround(packet.color[0] * 255)),
                               static_cast<uint8_t>(std::lround(packet.color[1] * 255)),
                               static_cast<uint8_t>(std::lround(packet.color[2] * 255))});
    }
    for (const auto& point: packet.points) {
        if (packet.pressureSensitive && !packet.highlighter && !packet.whiteout) {
            stroke->addPoint(Point(point.x, point.y, point.pressure * packet.width));
        } else {
            stroke->addPoint(Point(point.x, point.y));
        }
    }
    if (stroke->getPointCount() == 1) {
        const auto point = stroke->getPoint(0);
        stroke->addPoint(point);
    }
    layer.addElement(std::move(stroke));
}

void appendText(Layer& layer, const prototype::TextItem& item) {
    if (item.isLink) {
        auto link = std::make_unique<Link>();
        link->setText(item.text);
        link->setUrl(item.linkUrl);
        link->setFont(XojFont(item.fontName, item.fontSize));
        link->setColor(Color{item.color});
        link->setAlignment(TextAlignment(static_cast<TextAlignment::Value>(item.alignment)));
        link->setTransformation({item.xx, item.yx, item.xy, item.yy, {item.x, item.y}});
        layer.addElement(std::move(link));
        return;
    }
    auto text = std::make_unique<Text>();
    text->setText(item.text);
    text->setFont(XojFont(item.fontName, item.fontSize));
    text->setColor(Color{item.color});
    std::vector<Text::StyleRun> runs;
    runs.reserve(item.styleRuns.size());
    for (const auto& run: item.styleRuns) {
        runs.push_back({run.start, run.end, XojFont(run.fontName, run.fontSize),
                        run.color ? std::optional<Color>{Color{*run.color}} : std::nullopt, run.attributes});
    }
    text->setStyleRuns(std::move(runs));
    text->setWrap(item.wrapWidth);
    text->setAlignment(TextAlignment(static_cast<TextAlignment::Value>(item.alignment)));
    text->setJustify(item.justify);
    text->setTransformation({item.xx, item.yx, item.xy, item.yy, {item.x, item.y}});
    layer.addElement(std::move(text));
}

void appendImage(Layer& layer, const prototype::bridge::ImagePacket& packet) {
    std::unique_ptr<RectangularElement> image;
    if (packet.kind == prototype::CanvasImageKind::Latex) {
        auto texImage = std::make_unique<TexImage>();
        GError* error = nullptr;
        auto bytes = packet.encodedData;
        const bool loaded = texImage->loadData(std::move(bytes), &error);
        if (!loaded || (!texImage->getPdf() && !texImage->getImage())) {
            const std::string detail = error ? error->message : "Invalid LaTeX image data";
            g_clear_error(&error);
            throw std::runtime_error("Cannot decode transferred LaTeX image: " + detail);
        }
        g_clear_error(&error);
        texImage->setText(packet.latexSource);
        image = std::move(texImage);
    } else {
        auto rasterImage = std::make_unique<Image>();
        rasterImage->setImage(packet.encodedData);
        if (const auto error = rasterImage->renderBuffer()) {
            throw std::runtime_error("Cannot decode transferred image: " + *error);
        }
        image = std::move(rasterImage);
    }
    const auto& m = packet.transform;
    image->setTransformation(xoj::util::Matrix{m.xx, m.yx, m.xy, m.yy, {m.x, m.y}});
    layer.addElement(std::move(image));
}

void appendLayerAwareObjects(Document& document, const std::vector<prototype::bridge::StrokePacket>& strokes,
                             const std::vector<prototype::TextItem>& texts,
                             const std::vector<prototype::bridge::ImagePacket>& images,
                             const prototype::bridge::LayerDocument& layerDocument) {
    struct OrderedObject {
        int page;
        size_t layerId;
        size_t origin;
        size_t order;
        const prototype::bridge::StrokePacket* stroke;
        const prototype::TextItem* text;
        const prototype::bridge::ImagePacket* image;
    };
    std::vector<OrderedObject> objects;
    objects.reserve(strokes.size() + texts.size());
    for (const auto& stroke: strokes) {
        prototype::bridge::validate(stroke);
        objects.push_back({stroke.page, stroke.layerId, stroke.originLayer, stroke.layerOrder, &stroke, nullptr, nullptr});
    }
    for (const auto& text: texts) {
        prototype::bridge::text_transfer_detail::validateTextItem(text);
        objects.push_back({text.page, text.layerId, text.originLayer, text.layerOrder, nullptr, &text, nullptr});
    }
    for (const auto& image: images) {
        prototype::bridge::validate(image);
        objects.push_back({image.page, image.layerId, image.originLayer, image.layerOrder, nullptr, nullptr, &image});
    }
    if (layerDocument.size() != document.getPageCount()) {
        throw std::runtime_error("Layer transfer page count does not match the document");
    }
    for (auto& item: objects) {
        if (item.page < 0 || static_cast<size_t>(item.page) >= layerDocument.size()) {
            throw std::runtime_error("Transferred editable object refers to a missing page");
        }
        const auto& layers = layerDocument[static_cast<size_t>(item.page)].layers;
        if (layers.empty()) {
            throw std::runtime_error("Transferred object has no destination layer");
        }
        if (item.layerId == prototype::legacyTopLayer) {
            item.layerId = layers.size();
            if (layers.back().origins.empty()) {
                throw std::runtime_error("Transferred object has no target layer origin");
            }
            item.origin = layers.back().origins.back();
        }
        if (item.layerId == 0 || item.layerId > layers.size()) {
            throw std::runtime_error("Transferred object refers to a missing target layer");
        }
        if (item.origin == prototype::newLayerOrigin) {
            if (layers[item.layerId - 1].origins.empty()) {
                throw std::runtime_error("Transferred object has no target layer origin");
            }
            item.origin = layers[item.layerId - 1].origins.back();
        }
    }
    std::stable_sort(objects.begin(), objects.end(), [](const OrderedObject& a, const OrderedObject& b) {
        return std::tie(a.page, a.layerId, a.origin, a.order) < std::tie(b.page, b.layerId, b.origin, b.order);
    });

    for (size_t pageIndex = 0; pageIndex < document.getPageCount(); ++pageIndex) {
        auto page = document.getPage(pageIndex);
        const auto& pageInfo = layerDocument[pageIndex];
        if (pageInfo.initialLayerCount != page->getLayerCount() || pageInfo.layers.empty()) {
            throw std::runtime_error("Layer transfer no longer matches the document snapshot");
        }
        auto& oldLayers = page->getLayers();
        std::vector<size_t> sourceUseCount(pageInfo.initialLayerCount, 0);
        for (const auto& targetInfo: pageInfo.layers) {
            for (size_t origin: targetInfo.origins) {
                if (origin < sourceUseCount.size()) {
                    ++sourceUseCount[origin];
                }
            }
        }
        std::vector<Layer*> replacement;
        replacement.reserve(pageInfo.layers.size());
        for (const auto& info: pageInfo.layers) {
            auto* layer = new Layer();
            if (!info.name.empty()) {
                layer->setName(info.name);
            }
            layer->setVisible(info.visible);
            replacement.push_back(layer);
        }

        for (size_t targetIndex = 0; targetIndex < pageInfo.layers.size(); ++targetIndex) {
            auto* target = replacement[targetIndex];
            for (size_t origin: pageInfo.layers[targetIndex].origins) {
                if (origin < pageInfo.initialLayerCount) {
                    auto* source = oldLayers.at(origin);
                    std::vector<ElementPtr> sourceElements;
                    if (sourceUseCount[origin] > 1) {
                        std::unique_ptr<Layer> duplicate(source->clone());
                        sourceElements = duplicate->clearNoFree();
                    } else {
                        sourceElements = source->clearNoFree();
                    }
                    for (auto& element: sourceElements) {
                        target->addElement(std::move(element));
                    }
                }
                const auto first = std::lower_bound(objects.begin(), objects.end(),
                                                    std::tuple{pageIndex, targetIndex + 1, origin, size_t{0}},
                                                    [](const OrderedObject& object, const auto& key) {
                                                        return std::tie(object.page, object.layerId, object.origin,
                                                                        object.order) < key;
                                                    });
                for (auto item = first; item != objects.end() && item->page == static_cast<int>(pageIndex) &&
                                        item->layerId == targetIndex + 1 && item->origin == origin; ++item) {
                    if (item->stroke) {
                        appendStroke(*target, *item->stroke);
                    } else if (item->text) {
                        appendText(*target, *item->text);
                    } else {
                        appendImage(*target, *item->image);
                    }
                }
            }
        }

        for (const auto& item: objects) {
            if (item.page != static_cast<int>(pageIndex)) {
                continue;
            }
            if (item.layerId == 0 || item.layerId > pageInfo.layers.size() ||
                std::find(pageInfo.layers[item.layerId - 1].origins.begin(),
                          pageInfo.layers[item.layerId - 1].origins.end(), item.origin) ==
                        pageInfo.layers[item.layerId - 1].origins.end()) {
                throw std::runtime_error("Transferred object refers to an absent layer origin");
            }
        }

        for (auto* oldLayer: oldLayers) {
            delete oldLayer;
        }
        oldLayers = std::move(replacement);
        page->setSelectedLayerId(pageInfo.selectedLayer);
        if (pageInfo.backgroundName) {
            page->setBackgroundName(*pageInfo.backgroundName);
        }
        if (!pageInfo.backgroundFormat.empty()) {
            PageType background;
            background.format = PageTypeHandler::getPageTypeFormatForString(pageInfo.backgroundFormat);
            background.config = pageInfo.backgroundConfig;
            page->setBackgroundType(background);
        }
        GTK4DocumentLayerAccess::setBackgroundVisible(*page, pageInfo.backgroundVisible);
    }
}

void discardEditableObjects(Document& document) {
    std::vector<prototype::TextItem> discardedTexts;
    std::vector<prototype::bridge::ImagePacket> discardedImages;
    static_cast<void>(extractEditableInk(document, &discardedTexts, &discardedImages));
}

std::optional<prototype::bridge::StrokePacket> toEditablePacket(const Element& element, int pageIndex,
                                                                size_t layerId, size_t originLayer,
                                                                size_t layerOrder) {
    if (element.getType() != ELEMENT_STROKE) {
        return std::nullopt;
    }
    const auto* stroke = dynamic_cast<const Stroke*>(&element);
    if (!stroke) {
        return std::nullopt;
    }
    const auto& pts = stroke->getPointVector();
    const double width = stroke->getWidth();
    const int fill = stroke->getFill();
    const int capStyle = static_cast<int>(stroke->getStrokeCapStyle());
    if (pts.size() < 2 || !std::isfinite(width) || width <= 0.0 || fill < -1 || fill > 255 || capStyle < 0 ||
        capStyle > 2 || stroke->getLineStyle().hasDashes() || !stroke->getAudioFilename().empty()) {
        return std::nullopt;
    }
    const StrokeTool tool = stroke->getToolType();
    const uint32_t rgb = static_cast<uint32_t>(stroke->getColor()) & 0xffffffU;
    bool highlighter = false;
    bool whiteout = false;
    bool pressureSensitive = false;
    if (tool == StrokeTool::PEN) {
        pressureSensitive = stroke->hasPressure();
    } else if (tool == StrokeTool::HIGHLIGHTER) {
        if (stroke->hasPressure()) {
            return std::nullopt;
        }
        highlighter = true;
    } else if (tool == StrokeTool::ERASER) {
        if (rgb != 0xffffffU || stroke->hasPressure() || fill != -1) {
            return std::nullopt;
        }
        whiteout = true;
    } else {
        return std::nullopt;
    }

    prototype::bridge::StrokePacket packet;
    packet.page = pageIndex;
    packet.layerId = layerId;
    packet.originLayer = originLayer;
    packet.layerOrder = layerOrder;
    packet.highlighter = highlighter;
    packet.whiteout = whiteout;
    packet.pressureSensitive = pressureSensitive;
    packet.width = width;
    packet.fill = fill;
    packet.capStyle = capStyle;
    packet.color = {static_cast<double>((rgb >> 16U) & 0xffU) / 255.0,
                    static_cast<double>((rgb >> 8U) & 0xffU) / 255.0,
                    static_cast<double>(rgb & 0xffU) / 255.0};
    packet.points.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& point = pts[i];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
            return std::nullopt;
        }
        double pressure = 1.0;
        if (pressureSensitive) {
            // Native .xopp XML stores segment widths in pts[0..N-2].z and leaves pts[N-1].z == Point::NO_PRESSURE.
            const double rawWidth = (i + 1 < pts.size() || point.z > 0.0) ? point.z : pts[i - 1].z;
            if (!std::isfinite(rawWidth) || rawWidth <= 0.0) {
                return std::nullopt;
            }
            pressure = rawWidth / width;
            if (!std::isfinite(pressure) || pressure <= 0.0) {
                return std::nullopt;
            }
        }
        packet.points.push_back({point.x, point.y, pressure});
    }
    return packet;
}

std::optional<prototype::TextItem> toEditableTextPacket(const Element& element, int pageIndex, size_t layerId,
                                                        size_t originLayer, size_t layerOrder) {
    if (element.getType() != ELEMENT_TEXT && element.getType() != ELEMENT_LINK) {
        return std::nullopt;
    }
    prototype::TextItem item;
    item.page = pageIndex;
    item.layerId = layerId;
    item.originLayer = originLayer;
    item.layerOrder = layerOrder;
    if (element.getType() == ELEMENT_LINK) {
        const auto* link = dynamic_cast<const Link*>(&element);
        if (!link || link->getText().empty()) {
            return std::nullopt;
        }
        item.isLink = true;
        item.text = link->getText();
        item.linkUrl = link->getUrl();
        item.fontName = link->getFont().getName();
        item.fontSize = link->getFont().getSize();
        item.color = static_cast<uint32_t>(link->getColor());
        item.alignment = static_cast<int>(static_cast<TextAlignment::Value>(link->getAlignment()));
        const auto& naturalSize = link->getNaturalSize();
        item.layoutWidth = naturalSize.width;
        item.layoutHeight = naturalSize.height;
        const auto& matrix = link->getTransformation();
        item.xx = matrix.xx;
        item.yx = matrix.yx;
        item.xy = matrix.xy;
        item.yy = matrix.yy;
        item.x = matrix.shift.x;
        item.y = matrix.shift.y;
    } else {
        const auto* text = dynamic_cast<const Text*>(&element);
        if (!text || !text->getAudioFilename().empty() || text->getText().empty()) {
            return std::nullopt;
        }
        item.text = text->getText();
        item.fontName = text->getFontName();
        item.fontSize = text->getFontSize();
        item.color = static_cast<uint32_t>(text->getColor());
        item.wrapWidth = text->getWrap();
        const auto& naturalSize = text->getNaturalSize();
        item.layoutWidth = naturalSize.width;
        item.layoutHeight = naturalSize.height;
        item.alignment = static_cast<int>(static_cast<TextAlignment::Value>(text->getAlign()));
        item.justify = text->getJustify();
        const auto& matrix = text->getTransformation();
        item.xx = matrix.xx;
        item.yx = matrix.yx;
        item.xy = matrix.xy;
        item.yy = matrix.yy;
        item.x = matrix.shift.x;
        item.y = matrix.shift.y;
        item.styleRuns.reserve(text->getStyleRuns().size());
        for (const auto& run: text->getStyleRuns()) {
            item.styleRuns.push_back({run.start, run.end, run.font.getName(), run.font.getSize(),
                                      run.color ? std::optional<uint32_t>{static_cast<uint32_t>(*run.color)} : std::nullopt,
                                      run.attributes});
        }
    }
    try {
        prototype::bridge::text_transfer_detail::validateTextItem(item);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return item;
}

struct EditableSuffixItem {
    std::optional<prototype::bridge::StrokePacket> stroke;
    std::optional<prototype::TextItem> text;
    std::optional<prototype::bridge::ImagePacket> image;
};

std::optional<prototype::bridge::ImagePacket> toEditableImagePacket(const Element& element, int page, size_t layerId,
                                                                     size_t originLayer, size_t order) {
    prototype::bridge::ImagePacket packet;
    packet.page = page;
    packet.layerId = layerId;
    packet.originLayer = originLayer;
    packet.layerOrder = order;
    if (element.getType() == ELEMENT_TEXIMAGE) {
        const auto* texImage = dynamic_cast<const TexImage*>(&element);
        if (!texImage || (!texImage->getPdf() && !texImage->getImage()) || texImage->getBinaryData().empty() ||
            texImage->getBinaryData().size() > prototype::maxCanvasImageBytes ||
            texImage->getText().size() > prototype::maxCanvasLatexSourceBytes) {
            return std::nullopt;
        }
        const auto size = texImage->getNativeSize();
        if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0 || size.height <= 0 ||
            size.width > 100000 || size.height > 100000) {
            return std::nullopt;
        }
        packet.kind = prototype::CanvasImageKind::Latex;
        packet.nativeWidth = size.width;
        packet.nativeHeight = size.height;
        packet.pixelWidth = static_cast<int>(std::ceil(size.width));
        packet.pixelHeight = static_cast<int>(std::ceil(size.height));
        packet.latexSource = texImage->getText();
        packet.encodedData = texImage->getBinaryData();
        const auto& matrix = texImage->getTransformation();
        packet.transform = {matrix.xx, matrix.yx, matrix.xy, matrix.yy, matrix.shift.x, matrix.shift.y};
    } else if (element.getType() == ELEMENT_IMAGE) {
        const auto* image = dynamic_cast<const Image*>(&element);
        if (!image || !image->hasData() || image->getBinaryData().empty() ||
            image->getBinaryData().size() > prototype::maxCanvasImageBytes || image->renderBuffer()) {
            return std::nullopt;
        }
        const auto size = image->getNaturalSize();
        packet.pixelWidth = static_cast<int>(std::lround(size.width));
        packet.pixelHeight = static_cast<int>(std::lround(size.height));
        packet.encodedData = image->getBinaryData();
        const auto& matrix = image->getTransformation();
        packet.transform = {matrix.xx, matrix.yx, matrix.xy, matrix.yy, matrix.shift.x, matrix.shift.y};
    } else {
        return std::nullopt;
    }
    try {
        prototype::bridge::validate(packet);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return packet;
}

std::vector<prototype::bridge::StrokePacket> extractEditableInk(Document& document,
                                                                 std::vector<prototype::TextItem>* extractedTexts = nullptr,
                                                                 std::vector<prototype::bridge::ImagePacket>* extractedImages = nullptr) {
    std::vector<prototype::bridge::StrokePacket> extracted;
    size_t totalPoints = 0;
    size_t totalTextBytes = 0;
    size_t totalTextRuns = 0;
    size_t totalImageBytes = 0;
    size_t totalImages = 0;
    for (size_t pageIndex = 0; pageIndex < document.getPageCount(); ++pageIndex) {
        auto page = document.getPage(pageIndex);
        for (size_t layerIndex = 0; layerIndex < page->getLayerCount(); ++layerIndex) {
            auto& elements = page->getLayers()[layerIndex]->getElements();
            size_t suffixStart = elements.size();
            std::vector<EditableSuffixItem> pagePackets;
            while (suffixStart > 0) {
                EditableSuffixItem candidate;
                candidate.stroke = toEditablePacket(*elements[suffixStart - 1], static_cast<int>(pageIndex),
                                                    layerIndex + 1, layerIndex, suffixStart - 1);
                if (candidate.stroke) {
                    if (candidate.stroke->points.size() > prototype::bridge::maxTransferPoints - totalPoints) {
                        break;
                    }
                    totalPoints += candidate.stroke->points.size();
                } else {
                    if (extractedTexts) {
                        candidate.text = toEditableTextPacket(*elements[suffixStart - 1], static_cast<int>(pageIndex),
                                                              layerIndex + 1, layerIndex, suffixStart - 1);
                    }
                    if (candidate.text) {
                        if (candidate.text->text.size() >
                                    prototype::bridge::maxEditableTextBytes - totalTextBytes ||
                            candidate.text->styleRuns.size() >
                                    prototype::bridge::maxEditableTextStyleRuns - totalTextRuns) {
                            break;
                        }
                        totalTextBytes += candidate.text->text.size();
                        totalTextRuns += candidate.text->styleRuns.size();
                    } else if (extractedImages) {
                        candidate.image = toEditableImagePacket(*elements[suffixStart - 1], static_cast<int>(pageIndex),
                                                                layerIndex + 1, layerIndex, suffixStart - 1);
                        const size_t payloadSize = candidate.image ?
                                                           candidate.image->encodedData.size() +
                                                                   candidate.image->latexSource.size() :
                                                           0;
                        if (!candidate.image || totalImages >= prototype::bridge::maxEditableImages ||
                            totalImageBytes > prototype::bridge::maxImageTransferFileBytes ||
                            payloadSize > prototype::bridge::maxImageTransferFileBytes - totalImageBytes) {
                            break;
                        }
                        ++totalImages;
                        totalImageBytes += payloadSize;
                    } else {
                        break;
                    }
                }
                pagePackets.push_back(std::move(candidate));
                --suffixStart;
            }
            if (suffixStart < elements.size()) {
                elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(suffixStart), elements.end());
                for (auto item = pagePackets.rbegin(); item != pagePackets.rend(); ++item) {
                    if (item->stroke) {
                        extracted.push_back(std::move(*item->stroke));
                    }
                    if (item->text && extractedTexts) {
                        extractedTexts->push_back(std::move(*item->text));
                    }
                    if (item->image && extractedImages) {
                        extractedImages->push_back(std::move(*item->image));
                    }
                }
            }
        }
    }
    return extracted;
}

void updatePreview(Document& document) {
    if (!document.getPageCount()) {
        return;
    }
    const auto page = document.getPage(0);
    const auto scale = 128.0 / std::max(page->getWidth(), page->getHeight());
    xoj::util::CairoSurfaceSPtr surface;
    surface.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(page->getWidth() * scale)),
                                             static_cast<int>(std::ceil(page->getHeight() * scale))),
                  xoj::util::adopt);
    xoj::util::CairoSPtr cr;
    cr.reset(cairo_create(surface.get()), xoj::util::adopt);
    cairo_scale(cr.get(), scale, scale);
    auto flags = xoj::view::BACKGROUND_SHOW_ALL;
    if (page->getBackgroundType().isPdfPage()) {
        if (auto pdf = document.getPdfPage(page->getPdfPageNr())) {
            pdf->render(cr.get());
        }
        flags.showPDF = xoj::view::HIDE_PDF_BACKGROUND;
    }
    DocumentView view;
    view.drawPage(page, cr.get(), false, flags);
    if (cairo_status(cr.get()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error("Cannot update document preview");
    }
    document.setPreview(surface);
}

void requireNewTarget(const fs::path& target) {
    if (fs::exists(fs::symlink_status(target))) {
        throw std::runtime_error("Output already exists; choose a new filename");
    }
}

prototype::bridge::LayerDocument readLayerDocument(
        const Document& document, const prototype::bridge::LayerDocument* layerMetadata = nullptr) {
    using namespace background_config_strings;
    prototype::bridge::LayerDocument layers;
    layers.reserve(document.getPageCount());
    for (size_t pageIndex = 0; pageIndex < document.getPageCount(); ++pageIndex) {
        const auto page = document.getPage(pageIndex);
        prototype::bridge::PageLayerInfo pageInfo;
        pageInfo.initialLayerCount = page->getLayerCount();
        pageInfo.selectedLayer = std::clamp(page->getSelectedLayerId(), size_t{1}, pageInfo.initialLayerCount);
        pageInfo.backgroundVisible = page->isLayerVisible(0);
        if (page->backgroundHasName()) {
            pageInfo.backgroundName = page->getBackgroundName();
        }
        const auto background = page->getBackgroundType();
        pageInfo.backgroundFormat = PageTypeHandler::getStringForPageTypeFormat(background.format);
        pageInfo.backgroundConfig = background.config;
        pageInfo.backgroundColor = static_cast<uint32_t>(page->getBackgroundColor()) & 0xffffffU;
        if (background.format == PageTypeFormat::Lined || background.format == PageTypeFormat::Ruled) {
            pageInfo.gridYOffset = 80.0;
        } else if (background.format == PageTypeFormat::IsoDotted ||
                   background.format == PageTypeFormat::IsoGraph) {
            double triangleSize = 14.17;
            const BackgroundConfig config(background.config);
            config.loadValue(CFG_RASTER, triangleSize);
            if (std::isfinite(triangleSize) && triangleSize > 0) {
                constexpr double isoMarginFactor = 1.0;
                const double xStep = std::sqrt(3.0) / 2.0 * triangleSize;
                const double yStep = triangleSize / 2.0;
                const auto pageHeight = page->getHeight();
                const auto pageWidth = page->getWidth();
                const auto cols = std::max(0.0, std::floor((pageWidth - 2.0 * isoMarginFactor * triangleSize) / xStep));
                const auto rows = std::max(0.0, std::floor((pageHeight - 2.0 * isoMarginFactor * triangleSize) / yStep));
                pageInfo.gridXOffset = (pageWidth - cols * xStep) / 2.0;
                pageInfo.gridYOffset = (pageHeight - rows * yStep) / 2.0;
                pageInfo.gridXStep = xStep;
                pageInfo.gridYStep = yStep;
            }
        }
        pageInfo.layers.clear();
        for (size_t layerIndex = 0; layerIndex < page->getLayerCount(); ++layerIndex) {
            const auto* layer = page->getLayersView()[layerIndex];
            auto name = layer->hasName() ? layer->getName() : "";
            const bool hasNonEditableElements =
                    layerMetadata && pageIndex < layerMetadata->size() &&
                                    layerIndex < (*layerMetadata)[pageIndex].layers.size() ?
                            (*layerMetadata)[pageIndex].layers[layerIndex].hasNonEditableElements :
                            layer->isAnnotated();
            pageInfo.layers.push_back({std::move(name), layer->isVisible(), {layerIndex}, hasNonEditableElements});
        }
        layers.push_back(std::move(pageInfo));
    }
    return layers;
}

std::vector<int> readPdfPageMap(const Document& document) {
    std::vector<int> pages;
    pages.reserve(document.getPageCount());
    for (size_t pageIndex = 0; pageIndex < document.getPageCount(); ++pageIndex) {
        const auto pdfPage = document.getPage(pageIndex)->getPdfPageNr();
        if (pdfPage == npos) {
            pages.push_back(-1);
        } else if (pdfPage > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("PDF background page index is too large");
        } else {
            pages.push_back(static_cast<int>(pdfPage));
        }
    }
    return pages;
}

std::vector<std::string> readPdfPageLabels(const Document& document) {
    std::vector<std::string> labels;
    labels.reserve(document.getPdfPageCount());
    size_t totalBytes = 0;
    for (size_t pageIndex = 0; pageIndex < document.getPdfPageCount(); ++pageIndex) {
        const auto page = document.getPdfPage(pageIndex);
        if (!page) {
            throw std::runtime_error("PDF page label source is missing a page");
        }
        auto label = page->getPageLabel();
        if (label.size() > prototype::bridge::maxPdfPageLabelLength ||
            label.size() > prototype::bridge::maxPdfPageLabelBytes - totalBytes) {
            throw std::runtime_error("PDF page labels exceed the supported size limit");
        }
        totalBytes += label.size();
        labels.push_back(std::move(label));
    }
    return labels;
}

void writePdfTextSource(Document& document, const fs::path& output) {
    if (document.getPdfPageCount() == 0) {
        return;
    }
    requireNewTarget(output);
    GError* error = nullptr;
    if (!document.getPdfDocument().save(output, &error)) {
        const std::string message = error ? error->message : "unknown PDF error";
        g_clear_error(&error);
        throw std::runtime_error("Cannot save PDF text-selection source: " + message);
    }
}

void finishPdfPage(cairo_t* cr, cairo_surface_t* surface) {
    const cairo_status_t contextStatus = cairo_status(cr);
    if (contextStatus != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string("Cannot render layer preview: ") + cairo_status_to_string(contextStatus));
    }
    cairo_show_page(cr);
}

void createBackgroundPreview(Document& document, const fs::path& output) {
    requireNewTarget(output);
    auto* surface = cairo_pdf_surface_create(output.c_str(), 1, 1);
    auto* cr = cairo_create(surface);
    try {
        for (size_t index = 0; index < document.getPageCount(); ++index) {
            const auto page = document.getPage(index);
            cairo_pdf_surface_set_size(surface, page->getWidth(), page->getHeight());
            cairo_save(cr);
            auto flags = xoj::view::BACKGROUND_SHOW_ALL;
            flags.forceVisible = xoj::view::FORCE_VISIBLE;
            if (page->getBackgroundType().isPdfPage()) {
                if (auto pdf = document.getPdfPage(page->getPdfPageNr())) {
                    pdf->render(cr);
                }
                flags.showPDF = xoj::view::HIDE_PDF_BACKGROUND;
            }
            DocumentView view;
            view.initDrawing(page, cr, true);
            view.drawBackground(flags);
            view.finializeDrawing();
            cairo_restore(cr);
            finishPdfPage(cr, surface);
        }
        cairo_surface_finish(surface);
        const auto status = cairo_surface_status(surface);
        if (status != CAIRO_STATUS_SUCCESS) {
            throw std::runtime_error(std::string("Cannot write background preview: ") + cairo_status_to_string(status));
        }
    } catch (...) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        throw;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void createLayerPreview(Document& document, size_t layerIndex, const fs::path& output, bool fadeNonAudio = false) {
    requireNewTarget(output);
    auto* surface = cairo_pdf_surface_create(output.c_str(), 1, 1);
    auto* cr = cairo_create(surface);
    try {
        for (size_t index = 0; index < document.getPageCount(); ++index) {
            const auto page = document.getPage(index);
            cairo_pdf_surface_set_size(surface, page->getWidth(), page->getHeight());
            if (layerIndex < page->getLayerCount()) {
                DocumentView view;
                view.initDrawing(page, cr, true);
                const auto context = fadeNonAudio ?
                        xoj::view::Context{cr, xoj::view::FADE_OUT_NON_AUDIO_, xoj::view::HIDE_CURRENT_EDITING,
                                           xoj::view::NORMAL_COLOR} :
                        xoj::view::Context::createDefault(cr);
                xoj::view::LayerView(page->getLayersView()[layerIndex]).draw(context);
                view.finializeDrawing();
            }
            finishPdfPage(cr, surface);
        }
        cairo_surface_finish(surface);
        const auto status = cairo_surface_status(surface);
        if (status != CAIRO_STATUS_SUCCESS) {
            throw std::runtime_error(std::string("Cannot write layer preview: ") + cairo_status_to_string(status));
        }
    } catch (...) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        throw;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

std::vector<prototype::AudioTarget> renderAudioTargets(Document& document, const fs::path& audioFolder,
                                                       const fs::path& audioDirectory) {
    std::vector<prototype::AudioTarget> targets;
    std::error_code directoryError;
    fs::create_directories(audioDirectory, directoryError);
    if (directoryError) {
        throw std::runtime_error("Cannot create the audio playback cache: " + directoryError.message());
    }

    for (size_t pageIndex = 0; pageIndex < document.getPageCount(); ++pageIndex) {
        auto page = document.getPage(pageIndex);
        for (size_t layerIndex = 0; layerIndex < page->getLayerCount(); ++layerIndex) {
            const auto& elements = page->getLayersView()[layerIndex]->getElementsView();
            for (size_t elementIndex = 0; elementIndex < elements.size(); ++elementIndex) {
                const auto* element = elements[elementIndex];
                const auto* audio = dynamic_cast<const AudioContent*>(element);
                if (!audio || audio->getAudioFilename().empty()) {
                    continue;
                }

                prototype::AudioTarget target;
                target.page = static_cast<int>(pageIndex);
                target.originLayer = layerIndex;
                target.layerOrder = elementIndex;
                target.timestampMs = static_cast<uint64_t>(audio->getTimestamp());
                target.filename = audio->getAudioFilename().string();
                const auto& bounds = element->getBoundingBox();
                target.boundsX = bounds.x;
                target.boundsY = bounds.y;
                target.boundsWidth = bounds.width;
                target.boundsHeight = bounds.height;

                fs::path sourcePath = audio->getAudioFilename();
                bool parentIsRoot = false;
                if (sourcePath.has_parent_path()) {
                    std::error_code canonicalError;
                    parentIsRoot = fs::weakly_canonical(sourcePath.parent_path(), canonicalError) == fs::path("/") &&
                                   !canonicalError;
                }
                if (!sourcePath.has_parent_path() || parentIsRoot) {
                    sourcePath = audioFolder / sourcePath;
                }
                target.filename = sourcePath.string();
                target.playbackFilename = target.filename;

                if (const auto* stroke = dynamic_cast<const Stroke*>(element)) {
                    target.shape = prototype::AudioTargetShape::Stroke;
                    target.strokeWidth = stroke->getWidth();
                    target.pressureSensitive = stroke->hasPressure();
                    target.capStyle = static_cast<int>(stroke->getStrokeCapStyle());
                    target.points.reserve(stroke->getPointVector().size());
                    for (const auto& point: stroke->getPointVector()) {
                        target.points.push_back({point.x, point.y, point.z});
                    }
                } else if (const auto* rectangle = dynamic_cast<const RectangularElement*>(element)) {
                    target.shape = prototype::AudioTargetShape::Rectangle;
                    const auto& matrix = rectangle->getTransformation();
                    target.xx = matrix.xx;
                    target.yx = matrix.yx;
                    target.xy = matrix.xy;
                    target.yy = matrix.yy;
                    target.x = matrix.shift.x;
                    target.y = matrix.shift.y;
                    const auto& size = rectangle->getNaturalSize();
                    target.naturalWidth = size.width;
                    target.naturalHeight = size.height;
                } else {
                    continue;
                }
                prototype::audio_target_detail::validate(target);

                std::error_code sourceError;
                if (fs::is_regular_file(fs::status(sourcePath, sourceError)) && !sourceError) {
                    gchar* digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, sourcePath.string().c_str(), -1);
                    const auto destination = audioDirectory / (std::string("audio-") + (digest ? digest : "") +
                                                                sourcePath.extension().string());
                    g_free(digest);
                    std::error_code copyError;
                    if (!fs::exists(destination) && fs::copy_file(sourcePath, destination, copyError)) {
                        target.playbackFilename = destination.string();
                    } else if (fs::is_regular_file(destination)) {
                        target.playbackFilename = destination.string();
                    }
                }
                targets.push_back(std::move(target));
            }
        }
    }
    return targets;
}

std::vector<prototype::bridge::PageSizePacket> readDocumentPageSizes(const Document& document);

void renderPreviewBundle(Document& document, const fs::path& preview, const fs::path& layerState,
                         const fs::path& audioFolder = {}, const fs::path& audioDirectory = {},
                         const prototype::bridge::LayerDocument* layerMetadata = nullptr) {
    createBackgroundPreview(document, preview);
    auto pdfBackgroundFilename = document.getPdfFilepath().string();
    const auto sourceMetadata = document.getFilepath().parent_path() / "pdf-background-path.dat";
    std::error_code metadataError;
    if (fs::is_regular_file(fs::symlink_status(sourceMetadata, metadataError)) && !metadataError) {
        pdfBackgroundFilename = prototype::bridge::readDocumentPathTransfer(sourceMetadata);
    }
    prototype::bridge::writeDocumentPathTransfer(preview.parent_path() / "pdf-background-path.dat",
                                                  pdfBackgroundFilename);
    writePdfTextSource(document, preview.parent_path() / "background.pdf");
    prototype::bridge::writePdfPageMap(preview.parent_path() / "pdf-pages.dat", readPdfPageMap(document));
    prototype::bridge::writePdfPageLabels(preview.parent_path() / "pdf-labels.dat", readPdfPageLabels(document));
    prototype::bridge::writePageSizeTransfer(preview.parent_path() / "page-sizes.dat",
                                              readDocumentPageSizes(document));
    size_t maxLayers = 0;
    for (size_t index = 0; index < document.getPageCount(); ++index) {
        maxLayers = std::max(maxLayers, document.getPage(index)->getLayerCount());
    }
    for (size_t layerIndex = 0; layerIndex < maxLayers; ++layerIndex) {
        createLayerPreview(document, layerIndex, preview.parent_path() / ("layer-" + std::to_string(layerIndex) + ".pdf"));
        createLayerPreview(document, layerIndex,
                           preview.parent_path() / ("audio-layer-" + std::to_string(layerIndex) + ".pdf"), true);
    }
    prototype::bridge::writeLayerTransfer(layerState, readLayerDocument(document, layerMetadata));
    const auto mediaPath = audioDirectory.empty() ? preview.parent_path() / "audio" : audioDirectory;
    const auto targets = renderAudioTargets(document, audioFolder, mediaPath);
    prototype::writeAudioTargets(preview.parent_path() / "audio-targets.dat", targets);
}

void render(Document& document, const fs::path& output) {
    requireNewTarget(output);
    XojCairoPdfExport exporter(&document, nullptr);
    if (!exporter.createPdf(output, false)) {
        throw std::runtime_error(exporter.getLastError());
    }
}

void exportPdfDocument(Document& document, const fs::path& output) {
    if (g_ascii_strcasecmp(output.extension().string().c_str(), ".pdf") != 0) {
        throw std::runtime_error("Export target must use the .pdf extension");
    }
    const auto beforeStatus = fs::symlink_status(output);
    const bool replacing = fs::exists(beforeStatus);
    if (replacing && (fs::is_symlink(beforeStatus) || !fs::is_regular_file(beforeStatus))) {
        throw std::runtime_error("Refusing to export PDF over a symlink or non-regular file");
    }
    fs::create_directories(output.parent_path());
    prototype::bridge::SaveStaging staging(output.parent_path());
    const auto staged = staging.path / output.filename();
    XojCairoPdfExport exporter(&document, nullptr);
    if (!exporter.createPdf(staged, false)) {
        throw std::runtime_error(exporter.getLastError());
    }
    if (!fs::is_regular_file(staged) || fs::file_size(staged) == 0) {
        throw std::runtime_error("Exported PDF is missing or empty");
    }
    const auto finalStatus = fs::symlink_status(output);
    if (fs::exists(finalStatus)) {
        if (fs::is_symlink(finalStatus) || !fs::is_regular_file(finalStatus)) {
            throw std::runtime_error("Export target became a symlink or non-regular file during export");
        }
        fs::permissions(staged, fs::status(output).permissions());
    }
    fs::rename(staged, output);
}

ExportBackgroundType parseExportBackground(const std::string& background) {
    if (background == "none") {
        return EXPORT_BACKGROUND_NONE;
    }
    if (background == "unruled") {
        return EXPORT_BACKGROUND_UNRULED;
    }
    return EXPORT_BACKGROUND_ALL;
}

std::vector<fs::path> imageExportTargets(const fs::path& output, const Document& document,
                                         const std::string& requestedRange) {
    PageRangeVector range;
    if (requestedRange.empty()) {
        range.emplace_back(0, document.getPageCount() - 1);
    } else {
        range = ElementRange::parse(requestedRange, document.getPageCount());
    }
    if (range.empty()) {
        throw std::runtime_error("Image export range contains no pages");
    }

    const bool singlePage = range.size() == 1 && range.front().first == range.front().last;
    std::vector<fs::path> outputs;
    std::vector<bool> selected(document.getPageCount(), false);
    for (const auto& entry: range) {
        for (size_t page = entry.first; page <= entry.last; ++page) {
            selected[page] = true;
        }
    }
    for (size_t page = 0; page < selected.size(); ++page) {
        if (!selected[page]) {
            continue;
        }
        if (singlePage) {
            outputs.push_back(output);
        } else {
            auto numbered = output;
            const auto extension = numbered.extension();
            numbered.replace_extension();
            numbered += "-" + std::to_string(page + 1);
            numbered += extension.string();
            outputs.push_back(std::move(numbered));
        }
    }
    if (outputs.empty()) {
        throw std::runtime_error("Image export range contains no pages");
    }
    return outputs;
}

void exportDocument(Document& document, const fs::path& output, const std::string& range,
                    const std::string& layerRange, const std::string& background, bool progressiveMode,
                    const std::string& backend, int pngDpi, int pngWidth, int pngHeight) {
    const auto extension = output.extension().string();
    const bool pdf = extension == ".pdf";
    const bool svg = extension == ".svg";
    const bool png = extension == ".png";
    if (!pdf && !svg && !png) {
        return;
    }

    if (pdf && !document.getPdfFilepath().empty() && fs::exists(document.getPdfFilepath()) &&
        fs::weakly_canonical(output) == fs::weakly_canonical(document.getPdfFilepath())) {
        throw std::runtime_error("Do not overwrite the background PDF! This will cause errors!");
    }

    std::vector<fs::path> outputs{output};
    if (!pdf) {
        outputs = imageExportTargets(output, document, range);
    }
    for (const auto& target: outputs) {
        const auto status = fs::symlink_status(target);
        if (fs::exists(status) && (fs::is_symlink(status) || !fs::is_regular_file(status))) {
            throw std::runtime_error("Refusing to export over a symlink or non-regular file: " + target.string());
        }
    }

    const auto parent = output.parent_path().empty() ? fs::current_path() : output.parent_path();
    fs::create_directories(parent);
    prototype::bridge::SaveStaging staging(parent);
    const auto stagedOutput = staging.path / output.filename();
    const auto exportBackground = parseExportBackground(background);

    if (pdf) {
        const auto pdfRange = range.empty() ? nullptr : range.c_str();
        const auto selectedLayers = layerRange.empty() ? nullptr : layerRange.c_str();
        ExportHelper::exportPdf(&document, stagedOutput, pdfRange, selectedLayers, exportBackground, progressiveMode,
                                ExportBackend::fromString(backend));
    } else {
        const auto imageRange = range.empty() ? PageRangeVector{{0, document.getPageCount() - 1}} :
                                               ElementRange::parse(range, document.getPageCount());
        auto format = svg ? EXPORT_GRAPHICS_SVG : EXPORT_GRAPHICS_PNG;
        ImageExport exporter(&document, stagedOutput, format, exportBackground, imageRange);
        if (png) {
            if (pngDpi > 0) {
                exporter.setQualityParameter(EXPORT_QUALITY_DPI, pngDpi);
            } else if (pngWidth > 0) {
                exporter.setQualityParameter(EXPORT_QUALITY_WIDTH, pngWidth);
            } else if (pngHeight > 0) {
                exporter.setQualityParameter(EXPORT_QUALITY_HEIGHT, pngHeight);
            }
        }
        exporter.setLayerRange(layerRange.empty() ? nullptr : layerRange.c_str());
        DummyProgressListener progress;
        exporter.exportGraphics(&progress);
        if (!exporter.getLastErrorMsg().empty()) {
            throw std::runtime_error(exporter.getLastErrorMsg());
        }
    }

    for (const auto& target: outputs) {
        const auto stagedFile = staging.path / target.filename();
        if (!fs::is_regular_file(stagedFile) || fs::file_size(stagedFile) == 0) {
            throw std::runtime_error("Exported file is missing or empty: " + stagedFile.string());
        }
        if (prototype::bridge::existsWithoutFollowing(target)) {
            fs::permissions(stagedFile, fs::status(target).permissions());
            fs::rename(stagedFile, target);
        } else {
            fs::create_hard_link(stagedFile, target);
            fs::remove(stagedFile);
        }
    }
}

int parseExportInteger(const char* raw, const char* label) {
    const std::string_view text(raw);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error(std::string("Invalid export option: ") + label);
    }
    return value;
}

size_t parsePageIndex(std::string_view text, size_t limit, const char* label) {
    size_t parsed = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || end != text.data() + text.size() || parsed >= limit) {
        throw std::runtime_error(std::string("Invalid ") + label);
    }
    return parsed;
}

std::vector<size_t> readPageInsertionIndices(const fs::path& path, size_t originalPageCount) {
    std::error_code sizeError;
    const auto fileSize = fs::file_size(path, sizeError);
    if (sizeError || fileSize > prototype::bridge::maxPdfPageMapBytes) {
        throw std::runtime_error("Page insertion list is missing or too large");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open page insertion list");
    }

    std::string magic;
    size_t count = 0;
    input.imbue(std::locale::classic());
    if (!(input >> magic >> count) || magic != "XOURNALPP_GTK4_PAGE_INSERTIONS_V1" || count == 0 ||
        count > prototype::bridge::maxPdfPageMapPages || count > originalPageCount) {
        throw std::runtime_error("Invalid page insertion-list header");
    }

    std::vector<size_t> afterPages;
    afterPages.reserve(count);
    size_t previous = originalPageCount;
    for (size_t index = 0; index < count; ++index) {
        long long afterPage = -1;
        if (!(input >> afterPage) || afterPage < 0 || static_cast<unsigned long long>(afterPage) >= originalPageCount ||
            static_cast<size_t>(afterPage) >= previous) {
            throw std::runtime_error("Page insertion indices must be valid and strictly descending");
        }
        previous = static_cast<size_t>(afterPage);
        afterPages.push_back(previous);
    }
    input >> std::ws;
    if (!input.eof()) {
        throw std::runtime_error("Unexpected trailing page insertion-list data");
    }
    return afterPages;
}

std::vector<size_t> readPageDeletionIndices(const fs::path& path, size_t originalPageCount) {
    std::error_code sizeError;
    const auto fileSize = fs::file_size(path, sizeError);
    if (sizeError || fileSize > prototype::bridge::maxPdfPageMapBytes) {
        throw std::runtime_error("Page deletion list is missing or too large");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open page deletion list");
    }

    std::string magic;
    size_t count = 0;
    input.imbue(std::locale::classic());
    if (!(input >> magic >> count) || magic != "XOURNALPP_GTK4_PAGE_DELETIONS_V1" || count == 0 ||
        count >= originalPageCount || count > prototype::bridge::maxPdfPageMapPages) {
        throw std::runtime_error("Invalid page deletion-list header");
    }

    std::vector<size_t> pageIndices;
    pageIndices.reserve(count);
    size_t previous = originalPageCount;
    for (size_t index = 0; index < count; ++index) {
        long long page = -1;
        if (!(input >> page) || page < 0 || static_cast<unsigned long long>(page) >= originalPageCount ||
            static_cast<size_t>(page) >= previous) {
            throw std::runtime_error("Page deletion indices must be valid and strictly descending");
        }
        previous = static_cast<size_t>(page);
        pageIndices.push_back(previous);
    }
    input >> std::ws;
    if (!input.eof()) {
        throw std::runtime_error("Unexpected trailing page deletion-list data");
    }
    return pageIndices;
}

void insertPageAfterInDocument(Document& document, size_t afterPage) {
    const auto source = document.getPage(afterPage);
    if (!source) {
        throw std::runtime_error("Reference page is missing");
    }
    auto inserted = std::make_shared<XojPage>(source->getWidth(), source->getHeight());
    const auto bg = source->getBackgroundType();
    if (!bg.isPdfPage() && !bg.isImagePage()) {
        inserted->setBackgroundType(bg);
        inserted->setBackgroundColor(source->getBackgroundColor());
    } else {
        inserted->setBackgroundType(PageType(PageTypeFormat::Lined));
        inserted->setBackgroundColor(Colors::white);
    }
    document.insertPage(inserted, afterPage + 1);
}

void setPdfPageBackground(Document& document, size_t documentPageIndex, size_t pdfPageIndex) {
    if (documentPageIndex >= document.getPageCount() || pdfPageIndex >= document.getPdfPageCount()) {
        throw std::runtime_error("PDF background page selection is out of range");
    }
    const auto page = document.getPage(documentPageIndex);
    const auto pdfPage = document.getPdfPage(pdfPageIndex);
    if (!page || !pdfPage) {
        throw std::runtime_error("PDF background page selection is unavailable");
    }
    page->setBackgroundPdfPageNr(pdfPageIndex);
    page->setSize(pdfPage->getWidth(), pdfPage->getHeight());
}

std::vector<prototype::bridge::PageSizePacket> readDocumentPageSizes(const Document& document) {
    std::vector<prototype::bridge::PageSizePacket> sizes;
    sizes.reserve(document.getPageCount());
    for (size_t index = 0; index < document.getPageCount(); ++index) {
        const auto page = document.getPage(index);
        if (!page) {
            throw std::runtime_error("Document contains a missing page");
        }
        sizes.push_back({page->getWidth(), page->getHeight()});
    }
    return sizes;
}

void applyPageSizes(Document& document, const fs::path& path) {
    const auto sizes = prototype::bridge::readPageSizeTransfer(path);
    if (sizes.size() != document.getPageCount()) {
        throw std::runtime_error("Transferred page-size count does not match the document");
    }
    for (size_t index = 0; index < sizes.size(); ++index) {
        Document::setPageSize(document.getPage(index), sizes[index].width, sizes[index].height);
    }
}

void replaceFollowingPageWithClone(Document& document, size_t sourcePageIndex, size_t pdfPageIndex) {
    if (sourcePageIndex + 1 >= document.getPageCount() || pdfPageIndex >= document.getPdfPageCount()) {
        throw std::runtime_error("LayerActions clone target is out of range");
    }
    const auto source = document.getPage(sourcePageIndex);
    const auto replaced = document.getPage(sourcePageIndex + 1);
    const auto pdfPage = document.getPdfPage(pdfPageIndex);
    if (!source || !replaced || !pdfPage || !replaced->getBackgroundType().isPdfPage() ||
        replaced->getPdfPageNr() != pdfPageIndex) {
        throw std::runtime_error("The next page no longer has the requested PDF background");
    }

    auto clone = std::make_shared<XojPage>(*source);
    clone->setBackgroundPdfPageNr(pdfPageIndex);
    clone->setSize(pdfPage->getWidth(), pdfPage->getHeight());
    document.insertPage(clone, sourcePageIndex + 1);
    document.deletePage(sourcePageIndex + 2);
}

void saveDocument(Document& document, const fs::path& output, const std::string& expected, bool copy) {
    if (copy) {
        requireNewTarget(output);
    }
    prototype::bridge::requireVersion(output, expected);
    prototype::bridge::SaveStaging staging(output.parent_path());
    const auto staged = staging.path / output.filename();
    document.setFilepath(staged);
    updatePreview(document);
    SaveHandler saver;
    // Normalize external asset paths relative to the FINAL destination, while
    // attached assets are written alongside the staged document.
    saver.prepareSave(&document, output);
    if (!saver.getErrorMessage().empty()) {
        throw std::runtime_error(saver.getErrorMessage());
    }
    saver.saveTo(staged);
    if (!saver.getErrorMessage().empty()) {
        throw std::runtime_error(saver.getErrorMessage());
    }
    prototype::bridge::publishDocument(staging, output, expected);
}

void saveCopy(Document& document, const fs::path& output) {
    saveDocument(document, output, prototype::bridge::documentVersion(output).token, true);
}

void autosaveDocument(Document& document, const fs::path& output) {
    if (!prototype::bridge::isRecoveryArtifact(output)) {
        throw std::runtime_error("Autosave target must use an .autosave.xopp or emergencysave.xopp filename");
    }
    fs::create_directories(output.parent_path());
    prototype::bridge::SaveStaging staging(output.parent_path());
    const auto staged = staging.path / output.filename();
    document.setFilepath(staged);
    updatePreview(document);
    SaveHandler saver;
    saver.prepareSave(&document, output);
    if (!saver.getErrorMessage().empty()) {
        throw std::runtime_error(saver.getErrorMessage());
    }
    saver.saveTo(staged);
    if (!saver.getErrorMessage().empty()) {
        throw std::runtime_error(saver.getErrorMessage());
    }
    prototype::bridge::publishAutosave(staging, output);
}

void inspect(const Document& document) {
    std::cout << "{\"pages\":[";
    for (size_t i = 0; i < document.getPageCount(); ++i) {
        const auto page = document.getPage(i);
        size_t strokes = 0, texts = 0, images = 0;
        for (const auto* layer: page->getLayersView()) {
            for (const auto* element: layer->getElementsView()) {
                strokes += element->getType() == ELEMENT_STROKE;
                texts += element->getType() == ELEMENT_TEXT;
                images += element->getType() == ELEMENT_IMAGE;
            }
        }
        if (i) {
            std::cout << ',';
        }
        std::cout << "{\"width\":" << page->getWidth() << ",\"height\":" << page->getHeight()
                  << ",\"layers\":" << page->getLayerCount() << ",\"strokes\":" << strokes << ",\"texts\":" << texts
                  << ",\"images\":" << images << '}';
    }
    std::cout << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::setlocale(LC_NUMERIC, "C");
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(17);
    try {
        if (argc < 3) {
            throw std::runtime_error(
                    "Usage: xournalpp-document-worker inspect INPUT | render INPUT PDF | "
                    "export-document INPUT OUTPUT INK TEXT LAYERS IMAGES PAGE_SIZES RANGE LAYER_RANGE BACKGROUND "
                    "PROGRESSIVE BACKEND PNG_DPI PNG_WIDTH PNG_HEIGHT | "
                    "export-pdf INPUT PDF INK [TEXT [LAYERS [IMAGES [PAGE_SIZES]]]] | "
                    "open INPUT SNAPSHOT PDF [EXTRACTED_INK [EXTRACTED_TEXT [LAYERS [IMAGES [AUDIO_FOLDER "
                    "[AUDIO_CACHE]]]]]] | "
                    "insert-page INPUT SNAPSHOT PDF AFTER_PAGE [AUDIO_FOLDER [AUDIO_CACHE]] | "
                    "insert-pages INPUT SNAPSHOT PDF INSERTION_LIST [AUDIO_FOLDER AUDIO_CACHE] | "
                    "delete-page INPUT SNAPSHOT PDF PAGE_INDEX [AUDIO_FOLDER [AUDIO_CACHE]] | "
                    "delete-pages INPUT SNAPSHOT PDF DELETION_LIST [AUDIO_FOLDER AUDIO_CACHE] | "
                    "set-pdf-page-background INPUT SNAPSHOT PDF DOCUMENT_PAGE PDF_PAGE INK TEXT LAYERS IMAGES "
                    "[AUDIO_FOLDER AUDIO_CACHE] | "
                    "layer-actions-inspect-target INPUT CURRENT_PAGE INK TEXT LAYERS IMAGES | "
                    "layer-actions-clone-next INPUT SNAPSHOT PDF CURRENT_PAGE PDF_PAGE ALLOW_ANNOTATED INK TEXT "
                    "LAYERS IMAGES [AUDIO_FOLDER AUDIO_CACHE] | "
                    "revision INPUT SNAPSHOT PDF INK TEXT LAYERS IMAGES [AUDIO_FOLDER [AUDIO_CACHE]] | "
                    "save-copy INPUT XOPP INK [TEXT [LAYERS [IMAGES [PAGE_SIZES]]]] | "
                    "save INPUT XOPP INK VERSION [TEXT [LAYERS [IMAGES [PAGE_SIZES]]]] | "
                    "autosave INPUT AUTOSAVE_XOPP INK [TEXT [LAYERS [IMAGES [PAGE_SIZES]]]] | version PATH");
        }
        const std::string command = argv[1];
        LoadContext originalContext, snapshotContext;
        const auto input = std::string(argv[2]).empty() ? fs::path{} : fs::absolute(argv[2]);
        if (command == "version" && argc == 3) {
            std::cout << prototype::bridge::documentVersion(input).token << '\n';
            return 0;
        }
        if (command == "clipboard-roundtrip" && argc == 4) {
            std::ifstream file(input, std::ios::binary | std::ios::ate);
            if (!file || file.tellg() < 0 || static_cast<size_t>(file.tellg()) > prototype::clipboard::maxBytes)
                throw std::runtime_error("Invalid clipboard input size");
            const auto length = static_cast<size_t>(file.tellg());
            file.seekg(0);
            std::string bytes(length, '\0');
            if (!file.read(bytes.data(), static_cast<std::streamsize>(length)))
                throw std::runtime_error("Cannot read clipboard input");
            // The legacy reader is only invoked after strict structural and
            // allocation-limit preflight. Exercise the actual production codec.
            prototype::clipboard::decode(bytes);
            ObjectInputStream in;
            if (!in.read(bytes.data(), bytes.size())) throw std::runtime_error("Native clipboard header rejected");
            ObjectOutputStream out(new BinObjectEncoding());
            out.writeString(in.readString());
            in.readObject("EditSelection"); out.writeObject("EditSelection");
            for (int n = 0; n < 9; ++n) out.writeDouble(in.readDouble());
            in.readObject("EditSelectionContents"); out.writeObject("EditSelectionContents");
            for (int n = 0; n < 11; ++n) out.writeDouble(in.readDouble());
            in.endObject(); out.endObject();
            in.endObject(); out.endObject();
            const auto count = in.readInt(); out.writeInt(count);
            for (int n = 0; n < count; ++n) {
                Stroke stroke;
                stroke.readSerialized(in);
                stroke.serialize(out);
            }
            auto* encoded = out.stealData();
            std::ofstream output(argv[3], std::ios::binary);
            output.write(encoded->str, static_cast<std::streamsize>(encoded->len));
            g_string_free(encoded, true);
            output.close();
            if (!output) throw std::runtime_error("Cannot write clipboard output");
            return 0;
        }
        auto document = load(input, originalContext);
        if (command == "layer-actions-inspect-target" && argc == 8) {
            const auto currentPage = parsePageIndex(argv[3], document->getPageCount(), "current page index");
            const auto strokes = prototype::bridge::readTransfer(fs::absolute(argv[4]));
            const auto texts = prototype::bridge::readTextTransfer(fs::absolute(argv[5]));
            const auto layers = prototype::bridge::readLayerTransfer(fs::absolute(argv[6]));
            const auto images = prototype::bridge::readImageTransfer(fs::absolute(argv[7]));
            discardEditableObjects(*document);
            appendLayerAwareObjects(*document, strokes, texts, images, layers);

            int pdfPage = -1;
            bool annotated = false;
            const bool hasNextPage = currentPage + 1 < document->getPageCount();
            if (hasNextPage) {
                const auto next = document->getPage(currentPage + 1);
                if (!next) {
                    throw std::runtime_error("The next LayerActions page is unavailable");
                }
                if (next->getBackgroundType().isPdfPage() && next->getPdfPageNr() != npos) {
                    if (next->getPdfPageNr() > static_cast<size_t>(std::numeric_limits<int>::max())) {
                        throw std::runtime_error("The next PDF page index is too large");
                    }
                    pdfPage = static_cast<int>(next->getPdfPageNr());
                }
                annotated = next->isAnnotated();
            }
            std::cout << "XOURNALPP_GTK4_LAYER_ACTION_TARGET_V1 " << (hasNextPage ? 1 : 0) << ' ' << pdfPage << ' '
                      << (annotated ? 1 : 0) << '\n';
            return 0;
        } else if (command == "inspect" && argc == 3) {
            inspect(*document);
        } else if (command == "render" && argc == 4) {
            render(*document, fs::absolute(argv[3]));
        } else if (command == "export-pdf" && (argc >= 5 && argc <= 9)) {
            const auto output = fs::absolute(argv[3]);
            const auto texts = argc >= 6 ? prototype::bridge::readTextTransfer(fs::absolute(argv[5])) :
                                           std::vector<prototype::TextItem>{};
            const auto layers = argc >= 7 ? prototype::bridge::readLayerTransfer(fs::absolute(argv[6])) :
                                            readLayerDocument(*document);
            const auto images = argc >= 8 ? prototype::bridge::readImageTransfer(fs::absolute(argv[7])) :
                                            std::vector<prototype::bridge::ImagePacket>{};
            appendLayerAwareObjects(*document, prototype::bridge::readTransfer(argv[4]), texts, images, layers);
            if (argc >= 9) {
                applyPageSizes(*document, fs::absolute(argv[8]));
            }
            exportPdfDocument(*document, output);
        } else if (command == "export-document" && argc == 17) {
            const auto output = fs::absolute(argv[3]);
            const auto texts = prototype::bridge::readTextTransfer(fs::absolute(argv[5]));
            const auto layers = prototype::bridge::readLayerTransfer(fs::absolute(argv[6]));
            const auto images = prototype::bridge::readImageTransfer(fs::absolute(argv[7]));
            appendLayerAwareObjects(*document, prototype::bridge::readTransfer(fs::absolute(argv[4])), texts, images,
                                    layers);
            applyPageSizes(*document, fs::absolute(argv[8]));
            const std::string range = argv[9];
            const std::string layerRange = argv[10];
            const bool progressiveMode = std::string_view(argv[12]) == "1";
            if (!progressiveMode && std::string_view(argv[12]) != "0") {
                throw std::runtime_error("Invalid export option: progressiveMode");
            }
            exportDocument(*document, output, range, layerRange, argv[11], progressiveMode, argv[13],
                           parseExportInteger(argv[14], "pngDpi"), parseExportInteger(argv[15], "pngWidth"),
                           parseExportInteger(argv[16], "pngHeight"));
        } else if (command == "insert-page" && (argc == 6 || argc == 7 || argc == 8 || argc == 9)) {
            const auto afterPage = parsePageIndex(argv[5], document->getPageCount(), "after-page index");
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            const bool hasAudioPaths = argc == 8 || argc == 9;
            const bool hasPageSizes = argc == 7 || argc == 9;
            if (hasPageSizes) {
                applyPageSizes(*document, fs::absolute(argv[argc == 7 ? 6 : 8]));
            }
            insertPageAfterInDocument(*document, afterPage);
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            renderPreviewBundle(*document, pdf, pdf.parent_path() / "layers.dat",
                                hasAudioPaths ? fs::path(argv[6]) : fs::path{},
                                hasAudioPaths ? fs::path(argv[7]) : fs::path{});
        } else if (command == "insert-pages" && (argc == 6 || argc == 7 || argc == 8 || argc == 9)) {
            const auto afterPages = readPageInsertionIndices(fs::absolute(argv[5]), document->getPageCount());
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            const bool hasAudioPaths = argc == 8 || argc == 9;
            const bool hasPageSizes = argc == 7 || argc == 9;
            if (hasPageSizes) {
                applyPageSizes(*document, fs::absolute(argv[argc == 7 ? 6 : 8]));
            }
            for (const auto afterPage: afterPages) {
                insertPageAfterInDocument(*document, afterPage);
            }
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            renderPreviewBundle(*document, pdf, pdf.parent_path() / "layers.dat",
                                hasAudioPaths ? fs::path(argv[6]) : fs::path{},
                                hasAudioPaths ? fs::path(argv[7]) : fs::path{});
        } else if (command == "delete-page" && (argc == 6 || argc == 7 || argc == 8 || argc == 9)) {
            if (document->getPageCount() <= 1) {
                throw std::runtime_error("Cannot delete the only remaining page");
            }
            const auto pageIndex = parsePageIndex(argv[5], document->getPageCount(), "delete-page index");
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            const bool hasAudioPaths = argc == 8 || argc == 9;
            const bool hasPageSizes = argc == 7 || argc == 9;
            if (hasPageSizes) {
                applyPageSizes(*document, fs::absolute(argv[argc == 7 ? 6 : 8]));
            }
            document->deletePage(pageIndex);
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            renderPreviewBundle(*document, pdf, pdf.parent_path() / "layers.dat",
                                hasAudioPaths ? fs::path(argv[6]) : fs::path{},
                                hasAudioPaths ? fs::path(argv[7]) : fs::path{});
        } else if (command == "delete-pages" && (argc == 6 || argc == 7 || argc == 8 || argc == 9)) {
            const auto pageIndices = readPageDeletionIndices(fs::absolute(argv[5]), document->getPageCount());
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            const bool hasAudioPaths = argc == 8 || argc == 9;
            const bool hasPageSizes = argc == 7 || argc == 9;
            if (hasPageSizes) {
                applyPageSizes(*document, fs::absolute(argv[argc == 7 ? 6 : 8]));
            }
            for (const auto pageIndex: pageIndices) {
                document->deletePage(pageIndex);
            }
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            renderPreviewBundle(*document, pdf, pdf.parent_path() / "layers.dat",
                                hasAudioPaths ? fs::path(argv[6]) : fs::path{},
                                hasAudioPaths ? fs::path(argv[7]) : fs::path{});
        } else if (command == "set-pdf-page-background" && (argc == 11 || argc == 12 || argc == 13 || argc == 14)) {
            const auto documentPage = parsePageIndex(argv[5], document->getPageCount(), "document page index");
            const auto pdfPage = parsePageIndex(argv[6], document->getPdfPageCount(), "PDF page index");
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            const auto strokes = prototype::bridge::readTransfer(fs::absolute(argv[7]));
            const auto texts = prototype::bridge::readTextTransfer(fs::absolute(argv[8]));
            const auto layers = prototype::bridge::readLayerTransfer(fs::absolute(argv[9]));
            const auto images = prototype::bridge::readImageTransfer(fs::absolute(argv[10]));
            const bool hasAudioPaths = argc == 13 || argc == 14;
            const bool hasPageSizes = argc == 12 || argc == 14;
            if (hasPageSizes) {
                applyPageSizes(*document, fs::absolute(argv[hasAudioPaths ? 13 : 11]));
            }
            appendLayerAwareObjects(*document, strokes, texts, images, layers);
            setPdfPageBackground(*document, documentPage, pdfPage);
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            renderPreviewBundle(*document, pdf, pdf.parent_path() / "layers.dat",
                                hasAudioPaths ? fs::path(argv[11]) : fs::path{},
                                hasAudioPaths ? fs::path(argv[12]) : fs::path{}, &layers);
        } else if (command == "layer-actions-clone-next" &&
                   (argc == 12 || argc == 13 || argc == 14 || argc == 15)) {
            const auto sourcePage = parsePageIndex(argv[5], document->getPageCount(), "source page index");
            const auto pdfPage = parsePageIndex(argv[6], document->getPdfPageCount(), "next PDF page index");
            if (std::string_view(argv[7]) != "0" && std::string_view(argv[7]) != "1") {
                throw std::runtime_error("Invalid LayerActions annotation confirmation");
            }
            const bool allowAnnotated = std::string_view(argv[7]) == "1";
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            const auto inkPath = fs::absolute(snapshot.parent_path() / "editable-ink.dat");
            const auto textPath = fs::absolute(snapshot.parent_path() / "editable-text.dat");
            const auto imagePath = fs::absolute(snapshot.parent_path() / "editable-images.dat");
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            requireNewTarget(inkPath);
            requireNewTarget(textPath);
            requireNewTarget(imagePath);
            const auto strokes = prototype::bridge::readTransfer(fs::absolute(argv[8]));
            const auto texts = prototype::bridge::readTextTransfer(fs::absolute(argv[9]));
            const auto layers = prototype::bridge::readLayerTransfer(fs::absolute(argv[10]));
            const auto images = prototype::bridge::readImageTransfer(fs::absolute(argv[11]));
            const bool hasAudioPaths = argc == 14 || argc == 15;
            const bool hasPageSizes = argc == 13 || argc == 15;
            if (hasPageSizes) {
                applyPageSizes(*document, fs::absolute(argv[hasAudioPaths ? 14 : 12]));
            }
            discardEditableObjects(*document);
            appendLayerAwareObjects(*document, strokes, texts, images, layers);
            if (sourcePage + 1 >= document->getPageCount()) {
                throw std::runtime_error("No next page");
            }
            const auto next = document->getPage(sourcePage + 1);
            if (!next || !next->getBackgroundType().isPdfPage() || next->getPdfPageNr() != pdfPage) {
                throw std::runtime_error("The next page has no PDF background");
            }
            if (next->isAnnotated() && !allowAnnotated) {
                throw std::runtime_error("The next page contains annotations and requires confirmation");
            }

            replaceFollowingPageWithClone(*document, sourcePage, pdfPage);
            std::vector<prototype::TextItem> editableTexts;
            std::vector<prototype::bridge::ImagePacket> editableImages;
            const auto editableInk = extractEditableInk(*document, &editableTexts, &editableImages);
            prototype::bridge::writeTransfer(inkPath, editableInk);
            prototype::bridge::writeTextTransfer(textPath, editableTexts);
            prototype::bridge::writeImageTransfer(imagePath, editableImages);
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            const auto layerStatePath = pdf.parent_path() / "layers.dat";
            renderPreviewBundle(*document, pdf, layerStatePath,
                                hasAudioPaths ? fs::path(argv[12]) : fs::path{},
                                hasAudioPaths ? fs::path(argv[13]) : fs::path{}, &layers);
            auto clonedLayers = prototype::bridge::readLayerTransfer(layerStatePath);
            if (clonedLayers.size() != layers.size()) {
                throw std::runtime_error("LayerActions clone output does not match its source layers");
            }
            for (size_t page = 0; page < clonedLayers.size(); ++page) {
                const bool isClonePage = page == sourcePage + 1;
                const auto& sourceState = layers[isClonePage ? sourcePage : page];
                if (clonedLayers[page].layers.size() != sourceState.layers.size()) {
                    throw std::runtime_error("LayerActions clone output has an unexpected layer count");
                }
                clonedLayers[page].backgroundVisible = isClonePage ? true : sourceState.backgroundVisible;
                for (size_t layer = 0; layer < clonedLayers[page].layers.size(); ++layer) {
                    // XojPage copy clones layer names and contents but resets visibility to true.
                    clonedLayers[page].layers[layer].visible =
                            isClonePage ? true : sourceState.layers[layer].visible;
                    clonedLayers[page].layers[layer].hasNonEditableElements =
                            sourceState.layers[layer].hasNonEditableElements;
                }
            }
            prototype::bridge::writeLayerTransfer(layerStatePath, clonedLayers);
        } else if (command == "revision" && (argc == 9 || argc == 10 || argc == 11 || argc == 12)) {
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            const auto strokes = prototype::bridge::readTransfer(fs::absolute(argv[5]));
            const auto texts = prototype::bridge::readTextTransfer(fs::absolute(argv[6]));
            const auto layers = prototype::bridge::readLayerTransfer(fs::absolute(argv[7]));
            const auto images = prototype::bridge::readImageTransfer(fs::absolute(argv[8]));
            appendLayerAwareObjects(*document, strokes, texts, images, layers);
            if (argc == 10) {
                applyPageSizes(*document, fs::absolute(argv[9]));
            } else if (argc == 12) {
                applyPageSizes(*document, fs::absolute(argv[11]));
            }
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            renderPreviewBundle(*document, pdf, pdf.parent_path() / "layers.dat",
                                argc >= 11 ? fs::path(argv[9]) : fs::path{},
                                argc >= 11 ? fs::path(argv[10]) : fs::path{}, &layers);
        } else if (command == "open" && (argc >= 5 && argc <= 11)) {
            const auto pdfBackgroundFilename = document->getPdfFilepath().string();
            const bool nativeSave = document->getFilepath() == input && !input.empty() &&
                                    !prototype::bridge::isRecoveryArtifact(input) &&
                                    g_ascii_strcasecmp(input.extension().string().c_str(), ".xopp") == 0;
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            if (argc >= 6) {
                const auto extractedPath = fs::absolute(argv[5]);
                requireNewTarget(extractedPath);
                std::vector<prototype::TextItem> extractedTexts;
                std::vector<prototype::bridge::ImagePacket> extractedImages;
                const auto extracted = extractEditableInk(*document, argc >= 7 ? &extractedTexts : nullptr,
                                                          argc >= 9 ? &extractedImages : nullptr);
                prototype::bridge::writeTransfer(extractedPath, extracted);
                if (argc >= 7) {
                    const auto extractedTextPath = fs::absolute(argv[6]);
                    requireNewTarget(extractedTextPath);
                    prototype::bridge::writeTextTransfer(extractedTextPath, extractedTexts);
                }
                if (argc >= 9) {
                    const auto extractedImagePath = fs::absolute(argv[8]);
                    requireNewTarget(extractedImagePath);
                    prototype::bridge::writeImageTransfer(extractedImagePath, extractedImages);
                }
            }
            saveCopy(*document, snapshot);
            prototype::bridge::writeDocumentPathTransfer(snapshot.parent_path() / "pdf-background-path.dat",
                                                          pdfBackgroundFilename);
            document = load(snapshot, snapshotContext);
            if (argc >= 8) {
                renderPreviewBundle(*document, pdf, fs::absolute(argv[7]),
                                    argc >= 10 ? fs::path(argv[9]) : fs::path{},
                                    argc >= 11 ? fs::path(argv[10]) : fs::path{});
            } else {
                render(*document, pdf);
            }
            std::cout << (nativeSave ? "NATIVE_SAVE\n" : "SAVE_AS\n");
        } else if (command == "save-copy" && (argc >= 5 && argc <= 9)) {
            if (g_ascii_strcasecmp(fs::path(argv[3]).extension().string().c_str(), ".xopp") != 0) {
                throw std::runtime_error("Save-copy target must use the .xopp extension");
            }
            const auto texts = argc >= 6 ? prototype::bridge::readTextTransfer(fs::absolute(argv[5])) :
                                           std::vector<prototype::TextItem>{};
            const auto layers = argc >= 7 ? prototype::bridge::readLayerTransfer(fs::absolute(argv[6])) :
                                            readLayerDocument(*document);
            const auto images = argc >= 8 ? prototype::bridge::readImageTransfer(fs::absolute(argv[7])) :
                                            std::vector<prototype::bridge::ImagePacket>{};
            appendLayerAwareObjects(*document, prototype::bridge::readTransfer(argv[4]), texts, images, layers);
            if (argc >= 9) {
                applyPageSizes(*document, fs::absolute(argv[8]));
            }
            saveCopy(*document, fs::absolute(argv[3]));
        } else if (command == "save" && (argc >= 6 && argc <= 10)) {
            if (g_ascii_strcasecmp(fs::path(argv[3]).extension().string().c_str(), ".xopp") != 0) {
                throw std::runtime_error("Save target must use the .xopp extension");
            }
            const auto texts = argc >= 7 ? prototype::bridge::readTextTransfer(fs::absolute(argv[6])) :
                                           std::vector<prototype::TextItem>{};
            const auto layers = argc >= 8 ? prototype::bridge::readLayerTransfer(fs::absolute(argv[7])) :
                                            readLayerDocument(*document);
            const auto images = argc >= 9 ? prototype::bridge::readImageTransfer(fs::absolute(argv[8])) :
                                            std::vector<prototype::bridge::ImagePacket>{};
            appendLayerAwareObjects(*document, prototype::bridge::readTransfer(argv[4]), texts, images, layers);
            if (argc >= 10) {
                applyPageSizes(*document, fs::absolute(argv[9]));
            }
            const auto output = fs::absolute(argv[3]);
            saveDocument(*document, output, argv[5], false);
            std::cout << prototype::bridge::documentVersion(output).token << '\n';
        } else if (command == "autosave" && (argc >= 5 && argc <= 9)) {
            const auto output = fs::absolute(argv[3]);
            if (!prototype::bridge::isRecoveryArtifact(output)) {
                throw std::runtime_error("Autosave target must use an .autosave.xopp or emergencysave.xopp filename");
            }
            const auto texts = argc >= 6 ? prototype::bridge::readTextTransfer(fs::absolute(argv[5])) :
                                           std::vector<prototype::TextItem>{};
            const auto layers = argc >= 7 ? prototype::bridge::readLayerTransfer(fs::absolute(argv[6])) :
                                            readLayerDocument(*document);
            const auto images = argc >= 8 ? prototype::bridge::readImageTransfer(fs::absolute(argv[7])) :
                                            std::vector<prototype::bridge::ImagePacket>{};
            appendLayerAwareObjects(*document, prototype::bridge::readTransfer(argv[4]), texts, images, layers);
            if (argc >= 9) {
                applyPageSizes(*document, fs::absolute(argv[8]));
            }
            autosaveDocument(*document, output);
        } else {
            throw std::runtime_error("Invalid document-worker command");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
