#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "DocumentExport.h"

struct lua_State;

namespace prototype {

using PluginActionValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct PluginDialogButton {
    int response = 0;
    std::string label;
};

struct PluginDocumentLayer {
    bool visible = true;
    std::string name;
    std::optional<bool> isAnnotated;
};

struct PluginDocumentPage {
    double width = 595;
    double height = 842;
    bool isAnnotated = false;
    std::string pageTypeFormat = "plain";
    std::string pageTypeConfig;
    std::uint32_t backgroundColor = 0xffffff;
    int pdfBackgroundPageNo = 0;
    std::vector<PluginDocumentLayer> layers;
    std::size_t currentLayer = 1;
};

struct PluginDocumentStructure {
    std::vector<PluginDocumentPage> pages;
    std::size_t currentPage = 1;
    std::string pdfBackgroundFilename;
    std::string xoppFilename;
};

struct PluginStroke {
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> pressure;
    std::string tool;
    double width = 1;
    std::uint32_t color = 0;
    int fill = -1;
    std::string lineStyle = "plain";
    std::uintptr_t reference = 0;
    std::optional<std::size_t> page;
    std::optional<std::size_t> layer;
};

struct PluginStrokeInput {
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> pressure;
    std::optional<std::string> tool;
    std::optional<double> width;
    std::optional<std::uint32_t> color;
    std::optional<int> fill;
    std::optional<std::string> lineStyle;
};

struct PluginTextInput {
    std::string text;
    std::optional<std::string> fontName;
    std::optional<double> fontSize;
    std::optional<std::uint32_t> color;
    double x = 0;
    double y = 0;
    std::optional<double> wrap;
};

struct PluginLinkInput {
    std::string text;
    std::string url;
    std::optional<int> alignment;
    std::optional<std::string> fontName;
    std::optional<double> fontSize;
    std::optional<std::uint32_t> color;
    double x = 0;
    double y = 0;
};

struct PluginText {
    std::string text;
    std::string fontName;
    double fontSize = 12;
    std::uint32_t color = 0;
    double x = 0;
    double y = 0;
    std::optional<double> wrap;
    double width = 0;
    double height = 0;
    std::uintptr_t reference = 0;
    std::optional<std::size_t> page;
    std::optional<std::size_t> layer;
};

struct PluginLink {
    std::string text;
    std::string url;
    int alignment = 0;
    std::string fontName;
    double fontSize = 12;
    std::uint32_t color = 0;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    std::uintptr_t reference = 0;
    std::optional<std::size_t> page;
    std::optional<std::size_t> layer;
};

struct PluginImage {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    // The source image owns these bytes; Lua copies them before the query returns.
    std::string_view data;
    std::string format;
    int imageWidth = 0;
    int imageHeight = 0;
    std::uintptr_t reference = 0;
    std::optional<std::size_t> page;
    std::optional<std::size_t> layer;
};

struct PluginImageInput {
    std::optional<std::string> path;
    std::optional<std::string> data;
    double x = 0;
    double y = 0;
    int maxWidth = -1;
    int maxHeight = -1;
    double scale = 1;
    bool aspectRatio = true;
};

struct PluginImageResult {
    std::optional<std::uintptr_t> reference;
    std::string error;
};

struct PluginPaletteColor {
    std::string name;
    std::uint32_t color = 0;
};

enum class PluginStrokeUndoPolicy { Grouped, Individual, None };

struct PluginRuntimeHost {
    std::function<bool(const std::optional<std::string>&, std::uint32_t, bool, std::string&)> changeToolColor;
    std::function<bool(std::string_view, const PluginActionValue&, std::string&)> changeActionState;
    std::function<bool(std::string_view, std::string&)> activateAction;
    std::function<bool(std::string_view, std::string_view, std::string&)> changeCurrentPageBackground;
    std::function<bool(bool, std::string&)> addEmptyPages;
    std::function<bool(std::int64_t, std::string&)> transformSelectedImages;
    std::function<int()> getDisplayDpi;
    std::function<bool(std::size_t, std::string&, std::string&)> getPageLabel;
    std::function<std::vector<PluginPaletteColor>()> getColorPalette;
    std::function<double()> getZoom;
    std::function<bool(double, std::string&)> setZoom;
    std::function<bool(std::string_view, std::vector<PluginStroke>&, std::string&)> getStrokes;
    std::function<bool(std::string_view, std::vector<PluginText>&, std::string&)> getTexts;
    std::function<bool(std::string_view, std::vector<PluginLink>&, std::string&)> getLinks;
    std::function<bool(std::string_view, std::vector<PluginImage>&, std::string&)> getImages;
    std::function<bool(const std::vector<std::uintptr_t>&, std::string&)> addToSelection;
    std::function<void()> clearSelection;
    std::function<bool(const std::vector<PluginStrokeInput>&, PluginStrokeUndoPolicy,
                       std::vector<std::uintptr_t>&, std::string&)> addStrokes;
    std::function<bool(const std::vector<PluginTextInput>&, PluginStrokeUndoPolicy,
                       std::vector<std::uintptr_t>&, std::string&)> addTexts;
    std::function<bool(const std::vector<PluginLinkInput>&, PluginStrokeUndoPolicy,
                       std::vector<std::uintptr_t>&, std::string&)> addLinks;
    std::function<bool(const std::vector<PluginImageInput>&, PluginStrokeUndoPolicy,
                       std::vector<PluginImageResult>&, std::string&)> addImages;
    std::function<bool(double, double, bool, std::string&)> setPageSize;
    std::function<bool(std::size_t, std::string&)> setCurrentPage;
    std::function<bool(bool, std::string&)> setLayerVisibility;
    std::function<bool(std::string_view, std::string&)> setCurrentLayerName;
    std::function<bool(const DocumentExportOptions&, std::string&)> exportDocument;
    std::function<bool(std::string_view, std::string&)> quickScreenshot;
    std::function<std::string()> documentFilename;
    std::function<PluginDocumentStructure()> documentStructure;
    std::function<bool(std::string_view, std::string_view, const std::vector<PluginDialogButton>&, bool,
                       std::function<void(int)>, std::string&)>
            openDialog;
    std::function<void(std::string_view, std::string_view)> reportError;
};

struct PluginSummary {
    std::string name;
    std::string description;
    std::string author;
    std::string version;
    std::filesystem::path path;
    std::string error;
    bool valid = false;
    bool enabled = false;
    bool pendingEnabled = false;
    bool loaded = false;
    std::size_t menuItems = 0;
    std::size_t toolbarItems = 0;
};

class LuaPluginRuntime final {
public:
    LuaPluginRuntime();
    ~LuaPluginRuntime();

