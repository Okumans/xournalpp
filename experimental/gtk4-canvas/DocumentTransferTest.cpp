#include <chrono>
#include <iostream>

#include "DocumentInk.h"
#include "PageSizeTransfer.h"

using namespace prototype::bridge;

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("gtk4-ink-transfer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        prototype::Ink mouse{1, {{1, 2, 1}, {3, 4, 1}}, {prototype::Tool::Pen, {1, 0, 0}, 3}};
        mouse.layerOrder = 11;
        mouse.layerId = 2;
        mouse.originLayer = 7;
        auto stylus = mouse;
        stylus.pressureSensitive = true;
        auto highlighter = stylus;
        highlighter.brush.tool = prototype::Tool::Highlighter;
        auto pdfHighlight = highlighter;
        pdfHighlight.pressureSensitive = false;
        pdfHighlight.fill = 60;
        pdfHighlight.capStyle = 1;
        prototype::Ink whiteout{0, {{10, 15, 0.3}, {50, 15, 0.9}}, {prototype::Tool::Eraser, {0.2, 0.3, 0.4}, 8.5}, true};
        const auto packets = transferInk({mouse, stylus, highlighter, whiteout, pdfHighlight});
        if (packets[0].pressureSensitive || !packets[1].pressureSensitive || !packets[2].highlighter ||
            !packets[3].whiteout || packets[3].highlighter || packets[3].pressureSensitive ||
            packets[3].color != prototype::behavior::whiteoutColor ||
            packets[1].points[1].x != 3 || packets[0].page != 1 || packets[0].layerOrder != 11 ||
            packets[0].layerId != 2 || packets[0].originLayer != 7 || packets[4].fill != 60 ||
            packets[4].capStyle != 1) {
            throw std::runtime_error("Session ink transfer changed tool, pressure mode or page coordinates");
        }
        StrokePacket original{2, false, true, 4.25, {0.1, 0.2, 0.9}, {{12.34, 56.78, 0.25}, {80, 90, 0.8}}};
        original.layerOrder = 19;
        original.layerId = 3;
        original.originLayer = 12;
        const StrokePacket whiteoutPacket{0, false, false, 8.5, {1, 1, 1}, {{10, 20, 1}, {40, 20, 1}}, true};
        auto annotatedPacket = original;
        annotatedPacket.highlighter = true;
        annotatedPacket.pressureSensitive = false;
        annotatedPacket.fill = 230;
        annotatedPacket.capStyle = 1;
        writeTransfer(path, {original, whiteoutPacket, annotatedPacket});
        const auto decoded = readTransfer(path);
        if (decoded.size() != 3 || decoded[0].page != original.page || !decoded[0].pressureSensitive ||
            decoded[0].width != original.width || decoded[0].color != original.color ||
            decoded[0].layerOrder != original.layerOrder || decoded[0].layerId != original.layerId ||
            decoded[0].originLayer != original.originLayer ||
            decoded[0].points[0].x != original.points[0].x || decoded[0].points[1].pressure != 0.8 ||
            !decoded[1].whiteout || decoded[1].highlighter || decoded[1].width != 8.5 ||
            !decoded[2].highlighter || decoded[2].pressureSensitive || decoded[2].fill != 230 ||
            decoded[2].capStyle != 1) {
            throw std::runtime_error("Ink transfer did not preserve page, style or sample precision");
        }
        const auto hydrated = sessionInk(decoded);
        if (hydrated.size() != 3 || hydrated[0].page != 2 || hydrated[0].brush.tool != prototype::Tool::Pen ||
            !hydrated[0].pressureSensitive || hydrated[0].brush.width != 4.25 ||
            hydrated[0].layerOrder != 19 || hydrated[0].layerId != 3 || hydrated[0].originLayer != 12 ||
            hydrated[0].points[0].pressure != 0.25 || hydrated[0].points[1].pressure != 0.8 ||
            hydrated[1].brush.tool != prototype::Tool::Eraser || hydrated[1].pressureSensitive ||
            hydrated[1].brush.color != prototype::behavior::whiteoutColor ||
            hydrated[2].brush.tool != prototype::Tool::Highlighter || hydrated[2].fill != 230 ||
            hydrated[2].capStyle != 1) {
            throw std::runtime_error("Session hydration lost tool, pressure mode, or whiteout color");
        }
        prototype::CanvasState preloaded;
        preloaded.setPages({{595, 842}, {595, 842}, {595, 842}});
        prototype::PageLayers pageLayers;
        pageLayers.initialLayerCount = 13;
        pageLayers.selectedLayer = 3;
        pageLayers.layers = {{"Layer 1", true, {0}}, {"Layer 2", true, {1}}, {"Layer 3", true, {12}}};
        preloaded.setLayerDocument({prototype::PageLayers{}, prototype::PageLayers{}, pageLayers});
        preloaded.selectActivePage(2);
        preloaded.fit(800, 1000);
        preloaded.ink = {hydrated[0], hydrated[1]};
        if (preloaded.revision() != 0 || preloaded.canUndo() || preloaded.canRedo()) {
            throw std::runtime_error("Preloaded native ink must start at clean revision 0");
        }
        const auto erasePoint = preloaded.pageScreenPoint(2, (12.34 + 80.0) * 0.5, (56.78 + 90.0) * 0.5);
        preloaded.erase(erasePoint.x, erasePoint.y);
        if (preloaded.ink.size() != 3 || preloaded.revision() != 1 || !preloaded.canUndo()) {
            throw std::runtime_error("Preloaded native ink could not be partially erased");
        }
        preloaded.undo();
        if (preloaded.ink.size() != 2 || preloaded.revision() != 0) {
            throw std::runtime_error("Undoing an edit to preloaded native ink did not restore revision 0");
        }
        for (int invalid = 0; invalid < 5; ++invalid) {
            auto stroke = original;
            switch (invalid) {
                case 0:
                    stroke.width = -1;
                    break;
                case 1:
                    stroke.color[0] = 2;
                    break;
                case 2:
                    stroke.points[0].x = std::numeric_limits<double>::quiet_NaN();
                    break;
                case 3:
                    stroke.points.clear();
                    break;
                case 4:
                    stroke.highlighter = true;
                    stroke.whiteout = true;
                    break;
            }
            bool rejected = false;
            try {
                writeTransfer(path, {stroke});
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            if (!rejected || readTransfer(path)[0].width != original.width) {
                throw std::runtime_error("Invalid transfer changed the existing file");
            }
        }
        writeTransfer(path, {});
        if (!readTransfer(path).empty()) {
            throw std::runtime_error("Empty transfer must retain the original document without new ink");
        }
        const std::vector<PageSizePacket> pageSizes{{612.25, 792.5}, {320, 480}};
        writePageSizeTransfer(path, pageSizes);
        const auto decodedPageSizes = readPageSizeTransfer(path);
        if (decodedPageSizes.size() != pageSizes.size() || decodedPageSizes[0].width != pageSizes[0].width ||
            decodedPageSizes[0].height != pageSizes[0].height || decodedPageSizes[1].width != pageSizes[1].width ||
            decodedPageSizes[1].height != pageSizes[1].height) {
            throw std::runtime_error("Page-size transfer did not preserve page dimensions and precision");
        }
        bool rejectedPageSizes = false;
        try {
            writePageSizeTransfer(path, {{595, 842}, {0, 100}});
        } catch (const std::runtime_error&) {
            rejectedPageSizes = true;
        }
        if (!rejectedPageSizes || readPageSizeTransfer(path).size() != pageSizes.size()) {
            throw std::runtime_error("Invalid page-size transfer changed the existing file");
        }
        std::filesystem::remove(path);
        std::cout << "PASS: ink and page-size transfer precision, validation and empty sessions\n";
    } catch (const std::exception& error) {
        std::filesystem::remove(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
