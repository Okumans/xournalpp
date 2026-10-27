#include "CanvasState.h"

namespace prototype {

void CanvasState::insertPageAfter(int afterIndex, PageSize size, bool shiftHistory) {
    if (pages.empty()) {
        pages.push_back({pageWidth, pageHeight});
    }
    while (pageLayers.size() < pages.size()) {
        pageLayers.emplace_back();
    }
    const int clamped = std::clamp(afterIndex, 0, pageCount() - 1);
    const int insertedIndex = clamped + 1;
    insertPageAt(insertedIndex, size, PageLayers{});
    if (shiftHistory) {
        for (auto& edit: undoStates) {
            shiftEditPageIndices(edit, insertedIndex);
        }
        for (auto& edit: redoStates) {
            shiftEditPageIndices(edit, insertedIndex);
        }
    }
    selectActivePage(insertedIndex);
}

void CanvasState::recordPageInsertion(int pageIndex, int currentPageIndex, std::string sourceSnapshotPath) {
    if (pageIndex < 0 || pageIndex >= pageCount()) {
        throw std::out_of_range("Inserted page is outside the document");
    }
    if (currentPageIndex < 0) {
        currentPageIndex = pageIndex;
    }
    if (currentPageIndex < 0 || currentPageIndex >= pageCount()) {
        throw std::out_of_range("Inserted page source is outside the document");
    }
    Edit edit;
    edit.pageStructureChange = PageStructureChange{capturePage(currentPageIndex), true};
    edit.pageStructureChange->page.pageIndex = pageIndex;
    edit.pageStructureChange->page.sourceSnapshotPath = std::move(sourceSnapshotPath);
    forEachPageObject(edit.pageStructureChange->page, [pageIndex](auto& object) { object.page = pageIndex; });
    record(std::move(edit));
}

bool CanvasState::deletePageWithHistory(int removeIndex, std::string pageSnapshotPath) {
    if (pageCount() <= 1 || removeIndex < 0 || removeIndex >= pageCount() || pageSnapshotPath.empty()) {
        return false;
    }
    Edit edit;
    auto snapshot = capturePage(removeIndex);
    snapshot.sourceSnapshotPath = std::move(pageSnapshotPath);
    edit.pageStructureChange = PageStructureChange{std::move(snapshot), false};
    record(std::move(edit));
    return deletePageAt(removeIndex, false);
}

bool CanvasState::nextUndoPageStructureChange(bool& pageExistsAfter, int& pageIndex,
                                              std::string& sourceSnapshotPath) const {
    return readPageStructureChange(undoStates, pageExistsAfter, pageIndex, sourceSnapshotPath);
}

bool CanvasState::nextRedoPageStructureChange(bool& pageExistsAfter, int& pageIndex,
                                              std::string& sourceSnapshotPath) const {
    return readPageStructureChange(redoStates, pageExistsAfter, pageIndex, sourceSnapshotPath);
}

std::vector<std::string> CanvasState::pageHistorySnapshotPaths() const {
    std::vector<std::string> paths;
    auto collect = [&paths](const std::vector<Edit>& edits) {
        for (const auto& edit: edits) {
            if (edit.pageStructureChange && !edit.pageStructureChange->page.sourceSnapshotPath.empty()) {
                paths.push_back(edit.pageStructureChange->page.sourceSnapshotPath);
            }
        }
    };
    collect(undoStates);
    collect(redoStates);
    return paths;
}

bool CanvasState::replaceNextPageWithClone(int sourcePage, PageSize replacementSize,
                                           std::string insertedPageSnapshotPath, std::string replacedPageSnapshotPath) {
    if (sourcePage < 0 || sourcePage + 1 >= pageCount() || static_cast<size_t>(sourcePage) >= pageLayers.size() ||
        insertedPageSnapshotPath.empty() || replacedPageSnapshotPath.empty()) {
        return false;
    }

    const int insertedPage = sourcePage + 1;
    auto clonedLayers = pageLayers[static_cast<size_t>(sourcePage)];
    clonedLayers.backgroundVisible = true;
    clonedLayers.backgroundName.reset();
    clonedLayers.backgroundFormat = ":pdf";
    clonedLayers.backgroundConfig.clear();
    clonedLayers.gridXOffset = clonedLayers.gridYOffset = 0;
    clonedLayers.gridXStep = clonedLayers.gridYStep = 0;
    for (auto& layer: clonedLayers.layers) {
        layer.visible = true;
    }
    std::vector<Ink> clonedInk;
    std::vector<TextItem> clonedTexts;
    std::vector<CanvasImage> clonedImages;
    for (const auto& stroke: ink) {
        if (stroke.page == sourcePage) {
            auto copy = stroke;
            copy.page = insertedPage;
            clonedInk.push_back(std::move(copy));
        }
    }
    for (const auto& text: texts) {
        if (text.page == sourcePage) {
            auto copy = text;
            copy.page = insertedPage;
            clonedTexts.push_back(std::move(copy));
        }
    }
    for (const auto& image: images) {
        if (image.page == sourcePage) {
            auto copy = image;
            copy.page = insertedPage;
            clonedImages.push_back(std::move(copy));
        }
    }

    insertPageAfter(sourcePage, replacementSize);
    pageLayers[static_cast<size_t>(insertedPage)] = clonedLayers;
    ink.insert(ink.end(), std::make_move_iterator(clonedInk.begin()), std::make_move_iterator(clonedInk.end()));
    texts.insert(texts.end(), std::make_move_iterator(clonedTexts.begin()), std::make_move_iterator(clonedTexts.end()));
    images.insert(images.end(), std::make_move_iterator(clonedImages.begin()),
                  std::make_move_iterator(clonedImages.end()));
    recordPageInsertion(insertedPage, insertedPage, std::move(insertedPageSnapshotPath));
    if (!deletePageWithHistory(insertedPage + 1, std::move(replacedPageSnapshotPath))) {
        return false;
    }
    selectActivePage(insertedPage);
    return true;
}

bool CanvasState::deletePageAt(int removeIndex, bool clearHistory) {
    if (pageCount() <= 1 || removeIndex < 0 || removeIndex >= pageCount()) {
        return false;
    }
    cancel();
    clearLaserPointers();
    clearSelection();
    pages.erase(pages.begin() + removeIndex);
    ++pageStructureGeneration;
    rebuildPageLayout();
    if (static_cast<size_t>(removeIndex) < pageLayers.size()) {
        pageLayers.erase(pageLayers.begin() + removeIndex);
    }
    std::erase_if(ink, [&](const Ink& stroke) { return stroke.page == removeIndex; });
    for (auto& stroke: ink) {
        if (stroke.page > removeIndex) {
            --stroke.page;
        }
    }
    std::erase_if(texts, [&](const TextItem& text) { return text.page == removeIndex; });
    for (auto& text: texts) {
        if (text.page > removeIndex) {
            --text.page;
        }
    }
    std::erase_if(images, [&](const CanvasImage& image) { return image.page == removeIndex; });
    for (auto& image: images) {
        if (image.page > removeIndex) {
            --image.page;
        }
    }
    if (clearHistory) {
        undoStates.clear();
        redoStates.clear();
        revisionBase = ++nextRevision;
        notifyHistoryChanged();
    }
    selectActivePage(std::min(removeIndex, pageCount() - 1));
    return true;
}

void CanvasState::undo() {
    cancel();
    if (!undoStates.empty()) {
        auto& edit = undoStates.back();
        if (edit.pageStructureChange && edit.pageStructureChange->pageExistsAfter) {
            refreshPageStructureSnapshot(*edit.pageStructureChange);
        }
        apply(edit, false);
        redoStates.push_back(std::move(undoStates.back()));
        undoStates.pop_back();
    }
}

void CanvasState::redo() {
    cancel();
    if (!redoStates.empty()) {
        auto& edit = redoStates.back();
        if (edit.pageStructureChange && !edit.pageStructureChange->pageExistsAfter) {
            refreshPageStructureSnapshot(*edit.pageStructureChange);
        }
        apply(edit, true);
        undoStates.push_back(std::move(redoStates.back()));
        redoStates.pop_back();
    }
}

bool CanvasState::canUndo() const { return !undoStates.empty(); }

bool CanvasState::canRedo() const { return !redoStates.empty(); }

size_t CanvasState::revision() const { return undoStates.empty() ? revisionBase : undoStates.back().revision; }

CanvasState::PageSnapshot CanvasState::capturePage(int pageIndex) const {
    if (pageIndex < 0 || pageIndex >= pageCount() || static_cast<size_t>(pageIndex) >= pageLayers.size()) {
        throw std::out_of_range("Page snapshot is outside the document");
    }
    PageSnapshot snapshot;
    snapshot.pageIndex = pageIndex;
    snapshot.size = pages.at(static_cast<size_t>(pageIndex));
    snapshot.layers = pageLayers.at(static_cast<size_t>(pageIndex));
    for (size_t index = 0; index < ink.size(); ++index) {
        if (ink[index].page == pageIndex) {
            snapshot.ink.push_back({index, ink[index]});
        }
    }
    for (size_t index = 0; index < texts.size(); ++index) {
        if (texts[index].page == pageIndex) {
            snapshot.texts.push_back({index, texts[index]});
        }
    }
    for (size_t index = 0; index < images.size(); ++index) {
        if (images[index].page == pageIndex) {
            snapshot.images.push_back({index, images[index]});
        }
    }
    return snapshot;
}

void CanvasState::refreshPageStructureSnapshot(CanvasState::PageStructureChange& change) const {
    const auto sourceSnapshotPath = change.page.sourceSnapshotPath;
    auto snapshot = capturePage(change.page.pageIndex);
    snapshot.sourceSnapshotPath = sourceSnapshotPath;
    change.page = std::move(snapshot);
}

bool CanvasState::readPageStructureChange(const std::vector<CanvasState::Edit>& edits, bool& pageExistsAfter,
                                          int& pageIndex, std::string& sourceSnapshotPath) {
    if (edits.empty() || !edits.back().pageStructureChange) {
        return false;
    }
    const auto& change = *edits.back().pageStructureChange;
    pageExistsAfter = change.pageExistsAfter;
    pageIndex = change.page.pageIndex;
    sourceSnapshotPath = change.page.sourceSnapshotPath;
    return true;
}

void CanvasState::insertPageAt(int pageIndex, PageSize size, const PageLayers& layers) {
    pageIndex = std::clamp(pageIndex, 0, pageCount());
    cancel();
    clearLaserPointers();
    pages.insert(pages.begin() + pageIndex, size);
    ++pageStructureGeneration;
    pageLayers.insert(pageLayers.begin() + pageIndex, layers);
    for (auto& stroke: ink) {
        if (stroke.page >= pageIndex) {
            ++stroke.page;
        }
    }
    shiftTexts(texts, pageIndex);
    shiftImages(images, pageIndex);
    selectActivePage(pageIndex);
    rebuildPageLayout();
}

void CanvasState::restorePageSnapshot(const CanvasState::PageSnapshot& snapshot) {
    clearSelection();
    insertPageAt(snapshot.pageIndex, snapshot.size, snapshot.layers);
    restoreIndexedObjects(ink, snapshot.pageIndex, snapshot.ink);
    restoreIndexedObjects(texts, snapshot.pageIndex, snapshot.texts);
    restoreIndexedObjects(images, snapshot.pageIndex, snapshot.images);
    selectActivePage(snapshot.pageIndex);
}

void CanvasState::record(CanvasState::Edit edit) {
    const bool discardsPageSnapshot = std::any_of(redoStates.begin(), redoStates.end(), [](const Edit& entry) {
        return entry.pageStructureChange && !entry.pageStructureChange->page.sourceSnapshotPath.empty();
    });
    redoStates.clear();
    edit.revision = ++nextRevision;
    // Keep ink and text deltas, rather than a full document copy for each edit.
    undoStates.push_back(std::move(edit));
    if (discardsPageSnapshot) {
        notifyHistoryChanged();
    }
}

void CanvasState::notifyHistoryChanged() const {
    if (historyChangedCallback) {
        historyChangedCallback(pageHistorySnapshotPaths());
    }
}

void CanvasState::apply(const CanvasState::Edit& edit, bool forward) {
    if (edit.pageStructureChange) {
        const auto& change = *edit.pageStructureChange;
        if (forward == change.pageExistsAfter) {
            restorePageSnapshot(change.page);
        } else {
            static_cast<void>(deletePageAt(change.page.pageIndex, false));
        }
        return;
    }
    std::optional<int> changedBackgroundPage;
    if (edit.pageSizeChange) {
        const auto& change = *edit.pageSizeChange;
        if (change.page >= 0 && static_cast<size_t>(change.page) < pages.size()) {
            pages[static_cast<size_t>(change.page)] = forward ? change.after : change.before;
            rebuildPageLayout();
            if (change.page == page) {
                pageWidth = pages[static_cast<size_t>(page)].width;
                pageHeight = pages[static_cast<size_t>(page)].height;
            }
        }
    }
    if (edit.layerMutation) {
        const auto& mutation = *edit.layerMutation;
        const auto& layers = forward ? mutation.afterLayers : mutation.beforeLayers;
        auto& pageState = pageLayers.at(static_cast<size_t>(mutation.page));
        if (pageState.backgroundFormat != layers.backgroundFormat ||
            pageState.backgroundConfig != layers.backgroundConfig) {
            changedBackgroundPage = mutation.page;
        }
        pageState = layers;
        restoreIndexedObjects(ink, mutation.page, forward ? mutation.afterInk : mutation.beforeInk);
        restoreIndexedObjects(texts, mutation.page, forward ? mutation.afterTexts : mutation.beforeTexts);
        restoreIndexedObjects(images, mutation.page, forward ? mutation.afterImages : mutation.beforeImages);
    }
    for (auto it = edit.textChanges.rbegin(); it != edit.textChanges.rend(); ++it) {
        if (forward) {
            const auto pos = texts.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex);
            texts.erase(pos, pos + static_cast<std::ptrdiff_t>(it->before.size()));
            texts.insert(texts.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex), it->after.begin(),
                         it->after.end());
        } else {
            const auto pos = texts.begin() + static_cast<std::ptrdiff_t>(it->afterIndex);
            texts.erase(pos, pos + static_cast<std::ptrdiff_t>(it->after.size()));
            texts.insert(texts.begin() + static_cast<std::ptrdiff_t>(it->afterIndex), it->before.begin(),
                         it->before.end());
        }
    }
    for (auto it = edit.imageChanges.rbegin(); it != edit.imageChanges.rend(); ++it) {
        if (forward) {
            const auto pos = images.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex);
            images.erase(pos, pos + static_cast<std::ptrdiff_t>(it->before.size()));
            images.insert(images.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex), it->after.begin(),
                          it->after.end());
        } else {
            const auto pos = images.begin() + static_cast<std::ptrdiff_t>(it->afterIndex);
            images.erase(pos, pos + static_cast<std::ptrdiff_t>(it->after.size()));
            images.insert(images.begin() + static_cast<std::ptrdiff_t>(it->afterIndex), it->before.begin(),
                          it->before.end());
        }
    }
    for (auto it = edit.changes.rbegin(); it != edit.changes.rend(); ++it) {
        if (forward) {
            const auto pos = ink.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex);
            ink.erase(pos, pos + static_cast<std::ptrdiff_t>(it->before.size()));
            auto inserted = it->after;
            for (auto& stroke: inserted) {
                stroke.rasterCacheKey = 0;
            }
            ink.insert(ink.begin() + static_cast<std::ptrdiff_t>(it->beforeIndex),
                       std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));
        } else {
            const auto pos = ink.begin() + static_cast<std::ptrdiff_t>(it->afterIndex);
            ink.erase(pos, pos + static_cast<std::ptrdiff_t>(it->after.size()));
            auto inserted = it->before;
            for (auto& stroke: inserted) {
                stroke.rasterCacheKey = 0;
            }
            ink.insert(ink.begin() + static_cast<std::ptrdiff_t>(it->afterIndex),
                       std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));
        }
    }
    const auto oldSelection = selected;
    const auto oldTextSelection = selectedTexts;
    const auto oldImageSelection = selectedImages;
    selected = (forward ? edit.selectionAfter : edit.selectionBefore).value_or(std::vector<size_t>{});
    selectedTexts = (forward ? edit.textSelectionAfter : edit.textSelectionBefore).value_or(std::vector<size_t>{});
    selectedImages = (forward ? edit.imageSelectionAfter : edit.imageSelectionBefore).value_or(std::vector<size_t>{});
    if (edit.hasSelectionFrameState) {
        selectionFrame = forward ? edit.selectionFrameAfter : edit.selectionFrameBefore;
    } else if (selected != oldSelection || selectedTexts != oldTextSelection || selectedImages != oldImageSelection) {
        selectionFrame.reset();
    }
    if (changedBackgroundPage && backgroundChangedCallback) {
        backgroundChangedCallback(*changedBackgroundPage);
    }
}

}  // namespace prototype
