#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include <gio/gio.h>

#include "AudioTarget.h"
#include "DocumentAutosave.h"
#include "DocumentExport.h"
#include "DocumentImageTransfer.h"
#include "DocumentLayers.h"
#include "DocumentPathTransfer.h"
#include "DocumentPdfPages.h"
#include "DocumentTextTransfer.h"
#include "DocumentTransfer.h"
#include "DocumentVersion.h"
#include "PageSizeTransfer.h"
#include "PdfPageLabels.h"

namespace prototype::bridge {

struct LayerActionTargetInfo {
    bool hasNextPage = false;
    int nextPdfPage = -1;
    bool nextPageAnnotated = false;
};

// The GTK 3 loader/renderer lives in a separate process. Its private snapshot
// retains native objects independently of subsequent edits to the source file.
// External linked assets still refer to their original locations.
class DocumentSession {
public:
    struct StagedRevision {
        StagedRevision() = default;
        explicit StagedRevision(std::filesystem::path directory): directory(std::move(directory)) {}

        std::filesystem::path directory;
        std::filesystem::path pageHistorySnapshotPath;
        std::filesystem::path insertedPageHistorySnapshotPath;
        std::filesystem::path snapshotPath() const { return directory / "snapshot.xopp"; }
        std::filesystem::path previewPath() const { return directory / "preview.pdf"; }
        std::filesystem::path backgroundPdfPath() const { return directory / "background.pdf"; }
        std::filesystem::path pdfPageMapPath() const { return directory / "pdf-pages.dat"; }
        std::filesystem::path pdfPageLabelsPath() const { return directory / "pdf-labels.dat"; }
        std::filesystem::path layerStatePath() const { return directory / "layers.dat"; }
        std::filesystem::path audioTargetPath() const { return directory / "audio-targets.dat"; }
        std::filesystem::path editableInkPath() const { return directory / "editable-ink.dat"; }
        std::filesystem::path editableTextPath() const { return directory / "editable-text.dat"; }
        std::filesystem::path editableImagesPath() const { return directory / "editable-images.dat"; }
        std::filesystem::path pageSizesPath() const { return directory / "page-sizes.dat"; }

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
            initialPageSizes = readPageSizeTransfer(StagedRevision{activeDir}.pageSizesPath());
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
                  const LayerDocument& layers, const std::vector<PageSizePacket>& pageSizes) const {
        const auto transfer = path / "session.ink";
        const auto textTransfer = path / "session.text";
        const auto layerTransfer = path / "session.layers";
        const auto imageTransfer = path / "session.images";
        const auto pageSizeTransfer = path / "session.page-sizes";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        writePageSizeTransfer(pageSizeTransfer, pageSizes);
        runWorker(worker, {"save-copy", snapshotPath().string(), output, transfer.string(), textTransfer.string(),
                           layerTransfer.string(), imageTransfer.string(), pageSizeTransfer.string()});
    }

    std::string save(const std::string& worker, const std::string& output, const std::vector<StrokePacket>& packets,
                     const std::vector<TextItem>& texts, const std::vector<ImagePacket>& images,
                     const LayerDocument& layers, const std::vector<PageSizePacket>& pageSizes,
                     const std::string& expected) const {
        const auto transfer = path / "session.ink";
        const auto textTransfer = path / "session.text";
        const auto layerTransfer = path / "session.layers";
        const auto imageTransfer = path / "session.images";
        const auto pageSizeTransfer = path / "session.page-sizes";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        writePageSizeTransfer(pageSizeTransfer, pageSizes);
        auto version = runWorker(worker, {"save", snapshotPath().string(), output, transfer.string(), expected,
                                          textTransfer.string(), layerTransfer.string(), imageTransfer.string(),
                                          pageSizeTransfer.string()});
        if (!version.empty() && version.back() == '\n') {
            version.pop_back();
        }
        return version;
    }

    void autosave(const std::string& worker, const std::string& output,
                  const std::vector<StrokePacket>& packets, const std::vector<TextItem>& texts,
                  const std::vector<ImagePacket>& images,
                  const LayerDocument& layers, const std::vector<PageSizePacket>& pageSizes) const {
        const auto transfer = path / "autosave.ink";
        const auto textTransfer = path / "autosave.text";
        const auto layerTransfer = path / "autosave.layers";
        const auto imageTransfer = path / "autosave.images";
        const auto pageSizeTransfer = path / "autosave.page-sizes";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        writePageSizeTransfer(pageSizeTransfer, pageSizes);
        runWorker(worker, {"autosave", snapshotPath().string(), output, transfer.string(), textTransfer.string(),
                           layerTransfer.string(), imageTransfer.string(), pageSizeTransfer.string()});
    }

