#include "LuaPluginRuntime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <system_error>

#include <glib.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace prototype {
namespace {

constexpr std::string_view pluginRegistryKey = "Gtk4Canvas_Plugin";
constexpr std::int64_t noPluginMode = std::numeric_limits<std::int64_t>::max();
constexpr std::size_t maxManifestBytes = 64 * 1024;

struct KeyFileDeleter {
    void operator()(GKeyFile* keyFile) const {
        if (keyFile) {
            g_key_file_unref(keyFile);
        }
    }
};

struct LuaDeleter {
    void operator()(lua_State* state) const {
        if (state) {
            lua_close(state);
        }
    }
};

using LuaState = std::unique_ptr<lua_State, LuaDeleter>;

std::string readString(GKeyFile* keyFile, const char* group, const char* key) {
    GError* error = nullptr;
    gchar* value = g_key_file_get_string(keyFile, group, key, &error);
    if (error) {
        g_clear_error(&error);
        return {};
    }
    std::string result = value ? value : "";
    g_free(value);
    return result;
}

std::vector<std::filesystem::path> splitPaths(std::string_view value) {
    std::vector<std::filesystem::path> paths;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto end = value.find(',', start);
        auto token = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
            token.remove_prefix(1);
        }
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
            token.remove_suffix(1);
        }
        if (!token.empty()) {
            paths.emplace_back(token);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return paths;
}

std::string normalizedPath(const std::filesystem::path& path) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        canonical = std::filesystem::absolute(path, error);
    }
    if (error) {
        canonical = path;
    }
    return canonical.lexically_normal().string();
}

std::string luaError(lua_State* state) {
    const char* message = lua_tostring(state, -1);
    return message ? message : "Unknown Lua error";
}

GdkModifierType normalizedModifiers(GdkModifierType modifiers) {
    return static_cast<GdkModifierType>(modifiers & gtk_accelerator_get_default_mod_mask());
}

bool matchesShortcut(guint shortcutKey, GdkModifierType shortcutModifiers, guint keyval, GdkModifierType modifiers) {
    const auto normalized = normalizedModifiers(modifiers);
    if (shortcutKey != gdk_keyval_to_lower(keyval)) {
        return false;
    }
    if (shortcutModifiers == normalized) {
        return true;
    }

    constexpr auto shift = GdkModifierType(GDK_SHIFT_MASK);
    return (shortcutModifiers & shift) != 0 && (normalized & shift) == 0 &&
           (shortcutModifiers & ~shift) == normalized && gdk_keyval_to_lower(keyval) != keyval;
}

constexpr std::array<std::string_view, 28> toolNames{
        "none",
        "pen",
        "eraser",
        "highlighter",
        "text",
        "image",
        "selectRect",
        "selectRegion",
        "selectMultiLayerRect",
        "selectMultiLayerRegion",
        "selectObject",
        "playObject",
        "verticalSpace",
        "hand",
        "drawRect",
        "drawEllipse",
        "drawArrow",
        "drawDoubleArrow",
        "drawCoordinateSystem",
        "showFloatingToolbox",
        "drawSpline",
        "selectPdfTextLinear",
        "selectPdfTextRect",
        "laserPointerPen",
        "laserPointerHighlighter",
        "link",
        "latex",
        "smartSelect",
};

int luaOpenApp(lua_State* state) {
    static const luaL_Reg functions[] = {
            {"registerUi", LuaPluginRuntime::luaRegisterUi},
            {"changeToolColor", LuaPluginRuntime::luaChangeToolColor},
            {"changeActionState", LuaPluginRuntime::luaChangeActionState},
            {"activateAction", LuaPluginRuntime::luaActivateAction},
            {"changeCurrentPageBackground", LuaPluginRuntime::luaChangeCurrentPageBackground},
            {"openDialog", LuaPluginRuntime::luaOpenDialog},
            {nullptr, nullptr},
    };

    luaL_newlib(state, functions);
    lua_newtable(state);
    for (std::size_t index = 0; index < toolNames.size(); ++index) {
        const std::string key = "Tool_" + std::string(toolNames[index]);
        lua_pushinteger(state, static_cast<lua_Integer>(index));
        lua_setfield(state, -2, key.c_str());
    }
    lua_setfield(state, -2, "C");
    return 1;
}

}  // namespace

