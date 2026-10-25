#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>

#include "DocumentImageTransfer.h"

static void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / "xournalpp-gtk4-image-transfer-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto path = root / "images.dat";
    try {
        prototype::CanvasImage first;
        first.page = 2;
        first.layerId = 3;
        first.originLayer = 1;
        first.layerOrder = 17;
        first.pixelWidth = 640;
        first.pixelHeight = 480;
        first.transform = {0.75, 0.1, -0.05, 0.75, 123.25, 67.5};
        first.encodedData = "PNG payload bytes";

        auto second = first;
        second.page = 0;
        second.layerId = 1;
        second.originLayer = prototype::newLayerOrigin;
        second.layerOrder = 0;
        second.transform = {1, 0, 0, 1, 0, 0};
        second.encodedData = "another image";

        prototype::CanvasImage latex;
        latex.page = 1;
        latex.layerId = 2;
        latex.layerOrder = 4;
        latex.pixelWidth = 86;
        latex.pixelHeight = 32;
        latex.kind = prototype::CanvasImageKind::Latex;
        latex.nativeWidth = 85.75;
        latex.nativeHeight = 31.5;
        latex.transform = {1.2, 0.1, -0.05, 1.2, 55.25, 72.5};
        latex.encodedData = "%PDF-1.7\nfixture bytes";
        latex.latexSource = "\\frac{x^2}{y}\n% keep this line";

        prototype::bridge::writeImageTransfer(path,
                                              {prototype::bridge::imagePacket(first),
                                               prototype::bridge::imagePacket(second),
                                               prototype::bridge::imagePacket(latex)});
        const auto decoded = prototype::bridge::readImageTransfer(path);
        check(decoded.size() == 3 && decoded[0].page == first.page && decoded[0].layerId == first.layerId &&
                      decoded[0].originLayer == first.originLayer && decoded[0].layerOrder == first.layerOrder &&
                      decoded[0].pixelWidth == first.pixelWidth && decoded[0].pixelHeight == first.pixelHeight &&
                      decoded[0].encodedData == first.encodedData &&
                      decoded[0].transform.x == first.transform.x && decoded[0].transform.xy == first.transform.xy &&
                      decoded[1].page == second.page && decoded[1].encodedData == second.encodedData,
              "Native image transfer changed bytes, geometry or layer ordering");
        check(decoded[2].kind == prototype::CanvasImageKind::Latex && decoded[2].nativeWidth == latex.nativeWidth &&
                      decoded[2].nativeHeight == latex.nativeHeight && decoded[2].encodedData == latex.encodedData &&
                      decoded[2].latexSource == latex.latexSource && decoded[2].transform.yx == latex.transform.yx,
              "Native LaTeX transfer changed its source, PDF, native size or transform");

        prototype::bridge::writeImageTransfer(path, {});
        check(prototype::bridge::readImageTransfer(path).empty(), "Empty image transfer should round-trip");

        auto invalid = first;
        invalid.transform.xx = 0;
        invalid.transform.yy = 0;
        invalid.transform.xy = 0;
        invalid.transform.yx = 0;
        check(throws([&] { prototype::bridge::writeImageTransfer(path, {prototype::bridge::imagePacket(invalid)}); }),
              "Singular image transforms should be rejected");

        auto excessive = first;
        excessive.encodedData.assign(prototype::maxCanvasImageBytes + 1, 'x');
        check(throws([&] { prototype::bridge::writeImageTransfer(path, {prototype::bridge::imagePacket(excessive)}); }),
              "Oversized image payloads should be rejected");

        std::filesystem::remove_all(root);
        std::cout << "PASS: bounded native image transfer preserves encoded data, transforms and layer order\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
