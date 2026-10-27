#include <cmath>

#include "LuaPluginRuntime.h"

namespace prototype {

bool LuaPluginRuntime::changeToolColor(const std::optional<std::string>& tool, std::uint32_t color, bool selection,
                                       std::string& error) {
    if (!host.changeToolColor) {
        error = "Color changes are unavailable in this GTK 4 host";
        return false;
    }
    return host.changeToolColor(tool, color, selection, error);
}

bool LuaPluginRuntime::getActionState(std::string_view action, PluginActionValue& value, std::string& error) const {
    if (!host.getActionState) {
        error = "Action state queries are unavailable in this GTK 4 host";
        return false;
    }
    return host.getActionState(action, value, error);
}

bool LuaPluginRuntime::getToolInfo(std::string_view tool, PluginToolInfo& info, std::string& error) const {
    if (!host.getToolInfo) {
        error = "Tool information is unavailable in this GTK 4 host";
        return false;
    }
    return host.getToolInfo(tool, info, error);
}

bool LuaPluginRuntime::changeActionState(std::string_view action, const PluginActionValue& value, std::string& error) {
    if (!host.changeActionState) {
        error = "Action state changes are unavailable in this GTK 4 host";
        return false;
    }
    return host.changeActionState(action, value, error);
}

bool LuaPluginRuntime::activateAction(std::string_view action, std::string& error) {
    if (!host.activateAction) {
        error = "Actions are unavailable in this GTK 4 host";
        return false;
    }
    return host.activateAction(action, error);
}

bool LuaPluginRuntime::changeCurrentPageBackground(std::string_view format, std::string_view config,
                                                   std::string& error) {
    if (!host.changeCurrentPageBackground) {
        error = "Page background changes are unavailable in this GTK 4 host";
        return false;
    }
    return host.changeCurrentPageBackground(format, config, error);
}

bool LuaPluginRuntime::addEmptyPages(bool force, std::string& error) {
    if (!host.addEmptyPages) {
        error = "Adding note pages is unavailable in this GTK 4 host";
        return false;
    }
    return host.addEmptyPages(force, error);
}

bool LuaPluginRuntime::transformSelectedImages(std::int64_t action, std::string& error) {
    if (!host.transformSelectedImages) {
        error = "Image actions are unavailable in this GTK 4 host";
        return false;
    }
    return host.transformSelectedImages(action, error);
}

bool LuaPluginRuntime::getDisplayDpi(int& dpi, std::string& error) const {
    if (!host.getDisplayDpi) {
        error = "Display DPI queries are unavailable in this GTK 4 host";
        return false;
    }
    dpi = host.getDisplayDpi();
    if (dpi <= 0) {
        error = "The GTK 4 host returned an invalid display DPI value";
        return false;
    }
    return true;
}

bool LuaPluginRuntime::getPageLabel(std::size_t pageNumber, std::string& label, std::string& error) const {
    if (!host.getPageLabel) {
        error = "PDF page labels are unavailable in this GTK 4 host";
        return false;
    }
    return host.getPageLabel(pageNumber, label, error);
}

bool LuaPluginRuntime::getColorPalette(std::vector<PluginPaletteColor>& palette, std::string& error) const {
    if (!host.getColorPalette) {
        error = "Color palette queries are unavailable in this GTK 4 host";
        return false;
    }
    palette = host.getColorPalette();
    return true;
}

bool LuaPluginRuntime::refreshPage(std::string& error) {
    if (!host.refreshPage) {
        error = "Page refresh is unavailable in this GTK 4 host";
        return false;
    }
    return host.refreshPage(error);
}

bool LuaPluginRuntime::getFont(PluginFont& font, std::string& error) const {
    if (!host.getFont) {
        error = "Text font queries are unavailable in this GTK 4 host";
        return false;
    }
    font = host.getFont();
    if (!std::isfinite(font.size) || font.size <= 0) {
        error = "The GTK 4 host returned an invalid text font";
        return false;
    }
    return true;
}

bool LuaPluginRuntime::setFont(const PluginFont& font, std::string& error) {
    if (!host.setFont) {
        error = "Text font changes are unavailable in this GTK 4 host";
        return false;
    }
    if (!std::isfinite(font.size) || font.size <= 0) {
        error = "Font size must be a finite positive number";
        return false;
    }
    return host.setFont(font, error);
}

bool LuaPluginRuntime::getScrollPosition(PluginScrollPosition& position, std::string& error) const {
    if (!host.getScrollPosition) {
        error = "Scroll position queries are unavailable in this GTK 4 host";
        return false;
    }
    position = host.getScrollPosition();
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.width) ||
        !std::isfinite(position.height) || position.x < 0 || position.y < 0 || position.width < 0 ||
        position.height < 0) {
        error = "The GTK 4 host returned an invalid scroll position";
        return false;
    }
    return true;
}