struct LuaPluginRuntime::MenuEntry {
    Plugin* plugin = nullptr;
    std::string label;
    std::string callback;
    std::string parentPath;
    std::string accelerator;
    std::string shortcut;
    std::string toolbarId;
    std::string iconName;
    std::optional<std::int64_t> mode;
    guint shortcutKey = 0;
    GdkModifierType shortcutModifiers{};
};

struct LuaPluginRuntime::Plugin {
    LuaPluginRuntime* runtime = nullptr;
    std::string name;
    std::string description;
    std::string author;
    std::string version;
    std::string mainFile;
    std::filesystem::path path;
    std::string error;
    bool valid = false;
    bool defaultEnabled = false;
    bool enabled = false;
    bool pendingEnabled = false;
    bool inInitUi = false;
    bool loaded = false;
    LuaState state;
    std::vector<MenuEntry> menuEntries;
};

LuaPluginRuntime::LuaPluginRuntime() = default;
LuaPluginRuntime::~LuaPluginRuntime() = default;

void LuaPluginRuntime::discover(const std::vector<std::filesystem::path>& searchPaths, std::string_view enabledPaths,
                                std::string_view disabledPaths) {
    entries.clear();
    loaded = false;
    const auto enabled = splitPaths(enabledPaths);
    const auto disabled = splitPaths(disabledPaths);
    std::set<std::string> seen;

    for (const auto& root: searchPaths) {
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) {
            continue;
        }
        auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
        if (error) {
            canonicalRoot = root;
        }
        std::vector<std::filesystem::path> directories;
        try {
            for (const auto& item: std::filesystem::directory_iterator(canonicalRoot)) {
                if (item.is_directory()) {
                    directories.push_back(item.path());
                }
            }
        } catch (const std::filesystem::filesystem_error&) {
            continue;
        }
        std::sort(directories.begin(), directories.end());

        for (const auto& directory: directories) {
            const auto pathKey = normalizedPath(directory);
            if (!seen.insert(pathKey).second) {
                continue;
            }

            auto plugin = std::make_unique<Plugin>();
            plugin->runtime = this;
            plugin->name = directory.filename().string();
            plugin->path = directory;

            const auto manifestPath = directory / "plugin.ini";
            std::error_code manifestError;
            const auto manifestStatus = std::filesystem::symlink_status(manifestPath, manifestError);
            if (manifestError || !std::filesystem::is_regular_file(manifestStatus) ||
                std::filesystem::is_symlink(manifestStatus)) {
                plugin->error = "plugin.ini is missing or is not a regular file";
                entries.push_back(std::move(plugin));
                continue;
            }
            const auto manifestSize = std::filesystem::file_size(manifestPath, manifestError);
            if (manifestError || manifestSize > maxManifestBytes) {
                plugin->error = "plugin.ini exceeds the manifest size limit";
                entries.push_back(std::move(plugin));
                continue;
            }

            std::unique_ptr<GKeyFile, KeyFileDeleter> manifest(g_key_file_new());
            GError* loadError = nullptr;
            if (!g_key_file_load_from_file(manifest.get(), manifestPath.c_str(), G_KEY_FILE_NONE, &loadError)) {
                plugin->error = loadError ? loadError->message : "Cannot read plugin.ini";
                g_clear_error(&loadError);
                entries.push_back(std::move(plugin));
                continue;
            }

            plugin->author = readString(manifest.get(), "about", "author");
            plugin->description = readString(manifest.get(), "about", "description");
            plugin->version = readString(manifest.get(), "about", "version");
            plugin->mainFile = readString(manifest.get(), "plugin", "mainfile");
            plugin->defaultEnabled = readString(manifest.get(), "default", "enabled") == "true";
            plugin->enabled = plugin->defaultEnabled;
            if (std::find_if(enabled.begin(), enabled.end(),
                             [&](const auto& path) { return normalizedPath(path) == pathKey; }) != enabled.end()) {
                plugin->enabled = true;
            }
            if (std::find_if(disabled.begin(), disabled.end(),
                             [&](const auto& path) { return normalizedPath(path) == pathKey; }) != disabled.end()) {
                plugin->enabled = false;
            }
            plugin->pendingEnabled = plugin->enabled;

            if (plugin->mainFile.empty() || plugin->mainFile.find("..") != std::string::npos ||
                std::filesystem::path(plugin->mainFile).is_absolute()) {
                plugin->error = "Plugin mainfile is missing or contains an unsupported path";
                entries.push_back(std::move(plugin));
                continue;
            }
            const auto script = directory / plugin->mainFile;
            const auto scriptStatus = std::filesystem::symlink_status(script, manifestError);
            if (manifestError || std::filesystem::is_symlink(scriptStatus) ||
                !std::filesystem::is_regular_file(scriptStatus)) {
                plugin->error = "Plugin mainfile is missing or is not a regular file";
                entries.push_back(std::move(plugin));
                continue;
            }
            plugin->valid = true;
            entries.push_back(std::move(plugin));
        }
    }

    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs->name != rhs->name) {
            return lhs->name < rhs->name;
        }
        return normalizedPath(lhs->path) < normalizedPath(rhs->path);
    });
}

