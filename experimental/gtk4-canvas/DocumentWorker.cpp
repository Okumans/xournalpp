#include <algorithm>
#include <clocale>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

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

#include "DocumentTransfer.h"

namespace {

namespace fs = std::filesystem;

class StagingDirectory {
public:
    explicit StagingDirectory(const fs::path& parent) {
        std::string pattern = (parent / ".xournalpp-gtk4-save-XXXXXX").string();
        if (!g_mkdtemp(pattern.data())) {
            throw std::runtime_error("Cannot create private save staging directory");
        }
        path = pattern;
    }
    ~StagingDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

struct LoadContext {
    DocumentHandler handler;
    std::vector<std::string> warnings;
    LoadHandler loader{&warnings};
};

std::unique_ptr<Document> load(const fs::path& input, LoadContext& context) {
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
        stroke->setToolType(packet.highlighter ? StrokeTool::HIGHLIGHTER : StrokeTool::PEN);
        stroke->setWidth(packet.width);
        stroke->setColor(Color{static_cast<uint8_t>(std::lround(packet.color[0] * 255)),
                               static_cast<uint8_t>(std::lround(packet.color[1] * 255)),
                               static_cast<uint8_t>(std::lround(packet.color[2] * 255))});
        for (const auto& point: packet.points) {
            if (point.x < 0 || point.y < 0 || point.x > page->getWidth() || point.y > page->getHeight()) {
                throw std::runtime_error("Transferred stroke lies outside its page");
            }
            if (packet.pressureSensitive && !packet.highlighter) {
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

void saveCopy(Document& document, const fs::path& output) {
    if (fs::exists(fs::symlink_status(output))) {
        throw std::runtime_error("Save-copy target already exists; choose a new filename");
    }
    StagingDirectory staging(output.parent_path());
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
    std::vector<fs::path> published;
    try {
        for (const auto& file: fs::directory_iterator(staging.path)) {
            if (file.path() != staged) {
                const auto destination = output.parent_path() / file.path().filename();
                fs::create_hard_link(file.path(), destination);
                published.push_back(destination);
            }
        }
        // Publish the complete document last, without overwriting any existing file.
        fs::create_hard_link(staged, output);
    } catch (...) {
        for (const auto& path: published) {
            std::error_code error;
            fs::remove(path, error);
        }
        throw;
    }
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
            throw std::runtime_error("Usage: xournalpp-document-worker inspect INPUT | render INPUT PDF | open INPUT "
                                     "SNAPSHOT PDF | save-copy INPUT XOPP INK");
        }
        const std::string command = argv[1];
        LoadContext originalContext, snapshotContext;
        auto document = load(fs::absolute(argv[2]), originalContext);
        if (command == "inspect" && argc == 3) {
            inspect(*document);
        } else if (command == "render" && argc == 4) {
            render(*document, fs::absolute(argv[3]));
        } else if (command == "open" && argc == 5) {
            const auto snapshot = fs::absolute(argv[3]);
            const auto pdf = fs::absolute(argv[4]);
            requireNewTarget(snapshot);
            requireNewTarget(pdf);
            saveCopy(*document, snapshot);
            document = load(snapshot, snapshotContext);
            render(*document, pdf);
        } else if (command == "save-copy" && argc == 5) {
            if (g_ascii_strcasecmp(fs::path(argv[3]).extension().string().c_str(), ".xopp") != 0) {
                throw std::runtime_error("Save-copy target must use the .xopp extension");
            }
            appendInk(*document, prototype::bridge::readTransfer(argv[4]));
            saveCopy(*document, fs::absolute(argv[3]));
        } else {
            throw std::runtime_error("Invalid document-worker command");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