bool LuaPluginRuntime::scrollToPage(int page, bool relative, std::string& error) {
    if (!host.scrollToPage) {
        error = "Page scrolling is unavailable in this GTK 4 host";
        return false;
    }
    return host.scrollToPage(page, relative, error);
}

bool LuaPluginRuntime::scrollToPosition(double x, double y, bool relative, std::string& error) {
    if (!host.scrollToPosition) {
        error = "Canvas scrolling is unavailable in this GTK 4 host";
        return false;
    }
    if (!std::isfinite(x) || !std::isfinite(y)) {
        error = "Scroll positions must be finite numbers";
        return false;
    }
    return host.scrollToPosition(x, y, relative, error);
}

bool LuaPluginRuntime::getZoom(double& zoom, std::string& error) const {
    if (!host.getZoom) {
        error = "Zoom queries are unavailable in this GTK 4 host";
        return false;
    }
    zoom = host.getZoom();
    if (!std::isfinite(zoom)) {
        error = "The GTK 4 host returned an invalid zoom value";
        return false;
    }
    return true;
}

bool LuaPluginRuntime::setZoom(double zoom, std::string& error) {
    if (!host.setZoom) {
        error = "Zoom changes are unavailable in this GTK 4 host";
        return false;
    }
    if (!std::isfinite(zoom)) {
        error = "Zoom must be a finite number";
        return false;
    }
    return host.setZoom(zoom, error);
}

bool LuaPluginRuntime::getStrokes(std::string_view type, std::vector<PluginStroke>& strokes, std::string& error) {
    if (!host.getStrokes) {
        error = "Stroke access is unavailable in this GTK 4 host";
        return false;
    }
    return host.getStrokes(type, strokes, error);
}

bool LuaPluginRuntime::getTexts(std::string_view type, std::vector<PluginText>& texts, std::string& error) {
    if (!host.getTexts) {
        error = "Text access is unavailable in this GTK 4 host";
        return false;
    }
    return host.getTexts(type, texts, error);
}

bool LuaPluginRuntime::getLinks(std::string_view type, std::vector<PluginLink>& links, std::string& error) {
    if (!host.getLinks) {
        error = "Link access is unavailable in this GTK 4 host";
        return false;
    }
    return host.getLinks(type, links, error);
}

bool LuaPluginRuntime::getImages(std::string_view type, std::vector<PluginImage>& images, std::string& error) {
    if (!host.getImages) {
        error = "Image access is unavailable in this GTK 4 host";
        return false;
    }
    return host.getImages(type, images, error);
}

bool LuaPluginRuntime::addToSelection(const std::vector<std::uintptr_t>& references, std::string& error) {
    if (!host.addToSelection) {
        error = "Adding elements to the selection is unavailable in this GTK 4 host";
        return false;
    }
    return host.addToSelection(references, error);
}

bool LuaPluginRuntime::clearSelection(std::string& error) {
    if (!host.clearSelection) {
        error = "Clearing the selection is unavailable in this GTK 4 host";
        return false;
    }
    host.clearSelection();
    return true;
}

