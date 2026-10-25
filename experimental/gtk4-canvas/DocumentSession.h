#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "AudioTarget.h"
#include "DocumentAutosave.h"
#include "DocumentImageTransfer.h"
#include "DocumentLayers.h"
#include "DocumentPdfPages.h"
#include "PdfPageLabels.h"
#include "DocumentTextTransfer.h"
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
        std::filesystem::path pdfPageLabelsPath() const { return directory / "pdf-labels.dat"; }
        std::filesystem::path layerStatePath() const { return directory / "layers.dat"; }
        std::filesystem::path audioTargetPath() const { return directory / "audio-targets.dat"; }

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

        std::vector<std::filesystem::path> audioLayerPreviewPaths() const {
            std::vector<std::filesystem::path> paths;
            const auto pages = readLayerTransfer(layerStatePath());
            size_t count = 0;
            for (const auto& page: pages) {
                count = std::max(count, page.initialLayerCount);
            }
            for (size_t index = 0; index < count; ++index) {
                paths.push_back(directory / ("audio-layer-" + std::to_string(index) + ".pdf"));
            }
            return paths;
        }
    };

    DocumentSession(const std::string& worker, const std::string& input,
                    std::filesystem::path audioFolder = {}) : audioFolder(std::move(audioFolder)) {
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
            audioDirectory = path / "audio";
            std::filesystem::create_directories(audioDirectory);
            const bool nativeFile = g_ascii_strcasecmp(fs::path(input).extension().string().c_str(), ".xopp") == 0 &&
                                    fs::is_regular_file(fs::symlink_status(input));
            const auto before = nativeFile ? documentVersion(input).token : std::string{};
            const auto extracted = path / "initial.ink";
            const auto extractedTexts = path / "initial.text";
            const auto extractedImages = path / "initial.images";
            const auto layers = activeDir / "layers.dat";
            const auto kind = runWorker(worker, {"open", input, snapshotPath().string(), previewPath().string(),
                                                 extracted.string(), extractedTexts.string(), layers.string(),
                                                 extractedImages.string(), this->audioFolder.string(),
                                                 audioDirectory.string()});
            initialInk = readTransfer(extracted);
            initialTexts = readTextTransfer(extractedTexts);
            initialImages = readImageTransfer(extractedImages);
            initialLayers = readLayerTransfer(layers);
            initialAudioTargets = readAudioTargets(StagedRevision{activeDir}.audioTargetPath());
            initialPdfPageMap = readPdfPageMap(StagedRevision{activeDir}.pdfPageMapPath());
            pdfPageMap = initialPdfPageMap;
            pdfPageLabels = readPdfPageLabels(StagedRevision{activeDir}.pdfPageLabelsPath());
            layerPreviewPaths = StagedRevision{activeDir}.layerPreviewPaths();
            audioLayerPreviewPaths = StagedRevision{activeDir}.audioLayerPreviewPaths();
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

    StagedRevision stageRevision(const std::string& worker, const std::vector<StrokePacket>& packets,
                                 const std::vector<TextItem>& texts, const std::vector<ImagePacket>& images,
                                 const LayerDocument& layers) {
        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            const auto transfer = staged.directory / "revision.ink";
            const auto textTransfer = staged.directory / "revision.text";
            const auto layerTransfer = staged.directory / "revision.layers";
            const auto imageTransfer = staged.directory / "revision.images";
            writeTransfer(transfer, packets);
            writeTextTransfer(textTransfer, texts);
            writeLayerTransfer(layerTransfer, layers);
            writeImageTransfer(imageTransfer, images);
            runWorker(worker,
                      {"revision", snapshotPath().string(), staged.snapshotPath().string(),
                       staged.previewPath().string(), transfer.string(), textTransfer.string(), layerTransfer.string(),
                       imageTransfer.string(), audioFolder.string(), audioDirectory.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    StagedRevision stageInsertPageAfter(const std::string& worker, int afterPage) {
        return stagePageMutation(worker, "insert-page", afterPage);
    }

    StagedRevision stageInsertPagesAfter(const std::string& worker, const std::vector<int>& afterPages) {
        if (afterPages.empty() || afterPages.size() > maxPdfPageMapPages || pdfPageMap.size() > maxPdfPageMapPages) {
            throw std::runtime_error("Invalid page insertion batch size");
        }
        int previous = std::numeric_limits<int>::max();
        for (int afterPage: afterPages) {
            if (afterPage < 0 || static_cast<size_t>(afterPage) >= pdfPageMap.size() || afterPage >= previous) {
                throw std::runtime_error("Page insertion indices must be valid and strictly descending");
            }
            previous = afterPage;
        }

        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            const auto insertions = staged.directory / "page-insertions.dat";
            std::ofstream output(insertions, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Cannot create page insertion list");
            }
            output << "XOURNALPP_GTK4_PAGE_INSERTIONS_V1 " << afterPages.size() << '\n';
            for (int afterPage: afterPages) {
                output << afterPage << '\n';
            }
            output.close();
            if (!output) {
                throw std::runtime_error("Cannot write page insertion list");
            }

            runWorker(worker, {"insert-pages", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), insertions.string(), audioFolder.string(),
                               audioDirectory.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    StagedRevision stageDeletePageAt(const std::string& worker, int pageIndex) {
        return stagePageMutation(worker, "delete-page", pageIndex);
    }

    StagedRevision stageDeletePagesAt(const std::string& worker, const std::vector<int>& pageIndices) {
        if (pageIndices.empty() || pageIndices.size() >= pdfPageMap.size() || pageIndices.size() > maxPdfPageMapPages) {
            throw std::runtime_error("Invalid page deletion batch size");
        }
        int previous = std::numeric_limits<int>::max();
        for (int pageIndex: pageIndices) {
            if (pageIndex < 0 || static_cast<size_t>(pageIndex) >= pdfPageMap.size() || pageIndex >= previous) {
                throw std::runtime_error("Page deletion indices must be valid and strictly descending");
            }
            previous = pageIndex;
        }

        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            const auto deletions = staged.directory / "page-deletions.dat";
            std::ofstream output(deletions, std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("Cannot create page deletion list");
            }
            output << "XOURNALPP_GTK4_PAGE_DELETIONS_V1 " << pageIndices.size() << '\n';
            for (int pageIndex: pageIndices) {
                output << pageIndex << '\n';
            }
            output.close();
            if (!output) {
                throw std::runtime_error("Cannot write page deletion list");
            }
            runWorker(worker, {"delete-pages", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), deletions.string(), audioFolder.string(),
                               audioDirectory.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    StagedRevision stageSetPdfPageBackground(const std::string& worker, int documentPage, int pdfPage,
                                             const std::vector<StrokePacket>& packets,
                                             const std::vector<TextItem>& texts, const std::vector<ImagePacket>& images,
                                             const LayerDocument& layers) {
        if (documentPage < 0 || static_cast<size_t>(documentPage) >= pdfPageMap.size() || pdfPage < 0 ||
            static_cast<size_t>(pdfPage) >= pdfPageLabels.size()) {
            throw std::runtime_error("Invalid PDF background page selection");
        }
        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            const auto ink = staged.directory / "background-change.ink";
            const auto text = staged.directory / "background-change.text";
            const auto layer = staged.directory / "background-change.layers";
            const auto image = staged.directory / "background-change.images";
            writeTransfer(ink, packets);
            writeTextTransfer(text, texts);
            writeLayerTransfer(layer, layers);
            writeImageTransfer(image, images);
            runWorker(worker, {"set-pdf-page-background", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), std::to_string(documentPage), std::to_string(pdfPage),
                               ink.string(), text.string(), layer.string(), image.string(), audioFolder.string(),
                               audioDirectory.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    void commitRevision(const StagedRevision& staged) {
        const auto previous = activeDir;
        activeDir = staged.directory;
        pdfPageMap = readPdfPageMap(StagedRevision{activeDir}.pdfPageMapPath());
        pdfPageLabels = readPdfPageLabels(StagedRevision{activeDir}.pdfPageLabelsPath());
        layerPreviewPaths = StagedRevision{activeDir}.layerPreviewPaths();
        audioLayerPreviewPaths = StagedRevision{activeDir}.audioLayerPreviewPaths();
        audioTargets = readAudioTargets(StagedRevision{activeDir}.audioTargetPath());
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
    std::vector<AudioTarget> initialAudioTargets;
    LayerDocument initialLayers;
    std::vector<int> initialPdfPageMap;
    std::vector<std::string> pdfPageLabels;
    std::vector<int> pdfPageMap;
    std::vector<std::filesystem::path> layerPreviewPaths;
    std::vector<std::filesystem::path> audioLayerPreviewPaths;
    std::vector<AudioTarget> audioTargets;

    std::filesystem::path snapshotPath() const { return activeDir / "snapshot.xopp"; }
    std::filesystem::path previewPath() const { return activeDir / "preview.pdf"; }
    std::filesystem::path backgroundPdfPath() const { return activeDir / "background.pdf"; }
    const std::filesystem::path& getAudioFolder() const { return audioFolder; }

private:
    StagedRevision stagePageMutation(const std::string& worker, const char* command, int index) {
        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            runWorker(worker, {command, snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), std::to_string(index), audioFolder.string(),
                               audioDirectory.string()});
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
    std::filesystem::path audioFolder;
    std::filesystem::path audioDirectory;
    std::filesystem::path activeDir;
    size_t nextRevisionId = 1;
};

}  // namespace prototype::bridge