void LuaPluginRuntime::loadEnabled(PluginRuntimeHost pluginHost) {
    host = std::move(pluginHost);
    for (auto& entry: entries) {
        if (entry->enabled && entry->valid) {
            load(*entry);
        }
    }
    loaded = true;
}

std::vector<PluginSummary> LuaPluginRuntime::plugins() const {
    std::vector<PluginSummary> result;
    result.reserve(entries.size());
    for (const auto& entry: entries) {
        result.push_back(
                {entry->name, entry->description, entry->author, entry->version, entry->path, entry->error,
                 entry->valid, entry->enabled, entry->pendingEnabled, entry->loaded,
                 static_cast<std::size_t>(std::count_if(entry->menuEntries.begin(), entry->menuEntries.end(),
                                                        [](const MenuEntry& menu) { return !menu.label.empty(); })),
                 static_cast<std::size_t>(
                         std::count_if(entry->menuEntries.begin(), entry->menuEntries.end(),
                                       [](const MenuEntry& menu) { return !menu.toolbarId.empty(); }))});
    }
    return result;
}

std::optional<PluginSummary> LuaPluginRuntime::findPlugin(std::string_view name) const {
    const auto found =
            std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry->name == name; });
    if (found == entries.end()) {
        return std::nullopt;
    }
    const auto& entry = **found;
    return PluginSummary{
            entry.name,
            entry.description,
            entry.author,
            entry.version,
            entry.path,
            entry.error,
            entry.valid,
            entry.enabled,
            entry.pendingEnabled,
            entry.loaded,
            static_cast<std::size_t>(std::count_if(entry.menuEntries.begin(), entry.menuEntries.end(),
                                                   [](const MenuEntry& menu) { return !menu.label.empty(); })),
            static_cast<std::size_t>(std::count_if(entry.menuEntries.begin(), entry.menuEntries.end(),
                                                   [](const MenuEntry& menu) { return !menu.toolbarId.empty(); }))};
}

void LuaPluginRuntime::resetPendingStates() {
    for (auto& entry: entries) {
        entry->pendingEnabled = entry->enabled;
    }
}

