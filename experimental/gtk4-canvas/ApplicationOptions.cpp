#include "ApplicationOptions.h"

#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <string_view>

#include <glib.h>

namespace prototype {
namespace {

void requireDirectory(const std::filesystem::path& path, const char* errorMessage) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || status.type() != std::filesystem::file_type::directory) {
        throw std::runtime_error(errorMessage);
    }
}

}  // namespace

ApplicationOptions parseApplicationOptions(int argc, char** argv) {
    ApplicationOptions options;
    const std::filesystem::path executable(argv[0]);
    if (executable.has_parent_path()) {
        options.rasterExecutable = std::filesystem::absolute(executable).string();
    } else if (auto* resolved = g_find_program_in_path(argv[0])) {
        options.rasterExecutable = resolved;
        g_free(resolved);
    } else {
        throw std::runtime_error("Cannot locate the page raster renderer");
    }

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--dark") {
            options.dark = true;
            options.explicitDark = true;
        } else if (argument == "--autosave-seconds" && i + 1 < argc) {
            const std::string_view raw = argv[++i];
            int parsed = 0;
            const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
            if (error != std::errc{} || end != raw.data() + raw.size() || parsed < 1 || parsed > 86400) {
                throw std::runtime_error("Autosave interval must be between 1 and 86400 seconds");
            }
            options.autosaveSeconds = parsed;
            options.autosaveSecondsOverride = parsed;
        } else if (argument == "--disable-audio") {
            options.disableAudio = true;
        } else if (argument == "--config-dir" && i + 1 < argc) {
            const auto configPath = std::filesystem::absolute(argv[++i]);
            requireDirectory(configPath, "Configuration directory is missing or not a directory");
            options.configDirectory = configPath.string();
        } else if (argument == "--plugin-dir" && i + 1 < argc) {
            const auto pluginPath = std::filesystem::absolute(argv[++i]);
            requireDirectory(pluginPath, "Plugin directory is missing or not a directory");
            options.pluginDirectory = pluginPath.string();
        } else if (argument == "--verify-plugins") {
            options.verifyPlugins = true;
        } else if (argument == "--verify-search") {
            options.verifySearch = true;
        } else if (argument == "--verify-scroll-motion") {
            options.verifyScrollMotion = true;
        } else if (argument == "--verify-smart-select-performance") {
            options.verifySmartSelectPerformance = true;
        } else if (argument == "--verify-sidebar-performance") {
            options.verifySidebarPerformance = true;
        } else if (argument == "--verify-sidebar-startup") {
            options.verifySidebarStartup = true;
        } else if (argument == "--verify-page-layout") {
            options.verifyPageLayout = true;
        } else if (argument == "--verify-presentation-mode") {
            options.verifyPresentationMode = true;
        } else if (argument == "--verify-pen-line-style") {
            options.verifyPenLineStyle = true;
        } else if (argument == "--verify-beamer") {
            options.verifyBeamer = true;
        } else if (argument == "--verify-layer-actions") {
            options.verifyLayerActions = true;
        } else if (argument == "--verify-latex") {
            options.verifyLatex = true;
        } else if (argument == "--verify-text-clipboard") {
            options.verifyTextClipboard = true;
        } else if ((argument == "--pdf" || argument == "--document" || argument == "--worker" ||
                    argument == "--smoke-dir") &&
                   i + 1 < argc) {
            const auto path = std::filesystem::absolute(argv[++i]).string();
            if (argument == "--pdf") {
                options.pdf = path;
            } else if (argument == "--document") {
                options.nativeDocument = path;
            } else if (argument == "--worker") {
                options.worker = path;
            } else {
                options.smokeDirectory = path;
                std::filesystem::create_directories(path);
            }
        } else {
            throw std::runtime_error("Usage: xournalpp-gtk4-canvas [--pdf FILE | --document FILE --worker WORKER] "
                                     "[--config-dir DIRECTORY] [--autosave-seconds SECONDS] [--dark] "
                                     "[--plugin-dir DIRECTORY] [--verify-plugins] [--verify-search] "
                                     "[--verify-scroll-motion] "
                                     "[--verify-smart-select-performance] "
                                     "[--verify-sidebar-performance] [--verify-sidebar-startup] "
                                     "[--verify-page-layout] [--verify-presentation-mode] "
                                     "[--verify-pen-line-style] [--verify-beamer] "
                                     "[--verify-layer-actions] [--verify-latex] [--verify-text-clipboard] "
                                     "[--disable-audio] [--smoke-dir DIRECTORY]");
        }
    }

    if (!options.nativeDocument.empty() && (!options.pdf.empty() || options.worker.empty())) {
        throw std::runtime_error("Use --document with --worker, separately from --pdf");
    }
    if (!options.worker.empty() && !g_file_test(options.worker.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
        throw std::runtime_error("Document worker is missing or not executable");
    }
    if (options.verifyPlugins && options.smokeDirectory.empty()) {
        throw std::runtime_error("Plugin verification requires --smoke-dir DIRECTORY");
    }
    if (options.verifySearch && (options.smokeDirectory.empty() || options.pdf.empty() ||
                                 !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error("Search verification requires --smoke-dir and --pdf without --worker");
    }
    if (options.verifyScrollMotion && (options.smokeDirectory.empty() || !options.pdf.empty() ||
                                       !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error("Scroll motion verification requires --smoke-dir without a document or worker");
    }
    if (options.verifySmartSelectPerformance && (options.smokeDirectory.empty() || !options.pdf.empty() ||
                                                 !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error(
                "Smart Select performance verification requires --smoke-dir without a document or worker");
    }
    if (options.verifySidebarPerformance && (options.smokeDirectory.empty() || options.pdf.empty() ||
                                             !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error("Sidebar performance verification requires --smoke-dir and --pdf without --worker");
    }
    if (options.verifySidebarStartup && (options.smokeDirectory.empty() || options.pdf.empty() ||
                                         !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error("Sidebar startup verification requires --smoke-dir and --pdf without --worker");
    }
    if (options.verifyPageLayout && (options.smokeDirectory.empty() || !options.pdf.empty() ||
                                     !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error("Page layout verification requires --smoke-dir without a document or worker");
    }
    if (options.verifyPresentationMode && (options.smokeDirectory.empty() || !options.pdf.empty() ||
                                           !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error("Presentation Mode verification requires --smoke-dir without a document or worker");
    }
    if (options.verifyPenLineStyle && (options.smokeDirectory.empty() || !options.pdf.empty() ||
                                       !options.nativeDocument.empty() || !options.worker.empty())) {
        throw std::runtime_error("Pen line-style verification requires --smoke-dir without a document or worker");
    }
    if (options.verifyBeamer && options.smokeDirectory.empty()) {
        throw std::runtime_error("Beamer verification requires --smoke-dir DIRECTORY");
    }
    if (options.verifyLayerActions && options.smokeDirectory.empty()) {
        throw std::runtime_error("LayerActions verification requires --smoke-dir DIRECTORY");
    }
    if (options.verifyLatex &&
        (options.smokeDirectory.empty() || options.nativeDocument.empty() || options.worker.empty())) {
        throw std::runtime_error("LaTeX verification requires --smoke-dir, --document and --worker");
    }
    if (options.verifyTextClipboard &&
        (options.smokeDirectory.empty() || options.nativeDocument.empty() || options.worker.empty())) {
        throw std::runtime_error("Text clipboard verification requires --smoke-dir, --document and --worker");
    }

    return options;
}

}  // namespace prototype