bool LuaPluginRuntime::addStrokes(const std::vector<PluginStrokeInput>& strokes, PluginStrokeUndoPolicy undoPolicy,
                                  std::vector<std::uintptr_t>& references, std::string& error) {
    if (!host.addStrokes) {
        error = "Adding strokes is unavailable in this GTK 4 host";
        return false;
    }
    return host.addStrokes(strokes, undoPolicy, references, error);
}

bool LuaPluginRuntime::addTexts(const std::vector<PluginTextInput>& texts, PluginStrokeUndoPolicy undoPolicy,
                                std::vector<std::uintptr_t>& references, std::string& error) {
    if (!host.addTexts) {
        error = "Adding text is unavailable in this GTK 4 host";
        return false;
    }
    return host.addTexts(texts, undoPolicy, references, error);
}

bool LuaPluginRuntime::addLinks(const std::vector<PluginLinkInput>& links, PluginStrokeUndoPolicy undoPolicy,
                                std::vector<std::uintptr_t>& references, std::string& error) {
    if (!host.addLinks) {
        error = "Adding links is unavailable in this GTK 4 host";
        return false;
    }
    return host.addLinks(links, undoPolicy, references, error);
}

bool LuaPluginRuntime::addImages(const std::vector<PluginImageInput>& images, PluginStrokeUndoPolicy undoPolicy,
                                 std::vector<PluginImageResult>& results, std::string& error) {
    if (!host.addImages) {
        error = "Adding images is unavailable in this GTK 4 host";
        return false;
    }
    return host.addImages(images, undoPolicy, results, error);
}

bool LuaPluginRuntime::setPageSize(double width, double height, bool relative, std::string& error) {
    if (!host.setPageSize) {
        error = "Changing page size is unavailable in this GTK 4 host";
        return false;
    }
    return host.setPageSize(width, height, relative, error);
}

bool LuaPluginRuntime::setCurrentPage(std::size_t pageNumber, std::string& error) {
    if (!host.setCurrentPage) {
        error = "Page selection is unavailable in this GTK 4 host";
        return false;
    }
    return host.setCurrentPage(pageNumber, error);
}

bool LuaPluginRuntime::setCurrentLayer(std::size_t layerNumber, bool updateVisibility, std::string& error) {
    if (!host.setCurrentLayer) {
        error = "Layer selection is unavailable in this GTK 4 host";
        return false;
    }
    return host.setCurrentLayer(layerNumber, updateVisibility, error);
}

bool LuaPluginRuntime::setLayerVisibility(bool visible, std::string& error) {
    if (!host.setLayerVisibility) {
        error = "Layer visibility changes are unavailable in this GTK 4 host";
        return false;
    }
    return host.setLayerVisibility(visible, error);
}

bool LuaPluginRuntime::setCurrentLayerName(std::string_view name, std::string& error) {
    if (!host.setCurrentLayerName) {
        error = "Layer rename is unavailable in this GTK 4 host";
        return false;
    }
    return host.setCurrentLayerName(name, error);
}

bool LuaPluginRuntime::setBackgroundName(std::string_view name, std::string& error) {
    if (!host.setBackgroundName) {
        error = "Background naming is unavailable in this GTK 4 host";
        return false;
    }
    return host.setBackgroundName(name, error);
}

bool LuaPluginRuntime::exportDocument(const DocumentExportOptions& options, std::string& error) {
    if (!host.exportDocument) {
        error = "Document export is unavailable in this GTK 4 host";
        return false;
    }
    return host.exportDocument(options, error);
}

bool LuaPluginRuntime::quickScreenshot(std::string_view plugin, std::string& error) {
    if (!host.quickScreenshot) {
        error = "Screen-region capture is unavailable in this GTK 4 host";
        return false;
    }
    return host.quickScreenshot(plugin, error);
}

std::string LuaPluginRuntime::documentFilename() const {
    return host.documentFilename ? host.documentFilename() : std::string{};
}

PluginDocumentStructure LuaPluginRuntime::documentStructure() const {
    if (host.documentStructure) {
        return host.documentStructure();
    }
    PluginDocumentStructure structure;
    structure.xoppFilename = documentFilename();
    return structure;
}

}  // namespace prototype