void LuaPluginRuntime::setPendingEnabled(std::string_view name, bool enabled) {
    for (auto& entry: entries) {
        if (entry->name == name) {
            entry->pendingEnabled = enabled;
            return;
        }
    }
}

std::pair<std::string, std::string> LuaPluginRuntime::pendingPluginPaths() const {
    std::string enabled;
    std::string disabled;
    for (const auto& entry: entries) {
        if (!entry->valid || entry->pendingEnabled == entry->defaultEnabled) {
            continue;
        }
        auto& destination = entry->pendingEnabled ? enabled : disabled;
        if (!destination.empty()) {
            destination += ',';
        }
        destination += normalizedPath(entry->path);
    }
    return {std::move(enabled), std::move(disabled)};
}

bool LuaPluginRuntime::applyPendingStates() {
    bool changed = false;
    for (auto& entry: entries) {
        changed |= entry->enabled != entry->pendingEnabled;
        entry->enabled = entry->pendingEnabled;
    }
    return changed;
}

std::size_t LuaPluginRuntime::appendMenuSections(GMenu* destination, GActionMap* actionMap,
                                                 GtkApplication* application) {
    if (!destination || !actionMap) {
        return 0;
    }

    std::size_t actionId = 0;
    for (auto& plugin: entries) {
        if (!plugin->loaded) {
            continue;
        }
        auto* root = g_menu_new();
        std::map<std::string, GMenu*> pathMenus{{"", root}};
        std::vector<GMenu*> menusToUnref{root};

        const auto normalizeParentPath = [](std::string_view path) {
            std::string normalized;
            bool previousSeparator = true;
            for (const char character: path) {
                if (character == '/') {
                    if (!previousSeparator) {
                        normalized += character;
                    }
                    previousSeparator = true;
                } else {
                    normalized += character;
                    previousSeparator = false;
                }
            }
            if (!normalized.empty() && normalized.back() == '/') {
                normalized.pop_back();
            }
            return normalized;
        };
        const auto getOrCreateParent = [&](auto&& self, const std::string& path) -> GMenu* {
            if (const auto found = pathMenus.find(path); found != pathMenus.end()) {
                return found->second;
            }
            const auto separator = path.rfind('/');
            const auto parentPath = separator == std::string::npos ? std::string{} : path.substr(0, separator);
            const auto label = separator == std::string::npos ? path : path.substr(separator + 1);
            auto* parent = self(self, parentPath);
            auto* child = g_menu_new();
            menusToUnref.push_back(child);
            auto* item = g_menu_item_new_submenu(label.c_str(), G_MENU_MODEL(child));
            g_menu_append_item(parent, item);
            g_object_unref(item);
            pathMenus.emplace(path, child);
            return child;
        };

        for (auto& menu: plugin->menuEntries) {
            if (menu.label.empty()) {
                continue;
            }
            const auto actionName = "gtk4-plugin-" + std::to_string(actionId++);
            const auto detailedActionName = "win." + actionName;
            auto* action = g_simple_action_new(actionName.c_str(), nullptr);
            g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
                                 auto* entry = static_cast<MenuEntry*>(data);
                                 entry->plugin->runtime->call(*entry->plugin, entry->callback, entry->mode);
                             }),
                             &menu);
            g_action_map_add_action(actionMap, G_ACTION(action));
            g_object_unref(action);

            auto* parent = getOrCreateParent(getOrCreateParent, normalizeParentPath(menu.parentPath));
            auto* item = g_menu_item_new(menu.label.c_str(), detailedActionName.c_str());
            g_menu_append_item(parent, item);
            g_object_unref(item);
            if (!menu.accelerator.empty() && application) {
                const char* accelerators[] = {menu.accelerator.c_str(), nullptr};
                gtk_application_set_accels_for_action(application, detailedActionName.c_str(), accelerators);
            }
        }

        if (g_menu_model_get_n_items(G_MENU_MODEL(root)) > 0) {
            auto* pluginItem = g_menu_item_new_submenu(plugin->name.c_str(), G_MENU_MODEL(root));
            g_menu_append_item(destination, pluginItem);
            g_object_unref(pluginItem);
        }
        for (auto* menu: menusToUnref) {
            g_object_unref(menu);
        }
    }
    return actionId;
}

