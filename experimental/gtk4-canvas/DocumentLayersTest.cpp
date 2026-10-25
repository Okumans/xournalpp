#include "DocumentLayers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace prototype::bridge;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("gtk4-layer-transfer-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto path = root / "layers.dat";
    try {
        LayerDocument original(2);
        original[0].initialLayerCount = 2;
        original[0].selectedLayer = 2;
        original[0].backgroundVisible = false;
        original[0].backgroundName = "Paper";
        original[0].gridYOffset = 80;
        original[0].layers = {{"Lower", true, {0}}, {"上の層", false, {1, 2}}};
        original[1].initialLayerCount = 1;
        original[1].selectedLayer = 1;
        original[1].layers = {{"Page 2", true, {0}}};

        writeLayerTransfer(path, original);
        const auto decoded = readLayerTransfer(path);
        check(decoded == original, "Layer transfer changed page, visibility, name, origin, or active-layer metadata");

        writeLayerTransfer(path, {});
        check(readLayerTransfer(path).empty(), "Empty layer transfer did not round-trip");

        auto duplicatedLayer = original;
        duplicatedLayer[0].layers[1].origins = {0, 1};
        writeLayerTransfer(path, duplicatedLayer);
        check(readLayerTransfer(path) == duplicatedLayer,
              "A duplicated native source layer must be assignable to multiple destination layers");

        auto duplicateWithinLayer = original;
        duplicateWithinLayer[0].layers[1].origins = {1, 1};
        check(throws([&] { writeLayerTransfer(path, duplicateWithinLayer); }),
              "A destination layer must not contain the same source origin more than once");

        auto missingOrigin = original;
        missingOrigin[0].layers[0].origins.clear();
        check(throws([&] { writeLayerTransfer(path, missingOrigin); }),
              "A layer without a source or editable origin was accepted");

        auto invalidSelection = original;
        invalidSelection[0].selectedLayer = 0;
        check(throws([&] { writeLayerTransfer(path, invalidSelection); }),
              "The immutable background was accepted as the active editable layer");

        auto invalidGridOffset = original;
        invalidGridOffset[0].gridYOffset = std::numeric_limits<double>::infinity();
        check(throws([&] { writeLayerTransfer(path, invalidGridOffset); }),
              "A non-finite page-grid offset was accepted");

        {
            std::ofstream legacy(path, std::ios::binary);
            legacy << "XOURNALPP_GTK4_LAYERS_V1 1\n1 1 1 1\n1 1 1\n0\nL\n";
        }
        const auto legacyDecoded = readLayerTransfer(path);
        check(legacyDecoded.size() == 1 && !legacyDecoded[0].backgroundName &&
                      legacyDecoded[0].layers[0].name == "L",
              "Version 1 layer-transfer metadata must remain readable without an explicit background name");

        {
            std::ofstream legacy(path, std::ios::binary);
            legacy << "XOURNALPP_GTK4_LAYERS_V2 1\n1 1 1 1\n1 1\nP\n1 1 1\n0\nL\n";
        }
        const auto version2Decoded = readLayerTransfer(path);
        check(version2Decoded.size() == 1 && version2Decoded[0].backgroundName == "P" &&
                      version2Decoded[0].gridYOffset == 0 && version2Decoded[0].layers[0].name == "L",
              "Version 2 layer-transfer metadata must remain readable with the default grid offset");

        {
            std::ofstream truncated(path, std::ios::binary);
            truncated << "XOURNALPP_GTK4_LAYERS_V2 1\n1 1 1 1\n1 10\nBackground\n1 4 1\n0\nLow";
        }
        check(throws([&] { static_cast<void>(readLayerTransfer(path)); }),
              "Truncated layer-transfer metadata was accepted");

        std::filesystem::remove_all(root);
        std::cout << "PASS: bounded layer transfer preserves per-page stack and rejects invalid origins\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