    LuaPluginRuntime(const LuaPluginRuntime&) = delete;
    LuaPluginRuntime& operator=(const LuaPluginRuntime&) = delete;

    void discover(const std::vector<std::filesystem::path>& searchPaths, std::string_view enabledPaths,
                  std::string_view disabledPaths);
    void loadEnabled(PluginRuntimeHost host);

    std::vector<PluginSummary> plugins() const;
    std::optional<PluginSummary> findPlugin(std::string_view name) const;
    void resetPendingStates();
    void setPendingEnabled(std::string_view name, bool enabled);
    std::pair<std::string, std::string> pendingPluginPaths() const;
    bool applyPendingStates();

    std::size_t appendMenuSections(GMenu* destination, GActionMap* actionMap, GtkApplication* application);
    bool handleKeyPress(guint keyval, GdkModifierType modifiers, bool textEditing);
    bool invokeMenu(std::string_view pluginName, std::size_t menuIndex);

    static int luaRegisterUi(lua_State* state);
    static int luaChangeToolColor(lua_State* state);
    static int luaChangeActionState(lua_State* state);
    static int luaActivateAction(lua_State* state);
    static int luaChangeCurrentPageBackground(lua_State* state);
    static int luaAddEmptyPages(lua_State* state);
    static int luaTransformSelectedImages(lua_State* state);
    static int luaGetFolder(lua_State* state);
    static int luaGetDisplayDpi(lua_State* state);
    static int luaGetPageLabel(lua_State* state);
    static int luaGetColorPalette(lua_State* state);
    static int luaGetZoom(lua_State* state);
    static int luaSetZoom(lua_State* state);
    static int luaGetStrokes(lua_State* state);
    static int luaGetTexts(lua_State* state);
    static int luaGetLinks(lua_State* state);
    static int luaGetImages(lua_State* state);
    static int luaAddToSelection(lua_State* state);
    static int luaClearSelection(lua_State* state);
    static int luaAddStrokes(lua_State* state);
    static int luaAddSplines(lua_State* state);
    static int luaAddTexts(lua_State* state);
    static int luaAddLinks(lua_State* state);
    static int luaAddImages(lua_State* state);
    static int luaSetPageSize(lua_State* state);
    static int luaSetCurrentPage(lua_State* state);
    static int luaSetLayerVisibility(lua_State* state);
    static int luaSetCurrentLayerName(lua_State* state);
    static int luaExport(lua_State* state);
    static int luaQuickScreenshot(lua_State* state);
    static int luaGetDocumentStructure(lua_State* state);
    static int luaOpenDialog(lua_State* state);

private:
    struct Plugin;
    struct MenuEntry;