bool LuaPluginRuntime::handleKeyPress(guint keyval, GdkModifierType modifiers, bool textEditing) {
    if (textEditing) {
        return false;
    }
    for (auto& plugin: entries) {
        if (!plugin->loaded) {
            continue;
        }
        for (auto& menu: plugin->menuEntries) {
            if (menu.shortcutKey == 0 ||
                !matchesShortcut(menu.shortcutKey, menu.shortcutModifiers, keyval, modifiers)) {
                continue;
            }
            call(*plugin, menu.callback, menu.mode);
            return true;
        }
    }
    return false;
}

bool LuaPluginRuntime::invokeMenu(std::string_view pluginName, std::size_t menuIndex) {
    for (auto& plugin: entries) {
        if (plugin->name != pluginName || !plugin->loaded) {
            continue;
        }
        std::size_t visibleIndex = 0;
        for (auto& menu: plugin->menuEntries) {
            if (menu.label.empty()) {
                continue;
            }
            if (visibleIndex++ == menuIndex) {
                return call(*plugin, menu.callback, menu.mode);
            }
        }
    }
    return false;
}

void LuaPluginRuntime::load(Plugin& plugin) {
    plugin.state.reset(luaL_newstate());
    if (!plugin.state) {
        plugin.error = "Could not allocate a Lua state";
        return;
    }
    luaL_openlibs(plugin.state.get());

    lua_getglobal(plugin.state.get(), "package");
    lua_getfield(plugin.state.get(), -1, "path");
    const char* oldPath = lua_tostring(plugin.state.get(), -1);
    const auto pluginLuaPath = (plugin.path / "?.lua").string() + ";" + (oldPath ? oldPath : "");
    lua_pop(plugin.state.get(), 1);
    lua_pushstring(plugin.state.get(), pluginLuaPath.c_str());
    lua_setfield(plugin.state.get(), -2, "path");
    lua_pop(plugin.state.get(), 1);

    lua_pushlightuserdata(plugin.state.get(), &plugin);
    lua_setfield(plugin.state.get(), LUA_REGISTRYINDEX, pluginRegistryKey.data());
    luaL_requiref(plugin.state.get(), "app", luaOpenApp, 1);
    lua_pop(plugin.state.get(), 1);

    const auto scriptPath = (plugin.path / plugin.mainFile).string();
    if (luaL_loadfile(plugin.state.get(), scriptPath.c_str()) != LUA_OK) {
        plugin.error = luaError(plugin.state.get());
        lua_pop(plugin.state.get(), 1);
        reportError(plugin, plugin.error);
        return;
    }
    if (lua_pcall(plugin.state.get(), 0, 0, 0) != LUA_OK) {
        plugin.error = luaError(plugin.state.get());
        lua_pop(plugin.state.get(), 1);
        reportError(plugin, plugin.error);
        return;
    }

    lua_getglobal(plugin.state.get(), "initUi");
    if (lua_isfunction(plugin.state.get(), -1)) {
        plugin.inInitUi = true;
        const int status = lua_pcall(plugin.state.get(), 0, 0, 0);
        plugin.inInitUi = false;
        if (status != LUA_OK) {
            plugin.error = luaError(plugin.state.get());
            lua_pop(plugin.state.get(), 1);
            plugin.menuEntries.clear();
            reportError(plugin, plugin.error);
            return;
        }
    } else {
        lua_pop(plugin.state.get(), 1);
    }

    plugin.loaded = true;
}

