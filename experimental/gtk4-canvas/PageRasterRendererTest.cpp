#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>

#include "PageRasterRenderer.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int runWorker(const std::filesystem::path& outputDirectory, std::string& errorOutput) {
    std::vector<std::string> arguments{"xournalpp-gtk4-canvas",
                                       "--gtk4-page-raster-worker",
                                       "--page",
                                       "0",
                                       "--width",
                                       "16",
                                       "--height",
                                       "16",
                                       "--scale",
                                       "1",
                                       "--quality",
                                       "full",
                                       "--medium-scale",
                                       "0.5",
                                       "--lod-scale",
                                       "0.25",
                                       "--output-dir",
                                       outputDirectory.string()};
    std::vector<char*> argumentPointers;
    argumentPointers.reserve(arguments.size());
    for (auto& argument: arguments) {
        argumentPointers.push_back(argument.data());
    }

    std::ostringstream captured;
    auto* previousErrorStream = std::cerr.rdbuf(captured.rdbuf());
    const int result =
            prototype::runPageRasterWorker(static_cast<int>(argumentPointers.size()), argumentPointers.data());
    std::cerr.rdbuf(previousErrorStream);
    errorOutput = captured.str();
    return result;
}

}  // namespace

int main() {
    GError* error = nullptr;
    auto* temporary = g_dir_make_tmp("xournalpp-page-raster-test-XXXXXX", &error);
    if (!temporary) {
        const std::string message = error ? error->message : "Could not create test directory";
        g_clear_error(&error);
        std::cerr << "FAIL: " << message << '\n';
        return 1;
    }
    const std::filesystem::path temporaryDirectory(temporary);
    g_free(temporary);

    try {
        const auto optionalFailureDirectory = temporaryDirectory / "optional-failure";
        const auto blockedMediumPath = optionalFailureDirectory / "medium-background.png";
        std::filesystem::create_directories(blockedMediumPath);
        std::ofstream(blockedMediumPath / "keep").put('x');
        std::string optionalError;
        require(runWorker(optionalFailureDirectory, optionalError) == 0,
                "Optional medium-detail output failure made the page worker fail");
        require(std::filesystem::is_regular_file(optionalFailureDirectory / "background.png"),
                "Optional medium-detail output failure discarded the full-detail background");
        require(std::filesystem::is_regular_file(optionalFailureDirectory / "lod-background.png"),
                "Optional medium-detail output failure prevented the low-detail background");
        require(std::filesystem::is_regular_file(blockedMediumPath / "keep"),
                "Fault injection did not block the medium-detail output path");
        std::ifstream optionalReport(optionalFailureDirectory / "error.txt");
        const std::string report((std::istreambuf_iterator<char>(optionalReport)), std::istreambuf_iterator<char>());
        require(report.find("medium-detail page-raster background") != std::string::npos,
                "Optional-tier report did not identify the failed raster tier");

        const auto primaryFailureDirectory = temporaryDirectory / "primary-failure";
        std::filesystem::create_directories(primaryFailureDirectory / "background.png");
        std::string primaryError;
        require(runWorker(primaryFailureDirectory, primaryError) != 0,
                "Primary background output failure unexpectedly succeeded");
        require(primaryError.find("full-detail page-raster background") != std::string::npos &&
                        primaryError.find((primaryFailureDirectory / "background.png").string()) != std::string::npos,
                "Primary output failure did not report its tier and path");
    } catch (const std::exception& exception) {
        std::error_code cleanupError;
        std::filesystem::remove_all(temporaryDirectory, cleanupError);
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }

    std::error_code cleanupError;
    std::filesystem::remove_all(temporaryDirectory, cleanupError);
    std::cout << "PASS: page raster keeps primary output when an optional LOD tier fails and reports primary errors\n";
    return 0;
}
