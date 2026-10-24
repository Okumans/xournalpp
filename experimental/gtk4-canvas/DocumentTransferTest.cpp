#include <chrono>
#include <iostream>

#include "DocumentInk.h"

using namespace prototype::bridge;

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("gtk4-ink-transfer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        prototype::Ink mouse{1, {{1, 2, 1}, {3, 4, 1}}, {prototype::Tool::Pen, {1, 0, 0}, 3}};
        auto stylus = mouse;
        stylus.pressureSensitive = true;
        auto highlighter = stylus;
        highlighter.brush.tool = prototype::Tool::Highlighter;
        prototype::Ink whiteout{0, {{10, 15, 0.3}, {50, 15, 0.9}}, {prototype::Tool::Eraser, {0.2, 0.3, 0.4}, 8.5}, true};
        const auto packets = transferInk({mouse, stylus, highlighter, whiteout});
        if (packets[0].pressureSensitive || !packets[1].pressureSensitive || !packets[2].highlighter ||
            !packets[3].whiteout || packets[3].highlighter || packets[3].pressureSensitive ||
            packets[3].color != prototype::behavior::whiteoutColor ||
            packets[1].points[1].x != 3 || packets[0].page != 1) {
            throw std::runtime_error("Session ink transfer changed tool, pressure mode or page coordinates");
        }
        const StrokePacket original{2, false, true, 4.25, {0.1, 0.2, 0.9}, {{12.34, 56.78, 0.25}, {80, 90, 0.8}}};
        const StrokePacket whiteoutPacket{0, false, false, 8.5, {1, 1, 1}, {{10, 20, 1}, {40, 20, 1}}, true};
        writeTransfer(path, {original, whiteoutPacket});
        const auto decoded = readTransfer(path);
        if (decoded.size() != 2 || decoded[0].page != original.page || !decoded[0].pressureSensitive ||
            decoded[0].width != original.width || decoded[0].color != original.color ||
            decoded[0].points[0].x != original.points[0].x || decoded[0].points[1].pressure != 0.8 ||
            !decoded[1].whiteout || decoded[1].highlighter || decoded[1].width != 8.5) {
            throw std::runtime_error("Ink transfer did not preserve page, style or sample precision");
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
        std::filesystem::remove(path);
        std::cout << "PASS: transfer precision, pressure flag, preflight validation and empty sessions\n";
    } catch (const std::exception& error) {
        std::filesystem::remove(path);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