bool LuaPluginRuntime::call(Plugin& plugin, std::string_view function, std::optional<std::int64_t> mode) {
    if (!plugin.state || function.empty()) {
        return false;
    }
    const std::string functionName(function);
    lua_getglobal(plugin.state.get(), functionName.c_str());
    if (!lua_isfunction(plugin.state.get(), -1)) {
        lua_pop(plugin.state.get(), 1);
        plugin.error = "Callback is missing or is not a function: " + functionName;
        reportError(plugin, plugin.error);
        return false;
    }
    const int argumentCount = mode ? 1 : 0;
    if (mode) {
        lua_pushinteger(plugin.state.get(), *mode);
    }
    if (lua_pcall(plugin.state.get(), argumentCount, 0, 0) != LUA_OK) {
        plugin.error = luaError(plugin.state.get());
        lua_pop(plugin.state.get(), 1);
        reportError(plugin, plugin.error);
        return false;
    }
    plugin.error.clear();
    return true;
}

bool LuaPluginRuntime::call(Plugin& plugin, std::string_view function, const char* argument) {
    if (!plugin.state || function.empty()) {
        return false;
    }
    const std::string functionName(function);
    lua_getglobal(plugin.state.get(), functionName.c_str());
    if (!lua_isfunction(plugin.state.get(), -1)) {
        lua_pop(plugin.state.get(), 1);
        plugin.error = "Callback is missing or is not a function: " + functionName;
        reportError(plugin, plugin.error);
        return false;
    }
    lua_pushstring(plugin.state.get(), argument ? argument : "");
    if (lua_pcall(plugin.state.get(), 1, 0, 0) != LUA_OK) {
        plugin.error = luaError(plugin.state.get());
        lua_pop(plugin.state.get(), 1);
        reportError(plugin, plugin.error);
        return false;
    }
    plugin.error.clear();
    return true;
}

