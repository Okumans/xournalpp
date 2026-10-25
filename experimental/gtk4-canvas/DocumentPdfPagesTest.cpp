#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DocumentPdfPages.h"
#include "PdfPageLabels.h"

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool throws(const std::function<void()>& action) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / "xournalpp-gtk4-pdf-pages-test.dat";
    try {
        prototype::bridge::writePdfPageMap(path, {0, 3, -1, 1});
        check(prototype::bridge::readPdfPageMap(path) == std::vector<int>({0, 3, -1, 1}),
              "PDF page-map round trip changed page assignments");

        const auto labelsPath = path.string() + ".labels";
        prototype::bridge::writePdfPageLabels(labelsPath, {"1", "1", "Title page", "β"});
        check(prototype::bridge::readPdfPageLabels(labelsPath) ==
                      std::vector<std::string>({"1", "1", "Title page", "β"}),
              "PDF page-label round trip changed labels");
        check(throws([&] {
                  prototype::bridge::writePdfPageLabels(
                          labelsPath, {std::string(prototype::bridge::maxPdfPageLabelLength + 1, 'x')});
              }),
              "PDF page-label writer accepted an oversized label");
        {
            std::ofstream invalid(labelsPath, std::ios::trunc);
            invalid << "XOURNALPP_GTK4_PDF_LABELS_V1 1\n3\nabc";
        }
        check(throws([&] { prototype::bridge::readPdfPageLabels(labelsPath); }),
              "PDF page-label reader accepted a truncated label");

        {
            std::ofstream invalid(path, std::ios::trunc);
            invalid << "XOURNALPP_GTK4_PDF_PAGES_V1 1\n-2\n";
        }
        check(throws([&] { prototype::bridge::readPdfPageMap(path); }),
              "PDF page map accepted an invalid negative background page");
        std::filesystem::remove(path);
        std::filesystem::remove(labelsPath);
        std::cout << "PASS: PDF page-map round trip and validation\n";
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".labels", ignored);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