    void exportPdf(const std::string& worker, const std::string& output,
                   const std::vector<StrokePacket>& packets, const std::vector<TextItem>& texts,
                   const std::vector<ImagePacket>& images,
                   const LayerDocument& layers, const std::vector<PageSizePacket>& pageSizes) const {
        const auto transfer = path / "export.ink";
        const auto textTransfer = path / "export.text";
        const auto layerTransfer = path / "export.layers";
        const auto imageTransfer = path / "export.images";
        const auto pageSizeTransfer = path / "export.page-sizes";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        writePageSizeTransfer(pageSizeTransfer, pageSizes);
        runWorker(worker, {"export-pdf", snapshotPath().string(), output, transfer.string(), textTransfer.string(),
                           layerTransfer.string(), imageTransfer.string(), pageSizeTransfer.string()});
    }

    void exportDocument(const std::string& worker, const DocumentExportOptions& options,
                        const std::vector<StrokePacket>& packets, const std::vector<TextItem>& texts,
                        const std::vector<ImagePacket>& images, const LayerDocument& layers,
                        const std::vector<PageSizePacket>& pageSizes) const {
        const auto transfer = path / "plugin-export.ink";
        const auto textTransfer = path / "plugin-export.text";
        const auto layerTransfer = path / "plugin-export.layers";
        const auto imageTransfer = path / "plugin-export.images";
        const auto pageSizeTransfer = path / "plugin-export.page-sizes";
        writeTransfer(transfer, packets);
        writeTextTransfer(textTransfer, texts);
        writeLayerTransfer(layerTransfer, layers);
        writeImageTransfer(imageTransfer, images);
        writePageSizeTransfer(pageSizeTransfer, pageSizes);
        runWorker(worker,
                  {"export-document", snapshotPath().string(), options.outputFile, transfer.string(),
                   textTransfer.string(), layerTransfer.string(), imageTransfer.string(), pageSizeTransfer.string(),
                   options.range.value_or(""), options.layerRange.value_or(""), options.background,
                   options.progressiveMode ? "1" : "0", options.backend, std::to_string(options.pngDpi),
                   std::to_string(options.pngWidth), std::to_string(options.pngHeight)});
    }

