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

enum class PluginStrokeUndoPolicy { Grouped, Individual, None };

struct PluginRuntimeHost {
    std::function<bool(const std::optional<std::string>&, std::uint32_t, bool, std::string&)> changeToolColor;
    std::function<bool(std::string_view, const PluginActionValue&, std::string&)> changeActionState;
    std::function<bool(std::string_view, std::string&)> activateAction;
    std::function<bool(std::string_view, std::string_view, std::string&)> changeCurrentPageBackground;
    std::function<bool(bool, std::string&)> addEmptyPages;
    std::function<bool(std::int64_t, std::string&)> transformSelectedImages;
    std::function<bool(std::string_view, std::vector<PluginStroke>&, std::string&)> getStrokes;
    std::function<bool(const std::vector<PluginStrokeInput>&, PluginStrokeUndoPolicy,
                       std::vector<std::uintptr_t>&, std::string&)> addStrokes;
    std::function<bool(double, double, bool, std::string&)> setPageSize;
    std::function<bool(bool, std::string&)> setLayerVisibility;
    std::function<bool(std::string_view, std::string&)> setCurrentLayerName;
    std::function<bool(const DocumentExportOptions&, std::string&)> exportDocument;
    std::function<std::string()> documentFilename;
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
    static int luaGetStrokes(lua_State* state);
    static int luaAddStrokes(lua_State* state);
    static int luaSetPageSize(lua_State* state);
    static int luaSetLayerVisibility(lua_State* state);
    static int luaSetCurrentLayerName(lua_State* state);
    static int luaExport(lua_State* state);
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
    bool getStrokes(std::string_view type, std::vector<PluginStroke>& strokes, std::string& error);
    bool addStrokes(const std::vector<PluginStrokeInput>& strokes, PluginStrokeUndoPolicy undoPolicy,
                    std::vector<std::uintptr_t>& references, std::string& error);
    bool setPageSize(double width, double height, bool relative, std::string& error);
    bool setLayerVisibility(bool visible, std::string& error);
    bool setCurrentLayerName(std::string_view name, std::string& error);
    bool exportDocument(const DocumentExportOptions& options, std::string& error);
    std::string documentFilename() const;
    bool openDialog(Plugin& plugin, std::string_view message, const std::vector<PluginDialogButton>& buttons,
                    bool isError, std::string_view callback, std::string& error);
    void reportError(const Plugin& plugin, std::string_view message) const;
    static Plugin* pluginForState(lua_State* state);

    std::vector<std::unique_ptr<Plugin>> entries;
    PluginRuntimeHost host;
    bool loaded = false;
};

}  // namespace prototype
