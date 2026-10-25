#pragma once

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "DocumentAutosave.h"
#include "DocumentTextTransfer.h"
#include "DocumentImageTransfer.h"
#include "DocumentLayers.h"
#include "DocumentPdfPages.h"
#include "DocumentTransfer.h"
#include "DocumentVersion.h"

namespace prototype::bridge {

// The GTK 3 loader/renderer lives in a separate process. Its private snapshot
// retains native objects independently of subsequent edits to the source file.
// External linked assets still refer to their original locations.
class DocumentSession {
public:
    struct StagedRevision {
        std::filesystem::path directory;
        std::filesystem::path snapshotPath() const { return directory / "snapshot.xopp"; }
        std::filesystem::path previewPath() const { return directory / "preview.pdf"; }
        std::filesystem::path backgroundPdfPath() const { return directory / "background.pdf"; }
        std::filesystem::path pdfPageMapPath() const { return directory / "pdf-pages.dat"; }
        std::filesystem::path layerStatePath() const { return directory / "layers.dat"; }

        std::vector<std::filesystem::path> layerPreviewPaths() const {
            std::vector<std::filesystem::path> paths;
            const auto pages = readLayerTransfer(layerStatePath());
            size_t count = 0;
            for (const auto& page: pages) {
                count = std::max(count, page.initialLayerCount);
            }
            for (size_t index = 0; index < count; ++index) {
                paths.push_back(directory / ("layer-" + std::to_string(index) + ".pdf"));
            }
            return paths;
        }
    };