    StagedRevision stageRevision(const std::string& worker, const std::vector<StrokePacket>& packets,
                                 const std::vector<TextItem>& texts, const std::vector<ImagePacket>& images,
                                 const LayerDocument& layers, const std::vector<PageSizePacket>& pageSizes) {
        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            const auto transfer = staged.directory / "revision.ink";
            const auto textTransfer = staged.directory / "revision.text";
            const auto layerTransfer = staged.directory / "revision.layers";
            const auto imageTransfer = staged.directory / "revision.images";
            const auto pageSizeTransfer = staged.directory / "revision.page-sizes";
            writeTransfer(transfer, packets);
            writeTextTransfer(textTransfer, texts);
            writeLayerTransfer(layerTransfer, layers);
            writeImageTransfer(imageTransfer, images);
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker,
                      {"revision", snapshotPath().string(), staged.snapshotPath().string(),
                       staged.previewPath().string(), transfer.string(), textTransfer.string(), layerTransfer.string(),
                       imageTransfer.string(), audioFolder.string(), audioDirectory.string(), pageSizeTransfer.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    StagedRevision stageInsertPageAfter(const std::string& worker, int afterPage,
                                        const std::vector<PageSizePacket>& pageSizes) {
        return stagePageMutation(worker, "insert-page", afterPage, pageSizes);
    }

    StagedRevision stageInsertPagesAfter(const std::string& worker, const std::vector<int>& afterPages,
                                         const std::vector<PageSizePacket>& pageSizes) {
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

            const auto pageSizeTransfer = staged.directory / "page-mutation.page-sizes";
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker, {"insert-pages", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), insertions.string(), audioFolder.string(),
                               audioDirectory.string(), pageSizeTransfer.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    StagedRevision stageDeletePageAt(const std::string& worker, int pageIndex,
                                     const std::vector<PageSizePacket>& pageSizes, bool preserveForUndo = true) {
        if (!preserveForUndo) {
            return stagePageMutation(worker, "delete-page", pageIndex, pageSizes);
        }

        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        const auto historyDirectory = path / "page-history" / ("page-" + std::to_string(nextPageHistoryId++));
        staged.pageHistorySnapshotPath = historyDirectory / "snapshot.xopp";
        std::filesystem::create_directories(staged.directory);
        std::filesystem::create_directories(historyDirectory);
        try {
            const auto pageSizeTransfer = staged.directory / "page-mutation.page-sizes";
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker, {"delete-page-history", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), std::to_string(pageIndex), audioFolder.string(),
                               audioDirectory.string(), pageSizeTransfer.string(),
                               staged.pageHistorySnapshotPath.string()});
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(historyDirectory, ignored);
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    StagedRevision stageRestorePageAt(const std::string& worker, int pageIndex,
                                      const std::filesystem::path& pageSnapshot,
                                      const std::vector<PageSizePacket>& pageSizes) {
        if (pageSnapshot.empty() || !std::filesystem::is_regular_file(pageSnapshot) || pageIndex < 0 ||
            static_cast<size_t>(pageIndex) > pdfPageMap.size()) {
            throw std::runtime_error("The deleted page snapshot is unavailable");
        }
        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            const auto pageSizeTransfer = staged.directory / "page-mutation.page-sizes";
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker, {"restore-page", snapshotPath().string(), pageSnapshot.string(),
                               staged.snapshotPath().string(), staged.previewPath().string(),
                               std::to_string(pageIndex), audioFolder.string(), audioDirectory.string(),
                               pageSizeTransfer.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    void prunePageHistorySnapshots(const std::vector<std::string>& retainedPaths) const {
        const auto historyRoot = path / "page-history";
        std::error_code error;
        if (!std::filesystem::is_directory(historyRoot, error) || error) {
            return;
        }
        std::unordered_set<std::string> retained;
        retained.reserve(retainedPaths.size());
        for (const auto& item: retainedPaths) {
            retained.insert(std::filesystem::path(item).lexically_normal().string());
        }
        for (std::filesystem::directory_iterator it(historyRoot, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_directory(error) || error) {
                error.clear();
                continue;
            }
            const auto snapshot = (it->path() / "snapshot.xopp").lexically_normal().string();
            if (!retained.contains(snapshot)) {
                std::filesystem::remove_all(it->path(), error);
                error.clear();
            }
        }
    }

    StagedRevision stageDeletePagesAt(const std::string& worker, const std::vector<int>& pageIndices,
                                      const std::vector<PageSizePacket>& pageSizes) {
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
            const auto pageSizeTransfer = staged.directory / "page-mutation.page-sizes";
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker, {"delete-pages", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), deletions.string(), audioFolder.string(),
                               audioDirectory.string(), pageSizeTransfer.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    StagedRevision stageSetPdfPageBackground(const std::string& worker, int documentPage, int pdfPage,
                                             const std::vector<StrokePacket>& packets,
                                             const std::vector<TextItem>& texts, const std::vector<ImagePacket>& images,
                                             const LayerDocument& layers,
                                             const std::vector<PageSizePacket>& pageSizes) {
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
            const auto pageSizeTransfer = staged.directory / "background-change.page-sizes";
            writeTransfer(ink, packets);
            writeTextTransfer(text, texts);
            writeLayerTransfer(layer, layers);
            writeImageTransfer(image, images);
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker, {"set-pdf-page-background", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), std::to_string(documentPage), std::to_string(pdfPage),
                               ink.string(), text.string(), layer.string(), image.string(), audioFolder.string(),
                               audioDirectory.string(), pageSizeTransfer.string()});
        } catch (...) {
            discardRevision(staged);
            throw;
        }
        return staged;
    }

    LayerActionTargetInfo inspectLayerActionTarget(const std::string& worker, int currentPage,
                                                   const std::vector<StrokePacket>& packets,
                                                   const std::vector<TextItem>& texts,
                                                   const std::vector<ImagePacket>& images,
                                                   const LayerDocument& layers) {
        if (currentPage < 0 || static_cast<size_t>(currentPage) >= pdfPageMap.size()) {
            throw std::runtime_error("Invalid source page for LayerActions");
        }

        const auto directory = path / ("layer-action-inspect-" + std::to_string(nextRevisionId++));
        std::filesystem::create_directories(directory);
        try {
            const auto ink = directory / "inspect.ink";
            const auto text = directory / "inspect.text";
            const auto layer = directory / "inspect.layers";
            const auto image = directory / "inspect.images";
            writeTransfer(ink, packets);
            writeTextTransfer(text, texts);
            writeLayerTransfer(layer, layers);
            writeImageTransfer(image, images);
            const auto result = runWorker(worker, {"layer-actions-inspect-target", snapshotPath().string(),
                                                   std::to_string(currentPage), ink.string(), text.string(),
                                                   layer.string(), image.string()});
            std::istringstream stream(result);
            std::string header;
            int hasNextPage = 0;
            int pdfPage = -1;
            int annotated = 0;
            if (!(stream >> header >> hasNextPage >> pdfPage >> annotated) ||
                header != "XOURNALPP_GTK4_LAYER_ACTION_TARGET_V1" ||
                (hasNextPage != 0 && hasNextPage != 1) || pdfPage < -1 || (annotated != 0 && annotated != 1)) {
                throw std::runtime_error("Invalid LayerActions target metadata from the document worker");
            }
            stream >> std::ws;
            if (!stream.eof() || (!hasNextPage && (pdfPage != -1 || annotated))) {
                throw std::runtime_error("Inconsistent LayerActions target metadata from the document worker");
            }
            return {hasNextPage == 1, pdfPage, annotated == 1};
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
            throw;
        }
    }

    StagedRevision stageLayerActionsCloneNext(const std::string& worker, int currentPage, int pdfPage,
                                              bool allowAnnotated, const std::vector<StrokePacket>& packets,
                                              const std::vector<TextItem>& texts,
                                              const std::vector<ImagePacket>& images,
                                              const LayerDocument& layers,
                                              const std::vector<PageSizePacket>& pageSizes) {
        if (currentPage < 0 || static_cast<size_t>(currentPage + 1) >= pdfPageMap.size() || pdfPage < 0 ||
            pdfPageMap[static_cast<size_t>(currentPage + 1)] != pdfPage) {
            throw std::runtime_error("The next LayerActions page changed before the operation could start");
        }

        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        const auto replacedPageHistoryDirectory =
                path / "page-history" / ("page-" + std::to_string(nextPageHistoryId++));
        const auto insertedPageHistoryDirectory =
                path / "page-history" / ("page-" + std::to_string(nextPageHistoryId++));
        staged.pageHistorySnapshotPath = replacedPageHistoryDirectory / "snapshot.xopp";
        staged.insertedPageHistorySnapshotPath = insertedPageHistoryDirectory / "snapshot.xopp";
        std::filesystem::create_directories(staged.directory);
        std::filesystem::create_directories(replacedPageHistoryDirectory);
        std::filesystem::create_directories(insertedPageHistoryDirectory);
        try {
            const auto ink = staged.directory / "layer-action.ink";
            const auto text = staged.directory / "layer-action.text";
            const auto layer = staged.directory / "layer-action.layers";
            const auto image = staged.directory / "layer-action.images";
            const auto pageSizeTransfer = staged.directory / "layer-action.page-sizes";
            writeTransfer(ink, packets);
            writeTextTransfer(text, texts);
            writeLayerTransfer(layer, layers);
            writeImageTransfer(image, images);
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker, {"layer-actions-clone-next", snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), std::to_string(currentPage), std::to_string(pdfPage),
                               allowAnnotated ? "1" : "0", ink.string(), text.string(), layer.string(),
                               image.string(), audioFolder.string(), audioDirectory.string(), pageSizeTransfer.string(),
                               staged.pageHistorySnapshotPath.string(),
                               staged.insertedPageHistorySnapshotPath.string()});
            static_cast<void>(readTransfer(staged.editableInkPath()));
            static_cast<void>(readTextTransfer(staged.editableTextPath()));
            static_cast<void>(readImageTransfer(staged.editableImagesPath()));
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
        for (const auto& snapshot: {staged.pageHistorySnapshotPath, staged.insertedPageHistorySnapshotPath}) {
            if (!snapshot.empty()) {
                std::error_code ec;
                std::filesystem::remove_all(snapshot.parent_path(), ec);
            }
        }
    }

    std::string originalSavePath;
    std::string originalVersion;
    std::vector<StrokePacket> initialInk;
    std::vector<TextItem> initialTexts;
    std::vector<ImagePacket> initialImages;
    std::vector<PageSizePacket> initialPageSizes;
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

    std::string pdfBackgroundFilename() const {
        const auto metadataPath = activeDir / "pdf-background-path.dat";
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(metadataPath, error)) || error) {
            return {};
        }
        try {
            return readDocumentPathTransfer(metadataPath);
        } catch (const std::exception&) {
            return {};
        }
    }

private:
    StagedRevision stagePageMutation(const std::string& worker, const char* command, int index,
                                     const std::vector<PageSizePacket>& pageSizes) {
        StagedRevision staged{path / ("rev-" + std::to_string(nextRevisionId++))};
        std::filesystem::create_directories(staged.directory);
        try {
            const auto pageSizeTransfer = staged.directory / "page-mutation.page-sizes";
            writePageSizeTransfer(pageSizeTransfer, pageSizes);
            runWorker(worker, {command, snapshotPath().string(), staged.snapshotPath().string(),
                               staged.previewPath().string(), std::to_string(index), audioFolder.string(),
                               audioDirectory.string(), pageSizeTransfer.string()});
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
    size_t nextPageHistoryId = 1;
};

}  // namespace prototype::bridge
