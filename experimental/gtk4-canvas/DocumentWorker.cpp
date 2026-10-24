#include <algorithm>
#include <charconv>
#include <cmath>
#include <clocale>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <glib.h>

#include "control/xojfile/LoadHandler.h"
#include "control/xojfile/SaveHandler.h"
#include "model/Document.h"
#include "model/DocumentHandler.h"
#include "model/Layer.h"
#include "model/Point.h"
#include "model/Stroke.h"
#include "model/XojPage.h"
#include "pdf/base/XojCairoPdfExport.h"
#include "util/raii/CairoWrappers.h"
#include "view/DocumentView.h"

#include "DocumentAutosave.h"
#include "DocumentPublish.h"
#include "DocumentTransfer.h"

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

void appendInk(Document& document, const std::vector<prototype::bridge::StrokePacket>& packets) {
    for (const auto& packet: packets) {
        if (static_cast<size_t>(packet.page) >= document.getPageCount()) {
            throw std::runtime_error("Transferred stroke refers to a missing page");
        }
        auto page = document.getPage(static_cast<size_t>(packet.page));
        if (!page->getLayerCount()) {
            throw std::runtime_error("Cannot append to a document page without an ink layer");
        }
        auto stroke = std::make_unique<Stroke>();
        stroke->setToolType(packet.whiteout ? StrokeTool::ERASER :
                                              (packet.highlighter ? StrokeTool::HIGHLIGHTER : StrokeTool::PEN));
        stroke->setWidth(packet.width);
        if (packet.whiteout) {
            stroke->setColor(Color{255, 255, 255});
        } else {
            stroke->setColor(Color{static_cast<uint8_t>(std::lround(packet.color[0] * 255)),
                                   static_cast<uint8_t>(std::lround(packet.color[1] * 255)),
                                   static_cast<uint8_t>(std::lround(packet.color[2] * 255))});
        }
        for (const auto& point: packet.points) {
            if (point.x < 0 || point.y < 0 || point.x > page->getWidth() || point.y > page->getHeight()) {
                throw std::runtime_error("Transferred stroke lies outside its page");
            }
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
        // Existing layers and objects stay intact; the prototype has no layer picker yet.
        page->getLayers().back()->addElement(std::move(stroke));
    }
}

std::optional<prototype::bridge::StrokePacket> toEditablePacket(const Element& element, const XojPage& page,
                                                                int pageIndex) {
    if (element.getType() != ELEMENT_STROKE) {
        return std::nullopt;
    }
    const auto* stroke = dynamic_cast<const Stroke*>(&element);
    if (!stroke) {
        return std::nullopt;
    }
    const auto& pts = stroke->getPointVector();
    const double width = stroke->getWidth();
    if (pts.size() < 2 || !std::isfinite(width) || width <= 0.0 || stroke->getFill() != -1 ||
        stroke->getStrokeCapStyle() != StrokeCapStyle::ROUND || stroke->getLineStyle().hasDashes() ||
        !stroke->getAudioFilename().empty()) {
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
        if (rgb != 0xffffffU || stroke->hasPressure()) {
            return std::nullopt;
        }
        whiteout = true;
    } else {
        return std::nullopt;
    }

    prototype::bridge::StrokePacket packet;
    packet.page = pageIndex;
    packet.highlighter = highlighter;
    packet.whiteout = whiteout;
    packet.pressureSensitive = pressureSensitive;
    packet.width = width;
    packet.color = {static_cast<double>((rgb >> 16U) & 0xffU) / 255.0,
                    static_cast<double>((rgb >> 8U) & 0xffU) / 255.0,
                    static_cast<double>(rgb & 0xffU) / 255.0};
    packet.points.reserve(pts.size());
    const double pageWidth = page.getWidth();
    const double pageHeight = page.getHeight();
    for (size_t i = 0; i < pts.size(); ++i) {
        const auto& point = pts[i];
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0.0 || point.y < 0.0 ||
            point.x > pageWidth || point.y > pageHeight) {
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

std::vector<prototype::bridge::StrokePacket> extractEditableInk(Document& document) {
    std::vector<prototype::bridge::StrokePacket> extracted;
    size_t totalPoints = 0;
    for (size_t pageIndex = 0; pageIndex < document.getPageCount(); ++pageIndex) {
        auto page = document.getPage(pageIndex);
        if (!page->getLayerCount()) {
            continue;
        }
        auto* topLayer = page->getLayers().back();
        if (!topLayer->isVisible()) {
            continue;
        }
        auto& elements = topLayer->getElements();
        size_t suffixStart = elements.size();
        std::vector<prototype::bridge::StrokePacket> pagePackets;
        while (suffixStart > 0) {
            auto candidate = toEditablePacket(*elements[suffixStart - 1], *page, static_cast<int>(pageIndex));
            if (!candidate || candidate->points.size() > prototype::bridge::maxTransferPoints - totalPoints) {
                break;
            }
            totalPoints += candidate->points.size();
            pagePackets.push_back(std::move(*candidate));
            --suffixStart;
        }
        if (suffixStart < elements.size()) {
            elements.erase(elements.begin() + static_cast<std::ptrdiff_t>(suffixStart), elements.end());
            extracted.insert(extracted.end(), std::make_move_iterator(pagePackets.rbegin()),
                             std::make_move_iterator(pagePackets.rend()));
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

size_t parsePageIndex(std::string_view text, size_t limit, const char* label) {
    size_t parsed = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || end != text.data() + text.size() || parsed >= limit) {
        throw std::runtime_error(std::string("Invalid ") + label);
    }
    return parsed;
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
                    "export-pdf INPUT PDF INK | open INPUT SNAPSHOT PDF [EXTRACTED_INK] | "
                    "insert-page INPUT SNAPSHOT PDF AFTER_PAGE | delete-page INPUT SNAPSHOT PDF PAGE_INDEX | "
                    "save-copy INPUT XOPP INK | save INPUT XOPP INK VERSION | autosave INPUT AUTOSAVE_XOPP INK | "
                    "version PATH");
        }
        const std::string command = argv[1];
        LoadContext originalContext, snapshotContext;
        const auto input = std::string(argv[2]).empty() ? fs::path{} : fs::absolute(argv[2]);
        if (command == "version" && argc == 3) {
            std::cout << prototype::bridge::documentVersion(input).token << '\n';
            return 0;
        }
        auto document = load(input, originalContext);
        if (command == "inspect" && argc == 3) {
            inspect(*document);
        } else if (command == "render" && argc == 4) {
            render(*document, fs::absolute(argv[3]));
        } else if (command == "export-pdf" && argc == 5) {
            const auto output = fs::absolute(argv[3]);
            appendInk(*document, prototype::bridge::readTransfer(argv[4]));
            exportPdfDocument(*document, output);
        } else if (command == "insert-page" && argc == 6) {
            const auto afterPage = parsePageIndex(argv[5], document->getPageCount(), "after-page index");
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            insertPageAfterInDocument(*document, afterPage);
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            render(*document, pdf);
        } else if (command == "delete-page" && argc == 6) {
            if (document->getPageCount() <= 1) {
                throw std::runtime_error("Cannot delete the only remaining page");
            }
            const auto pageIndex = parsePageIndex(argv[5], document->getPageCount(), "delete-page index");
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            document->deletePage(pageIndex);
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            render(*document, pdf);
        } else if (command == "open" && (argc == 5 || argc == 6)) {
            const bool nativeSave = document->getFilepath() == input && !input.empty() &&
                                    !prototype::bridge::isRecoveryArtifact(input) &&
                                    g_ascii_strcasecmp(input.extension().string().c_str(), ".xopp") == 0;
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            if (argc == 6) {
                const auto extractedPath = fs::absolute(argv[5]);
                requireNewTarget(extractedPath);
                const auto extracted = extractEditableInk(*document);
                prototype::bridge::writeTransfer(extractedPath, extracted);
            }
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            render(*document, pdf);
            std::cout << (nativeSave ? "NATIVE_SAVE\n" : "SAVE_AS\n");
        } else if (command == "save-copy" && argc == 5) {
            if (g_ascii_strcasecmp(fs::path(argv[3]).extension().string().c_str(), ".xopp") != 0) {
                throw std::runtime_error("Save-copy target must use the .xopp extension");
            }
            appendInk(*document, prototype::bridge::readTransfer(argv[4]));
            saveCopy(*document, fs::absolute(argv[3]));
        } else if (command == "save" && argc == 6) {
            if (g_ascii_strcasecmp(fs::path(argv[3]).extension().string().c_str(), ".xopp") != 0) {
                throw std::runtime_error("Save target must use the .xopp extension");
            }
            appendInk(*document, prototype::bridge::readTransfer(argv[4]));
            const auto output = fs::absolute(argv[3]);
            saveDocument(*document, output, argv[5], false);
            std::cout << prototype::bridge::documentVersion(output).token << '\n';
        } else if (command == "autosave" && argc == 5) {
            const auto output = fs::absolute(argv[3]);
            if (!prototype::bridge::isRecoveryArtifact(output)) {
                throw std::runtime_error("Autosave target must use an .autosave.xopp or emergencysave.xopp filename");
            }
            appendInk(*document, prototype::bridge::readTransfer(argv[4]));
            autosaveDocument(*document, output);
        } else {
            throw std::runtime_error("Invalid document-worker command");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