bool LuaPluginRuntime::changeToolColor(const std::optional<std::string>& tool, std::uint32_t color, bool selection,
                                       std::string& error) {
    if (!host.changeToolColor) {
        error = "Color changes are unavailable in this GTK 4 host";
        return false;
    }
    return host.changeToolColor(tool, color, selection, error);
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

bool LuaPluginRuntime::openDialog(Plugin& plugin, std::string_view message,
                                  const std::vector<PluginDialogButton>& buttons, bool isError,
                                  std::string_view callback, std::string& error) {
    if (!host.openDialog) {
        error = "Plugin dialogs are unavailable in this GTK 4 host";
        return false;
    }

    auto* pluginEntry = &plugin;
    const std::string callbackName(callback);
    return host.openDialog(plugin.name, message, buttons, isError,
                           [this, pluginEntry, callbackName](int response) {
                               if (response >= 1 && !callbackName.empty()) {
                                   call(*pluginEntry, callbackName, static_cast<std::int64_t>(response));
                               }
                           },
                           error);
}

void LuaPluginRuntime::reportError(const Plugin& plugin, std::string_view message) const {
    g_warning("Plugin '%s': %.*s", plugin.name.c_str(), static_cast<int>(message.size()), message.data());
    if (host.reportError) {
        host.reportError(plugin.name, message);
    }
}

LuaPluginRuntime::Plugin* LuaPluginRuntime::pluginForState(lua_State* state) {
    lua_getfield(state, LUA_REGISTRYINDEX, pluginRegistryKey.data());
    auto* plugin = static_cast<Plugin*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return plugin;
}

int LuaPluginRuntime::luaRegisterUi(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin || !plugin->inInitUi) {
        return luaL_error(state, "registerUi needs to be called within initUi()");
    }
    luaL_checktype(state, 1, LUA_TTABLE);

    char error[512]{};
    {
        const auto getString = [state, &error](const char* key, bool required) -> std::optional<std::string> {
            lua_getfield(state, 1, key);
            std::optional<std::string> value;
            if (!lua_isnil(state, -1)) {
                if (!lua_isstring(state, -1)) {
                    std::snprintf(error, sizeof(error), "registerUi field '%s' must be a string", key);
                } else {
                    value = lua_tostring(state, -1);
                }
            } else if (required) {
                std::snprintf(error, sizeof(error), "registerUi is missing required field '%s'", key);
            }
            lua_pop(state, 1);
            return value;
        };

        MenuEntry entry;
        entry.label = getString("menu", false).value_or("");
        entry.callback = getString("callback", true).value_or("");
        entry.parentPath = getString("parentPath", false).value_or("");
        entry.accelerator = getString("accelerator", false).value_or("");
        entry.shortcut = getString("shortcut", false).value_or("");
        entry.toolbarId = getString("toolbarId", false).value_or("");
        entry.iconName = getString("iconName", false).value_or("");
        if (error[0] == '\0' && entry.label.empty() && entry.toolbarId.empty()) {
            std::snprintf(error, sizeof(error), "registerUi needs a menu label or toolbarId");
        }

        lua_getfield(state, 1, "mode");
        if (!lua_isnil(state, -1)) {
            if (!lua_isinteger(state, -1)) {
                std::snprintf(error, sizeof(error), "registerUi field 'mode' must be an integer");
            } else {
                entry.mode = static_cast<std::int64_t>(lua_tointeger(state, -1));
            }
        }
        lua_pop(state, 1);

        if (error[0] == '\0' && !entry.accelerator.empty()) {
            guint keyval = 0;
            GdkModifierType modifiers{};
            gtk_accelerator_parse(entry.accelerator.c_str(), &keyval, &modifiers);
            if (keyval == 0) {
                std::snprintf(error, sizeof(error), "Invalid menu accelerator: '%s'", entry.accelerator.c_str());
            }
        }
        if (error[0] == '\0' && !entry.shortcut.empty()) {
            guint keyval = 0;
            GdkModifierType modifiers{};
            gtk_accelerator_parse(entry.shortcut.c_str(), &keyval, &modifiers);
            if (keyval == 0) {
                std::snprintf(error, sizeof(error), "Invalid canvas shortcut: '%s'", entry.shortcut.c_str());
            } else {
                entry.shortcutKey = gdk_keyval_to_lower(keyval);
                entry.shortcutModifiers = normalizedModifiers(modifiers);
            }
        }

        if (error[0] == '\0') {
            entry.plugin = plugin;
            const auto menuIndex = plugin->menuEntries.size();
            plugin->menuEntries.push_back(std::move(entry));
            lua_settop(state, 1);
            lua_createtable(state, 0, 1);
            lua_pushinteger(state, static_cast<lua_Integer>(menuIndex));
            lua_setfield(state, -2, "menuId");
            return 1;
        }
    }
    lua_pushstring(state, error);
    return lua_error(state);
}