    void load(Plugin& plugin);
    bool call(Plugin& plugin, std::string_view function, std::optional<std::int64_t> mode = std::nullopt);
    bool call(Plugin& plugin, std::string_view function, const char* argument);
    bool changeToolColor(const std::optional<std::string>& tool, std::uint32_t color, bool selection,
                         std::string& error);
    bool changeActionState(std::string_view action, const PluginActionValue& value, std::string& error);
    bool activateAction(std::string_view action, std::string& error);
    bool changeCurrentPageBackground(std::string_view format, std::string_view config, std::string& error);
    bool addEmptyPages(bool force, std::string& error);
    bool transformSelectedImages(std::int64_t action, std::string& error);
    bool getDisplayDpi(int& dpi, std::string& error) const;
    bool getPageLabel(std::size_t pageNumber, std::string& label, std::string& error) const;
    bool getColorPalette(std::vector<PluginPaletteColor>& palette, std::string& error) const;
    bool getZoom(double& zoom, std::string& error) const;
    bool setZoom(double zoom, std::string& error);
    bool getStrokes(std::string_view type, std::vector<PluginStroke>& strokes, std::string& error);
    bool getTexts(std::string_view type, std::vector<PluginText>& texts, std::string& error);
    bool getLinks(std::string_view type, std::vector<PluginLink>& links, std::string& error);
    bool getImages(std::string_view type, std::vector<PluginImage>& images, std::string& error);
    bool addToSelection(const std::vector<std::uintptr_t>& references, std::string& error);
    bool clearSelection(std::string& error);
    bool addStrokes(const std::vector<PluginStrokeInput>& strokes, PluginStrokeUndoPolicy undoPolicy,
                    std::vector<std::uintptr_t>& references, std::string& error);
    bool addTexts(const std::vector<PluginTextInput>& texts, PluginStrokeUndoPolicy undoPolicy,
                  std::vector<std::uintptr_t>& references, std::string& error);
    bool addLinks(const std::vector<PluginLinkInput>& links, PluginStrokeUndoPolicy undoPolicy,
                  std::vector<std::uintptr_t>& references, std::string& error);
    bool addImages(const std::vector<PluginImageInput>& images, PluginStrokeUndoPolicy undoPolicy,
                   std::vector<PluginImageResult>& results, std::string& error);
    bool setPageSize(double width, double height, bool relative, std::string& error);
    bool setCurrentPage(std::size_t pageNumber, std::string& error);
    bool setLayerVisibility(bool visible, std::string& error);
    bool setCurrentLayerName(std::string_view name, std::string& error);
    bool exportDocument(const DocumentExportOptions& options, std::string& error);
    bool quickScreenshot(std::string_view plugin, std::string& error);
    std::string documentFilename() const;
    PluginDocumentStructure documentStructure() const;
    bool openDialog(Plugin& plugin, std::string_view message, const std::vector<PluginDialogButton>& buttons,
                    bool isError, std::string_view callback, std::string& error);
    void reportError(const Plugin& plugin, std::string_view message) const;
    static Plugin* pluginForState(lua_State* state);

    std::vector<std::unique_ptr<Plugin>> entries;
    PluginRuntimeHost host;
    bool loaded = false;
};

}  // namespace prototype