    DocumentSession(const std::string& worker, const std::string& input) {
        GError* error = nullptr;
        auto* directory = g_dir_make_tmp("xournalpp-gtk4-document-XXXXXX", &error);
        if (!directory) {
            throwError(error);
        }
        path = directory;
        g_free(directory);
        activeDir = path / "rev-0";
        try {
            std::filesystem::create_directories(activeDir);
            const bool nativeFile = g_ascii_strcasecmp(fs::path(input).extension().string().c_str(), ".xopp") == 0 &&
                                    fs::is_regular_file(fs::symlink_status(input));
            const auto before = nativeFile ? documentVersion(input).token : std::string{};
            const auto extracted = path / "initial.ink";
            const auto extractedTexts = path / "initial.text";
            const auto extractedImages = path / "initial.images";
            const auto layers = activeDir / "layers.dat";
            const auto kind = runWorker(worker, {"open", input, snapshotPath().string(), previewPath().string(),
                                                 extracted.string(), extractedTexts.string(), layers.string(),
                                                 extractedImages.string()});
            initialInk = readTransfer(extracted);
            initialTexts = readTextTransfer(extractedTexts);
            initialImages = readImageTransfer(extractedImages);
            initialLayers = readLayerTransfer(layers);
            initialPdfPageMap = readPdfPageMap(StagedRevision{activeDir}.pdfPageMapPath());
            pdfPageMap = initialPdfPageMap;
            layerPreviewPaths = StagedRevision{activeDir}.layerPreviewPaths();
            std::filesystem::remove(extracted);
            std::filesystem::remove(extractedTexts);
            std::filesystem::remove(extractedImages);
            if (nativeFile) {
                requireUnchangedInput(input, before);
                if (kind == "NATIVE_SAVE\n") {
                    originalSavePath = input;
                    originalVersion = before;
                }
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~DocumentSession() { cleanup(); }
    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    void saveCopy(const std::string& worker, const std::string& output,
                  const std::vector<StrokePacket>& packets, const std::vector<TextItem>& texts,
                  const std::vector<ImagePacket>& images,
                  const LayerDocument& layers) const {
        const auto transfer = path / "session.ink";
        const auto textTransfer = path / "session.text";
        const auto layerTransfer = path / "session.layers";
        const auto imageTransfer = path / "session.images";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        runWorker(worker, {"save-copy", snapshotPath().string(), output, transfer.string(), textTransfer.string(),
                           layerTransfer.string(), imageTransfer.string()});
    }

    std::string save(const std::string& worker, const std::string& output, const std::vector<StrokePacket>& packets,
                     const std::vector<TextItem>& texts, const std::vector<ImagePacket>& images,
                     const LayerDocument& layers, const std::string& expected) const {
        const auto transfer = path / "session.ink";
        const auto textTransfer = path / "session.text";
        const auto layerTransfer = path / "session.layers";
        const auto imageTransfer = path / "session.images";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        auto version = runWorker(worker, {"save", snapshotPath().string(), output, transfer.string(), expected,
                                          textTransfer.string(), layerTransfer.string(), imageTransfer.string()});
        if (!version.empty() && version.back() == '\n') {
            version.pop_back();
        }
        return version;
    }

    void autosave(const std::string& worker, const std::string& output,
                  const std::vector<StrokePacket>& packets, const std::vector<TextItem>& texts,
                  const std::vector<ImagePacket>& images,
                  const LayerDocument& layers) const {
        const auto transfer = path / "autosave.ink";
        const auto textTransfer = path / "autosave.text";
        const auto layerTransfer = path / "autosave.layers";
        const auto imageTransfer = path / "autosave.images";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        runWorker(worker, {"autosave", snapshotPath().string(), output, transfer.string(), textTransfer.string(),
                           layerTransfer.string(), imageTransfer.string()});
    }

    void exportPdf(const std::string& worker, const std::string& output,
                   const std::vector<StrokePacket>& packets, const std::vector<TextItem>& texts,
                   const std::vector<ImagePacket>& images,
                   const LayerDocument& layers) const {
        const auto transfer = path / "export.ink";
        const auto textTransfer = path / "export.text";
        const auto layerTransfer = path / "export.layers";
        const auto imageTransfer = path / "export.images";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        runWorker(worker, {"export-pdf", snapshotPath().string(), output, transfer.string(), textTransfer.string(),
                           layerTransfer.string(), imageTransfer.string()});
    }

    StagedRevision stageInsertPageAfter(const std::string& worker, int afterPage) {
        return stagePageMutation(worker, "insert-page", afterPage);
    }

    StagedRevision stageDeletePageAt(const std::string& worker, int pageIndex) {
        return stagePageMutation(worker, "delete-page", pageIndex);
    }

    void commitRevision(const StagedRevision& staged) {
        const auto previous = activeDir;
        activeDir = staged.directory;
        pdfPageMap = readPdfPageMap(StagedRevision{activeDir}.pdfPageMapPath());
        layerPreviewPaths = StagedRevision{activeDir}.layerPreviewPaths();
        if (!previous.empty() && previous != activeDir) {
            std::error_code ec;
            std::filesystem::remove_all(previous, ec);
        }
    }

    static void discardRevision(const StagedRevision& staged) {
        if (!staged.directory.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(staged.directory, ec);
        }
    }

    std::string originalSavePath;
    std::string originalVersion;
    std::vector<StrokePacket> initialInk;
    std::vector<TextItem> initialTexts;
    std::vector<ImagePacket> initialImages;
    LayerDocument initialLayers;
    std::vector<int> initialPdfPageMap;
    std::vector<int> pdfPageMap;
    std::vector<std::filesystem::path> layerPreviewPaths;

    std::filesystem::path snapshotPath() const { return activeDir / "snapshot.xopp"; }
    std::filesystem::path previewPath() const { return activeDir / "preview.pdf"; }
    std::filesystem::path backgroundPdfPath() const { return activeDir / "background.pdf"; }

private:
    StagedRevision stagePageMutation(const std::string& worker, const char* command, int index) {
        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            runWorker(worker, {command, snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), std::to_string(index)});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }
    static void requireUnchangedInput(const std::string& input, const std::string& before) {
        if (documentVersion(input).token != before) {
            throw std::runtime_error("The document changed while opening. Open it again to load the current file.");
        }
    }

    static std::string runWorker(const std::string& worker, const std::vector<std::string>& arguments) {
        std::vector<const gchar*> command{worker.c_str()};
        for (const auto& argument: arguments) {
            command.push_back(argument.c_str());
        }
        command.push_back(nullptr);
        GError* error = nullptr;
        auto* process = g_subprocess_newv(command.data(),
                                          GSubprocessFlags(G_SUBPROCESS_FLAGS_STDERR_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE),
                                          &error);
        if (!process) {
            throwError(error);
        }
        gchar* diagnostics = nullptr;
        gchar* output = nullptr;
        const bool communicated = g_subprocess_communicate_utf8(process, nullptr, nullptr, &output,
                                                                &diagnostics, &error);
        const bool succeeded = communicated && g_subprocess_get_successful(process);
        const std::string message = diagnostics ? diagnostics : "";
        const std::string result = output ? output : "";
        g_free(diagnostics);
        g_free(output);
        if (!communicated) {
            g_subprocess_force_exit(process);
            g_subprocess_wait(process, nullptr, nullptr);
        }
        g_object_unref(process);
        if (!communicated) {
            throwError(error);
        }
        if (!succeeded) {
            throw std::runtime_error(message.empty() ? "Document worker failed" : message);
        }
        return result;
    }

    [[noreturn]] static void throwError(GError* error) {
        const std::string message = error ? error->message : "Document worker failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }

    void cleanup() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
    std::filesystem::path activeDir;
    size_t nextRevisionId = 1;
};

}  // namespace prototype::bridge
