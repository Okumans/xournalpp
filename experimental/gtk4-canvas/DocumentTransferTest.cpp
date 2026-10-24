#include <chrono>
#include <iostream>

#include "DocumentTransfer.h"

using namespace prototype::bridge;

int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("gtk4-ink-transfer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const StrokePacket original{2, false, true, 4.25, {0.1, 0.2, 0.9}, {{12.34, 56.78, 0.25}, {80, 90, 0.8}}};
        writeTransfer(path, {original});
        const auto decoded = readTransfer(path);
        if (decoded.size() != 1 || decoded[0].page != original.page || !decoded[0].pressureSensitive ||
            decoded[0].width != original.width || decoded[0].color != original.color ||
            decoded[0].points[0].x != original.points[0].x || decoded[0].points[1].pressure != 0.8) {
            throw std::runtime_error("Ink transfer did not preserve page, style or sample precision");
        }
        for (int invalid = 0; invalid < 4; ++invalid) {
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