int LuaPluginRuntime::luaChangeToolColor(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_getfield(state, 1, "selection");
    bool selection = false;
    if (lua_isboolean(state, -1)) {
        selection = lua_toboolean(state, -1);
    } else if (!lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return luaL_error(state, "'selection' must be a boolean or nil");
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "tool");
    const char* toolName = nullptr;
    if (lua_type(state, -1) == LUA_TSTRING) {
        toolName = lua_tostring(state, -1);
    } else if (!lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return luaL_error(state, "'tool' must be a string or nil");
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "color");
    std::uint32_t color = 0;
    if (lua_isinteger(state, -1)) {
        const auto value = lua_tointeger(state, -1);
        if (value < 0 || value > 0xffffff) {
            lua_pop(state, 1);
            return luaL_error(state, "Color must be an RGB hex code in the range 0x000000..0xFFFFFF");
        }
        color = static_cast<std::uint32_t>(value);
    } else if (!lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return luaL_error(state, "'color' must be an integer RGB hex code or nil");
    }
    lua_pop(state, 1);

    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    bool changed = false;
    char error[512]{};
    {
        std::optional<std::string> tool;
        if (toolName) {
            tool = toolName;
        }
        std::string hostError;
        changed = plugin->runtime->changeToolColor(tool, color, selection, hostError);
        if (!changed) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Tool color change is unavailable" : hostError.c_str());
        }
    }
    if (!changed) {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaChangeActionState(lua_State* state) {
    const char* action = luaL_checkstring(state, 1);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    char error[512]{};
    bool changed = false;
    {
        PluginActionValue value;
        switch (lua_type(state, 2)) {
            case LUA_TNIL:
                break;
            case LUA_TBOOLEAN:
                value = static_cast<bool>(lua_toboolean(state, 2));
                break;
            case LUA_TNUMBER:
                value = lua_isinteger(state, 2) ?
                                PluginActionValue{static_cast<std::int64_t>(lua_tointeger(state, 2))} :
                                PluginActionValue{static_cast<double>(lua_tonumber(state, 2))};
                break;
            case LUA_TSTRING:
                value = std::string(lua_tostring(state, 2));
                break;
            default:
                std::snprintf(error, sizeof(error),
                              "changeActionState expects a boolean, number, string, or nil state");
                break;
        }
        if (error[0] == '\0') {
            std::string hostError;
            changed = plugin->runtime->changeActionState(action, value, hostError);
            if (!changed) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Action state change is unavailable" : hostError.c_str());
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaActivateAction(lua_State* state) {
    const char* action = luaL_checkstring(state, 1);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    bool activated = false;
    char error[512]{};
    {
        std::string hostError;
        activated = plugin->runtime->activateAction(action, hostError);
        if (!activated) {
            std::snprintf(error, sizeof(error), "%s", hostError.empty() ? "Action is unavailable" : hostError.c_str());
        }
    }
    if (!activated) {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaChangeCurrentPageBackground(lua_State* state) {
    const char* format = luaL_checkstring(state, 1);
    const char* config = luaL_optstring(state, 2, "");
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    bool changed = false;
    char error[512]{};
    {
        std::string hostError;
        changed = plugin->runtime->changeCurrentPageBackground(format, config, hostError);
        if (!changed) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Page background change is unavailable" : hostError.c_str());
        }
    }
    if (!changed) {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaOpenDialog(lua_State* state) {
    const char* message = luaL_checkstring(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);
    const char* callback = luaL_optstring(state, 3, "");
    const bool isError = lua_toboolean(state, 4);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    bool opened = false;
    {
        std::string messageText(message);
        std::string callbackName(callback);
        std::vector<PluginDialogButton> buttons;
        lua_pushnil(state);
        while (lua_next(state, 2) != 0) {
            if (!lua_isinteger(state, -2) || lua_type(state, -1) != LUA_TSTRING) {
                std::snprintf(error, sizeof(error), "openDialog options must map integer responses to button labels");
                lua_pop(state, 2);
                break;
            }
            const auto response = lua_tointeger(state, -2);
            if (response < std::numeric_limits<int>::min() || response > std::numeric_limits<int>::max()) {
                std::snprintf(error, sizeof(error), "openDialog response values must fit in an integer");
                lua_pop(state, 2);
                break;
            }
            const char* label = lua_tostring(state, -1);
            buttons.push_back({static_cast<int>(response), label ? label : ""});
            lua_pop(state, 1);
        }
        std::sort(buttons.begin(), buttons.end(),
                  [](const PluginDialogButton& lhs, const PluginDialogButton& rhs) {
                      return lhs.response < rhs.response;
                  });
        if (error[0] == '\0') {
            std::string hostError;
            opened = plugin->runtime->openDialog(*plugin, messageText, buttons, isError, callbackName, hostError);
            if (!opened) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Could not open plugin dialog" : hostError.c_str());
            }
        }
    }
    if (!opened) {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

}  // namespace prototype
