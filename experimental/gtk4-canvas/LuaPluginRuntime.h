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

struct lua_State;

namespace prototype {

using PluginActionValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct PluginDialogButton {
    int response = 0;
    std::string label;
};

struct PluginRuntimeHost {
    std::function<bool(const std::optional<std::string>&, std::uint32_t, bool, std::string&)> changeToolColor;
    std::function<bool(std::string_view, const PluginActionValue&, std::string&)> changeActionState;
    std::function<bool(std::string_view, std::string&)> activateAction;
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
    bool openDialog(Plugin& plugin, std::string_view message, const std::vector<PluginDialogButton>& buttons,
                    bool isError, std::string_view callback, std::string& error);
    void reportError(const Plugin& plugin, std::string_view message) const;
    static Plugin* pluginForState(lua_State* state);

    std::vector<std::unique_ptr<Plugin>> entries;
    PluginRuntimeHost host;
    bool loaded = false;
};

}  // namespace prototype
