#include "LuaPluginRuntime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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

#include "CanvasDrawing.h"

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

bool readLuaStringField(lua_State* state, int tableIndex, const char* field, std::optional<std::string>& value,
                        bool required, char* error, std::size_t errorSize) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, absoluteIndex, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (required) {
            std::snprintf(error, errorSize, "Missing '%s' string", field);
            return false;
        }
        return true;
    }
    if (!lua_isstring(state, -1)) {
        std::snprintf(error, errorSize, "Field '%s' must be a string", field);
        lua_pop(state, 1);
        return false;
    }
    std::size_t length = 0;
    const char* bytes = lua_tolstring(state, -1, &length);
    value = std::string(bytes, length);
    lua_pop(state, 1);
    return true;
}

bool readLuaNumberField(lua_State* state, int tableIndex, const char* field, std::optional<double>& value,
                        bool required, char* error, std::size_t errorSize) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, absoluteIndex, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (required) {
            std::snprintf(error, errorSize, "Missing numeric '%s' field", field);
            return false;
        }
        return true;
    }
    if (!lua_isnumber(state, -1)) {
        std::snprintf(error, errorSize, "Field '%s' must be a number", field);
        lua_pop(state, 1);
        return false;
    }
    const double number = lua_tonumber(state, -1);
    lua_pop(state, 1);
    if (!std::isfinite(number)) {
        std::snprintf(error, errorSize, "Field '%s' must be finite", field);
        return false;
    }
    value = number;
    return true;
}

bool readLuaFont(lua_State* state, int tableIndex, std::optional<std::string>& fontName,
                 std::optional<double>& fontSize, char* error, std::size_t errorSize) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, absoluteIndex, "font");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1)) {
        std::snprintf(error, errorSize, "'font' value must be a table");
        lua_pop(state, 1);
        return false;
    }

    const int fontIndex = lua_absindex(state, -1);
    const bool valid = readLuaStringField(state, fontIndex, "name", fontName, false, error, errorSize) &&
                       readLuaNumberField(state, fontIndex, "size", fontSize, false, error, errorSize);
    lua_pop(state, 1);
    if (valid && fontSize && *fontSize <= 0) {
        std::snprintf(error, errorSize, "Font size must be positive");
        return false;
    }
    return valid;
}

bool readLuaColor(lua_State* state, int tableIndex, std::optional<std::uint32_t>& color,
                  char* error, std::size_t errorSize) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, absoluteIndex, "color");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isinteger(state, -1)) {
        std::snprintf(error, errorSize, "'color' must be an integer RGB hex code or unset");
        lua_pop(state, 1);
        return false;
    }
    const lua_Integer value = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (value < 0 || value > 0xffffff) {
        std::snprintf(error, errorSize, "Color must be in the range 0x000000..0xFFFFFF");
        return false;
    }
    color = static_cast<std::uint32_t>(value);
    return true;
}

bool readLuaBinaryField(lua_State* state, int tableIndex, const char* field, std::optional<std::string>& value,
                        char* error, std::size_t errorSize) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, absoluteIndex, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
        std::snprintf(error, errorSize, "Field '%s' must be a string", field);
        lua_pop(state, 1);
        return false;
    }
    std::size_t length = 0;
    const char* bytes = lua_tolstring(state, -1, &length);
    value = std::string(bytes, length);
    lua_pop(state, 1);
    return true;
}

bool readLuaIntegerField(lua_State* state, int tableIndex, const char* field, std::optional<lua_Integer>& value,
                         char* error, std::size_t errorSize) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, absoluteIndex, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    int isInteger = 0;
    const lua_Integer integer = lua_tointegerx(state, -1, &isInteger);
    lua_pop(state, 1);
    if (!isInteger) {
        std::snprintf(error, errorSize, "Field '%s' must be an integer", field);
        return false;
    }
    value = integer;
    return true;
}

bool readLuaUndoPolicy(lua_State* state, int tableIndex, PluginStrokeUndoPolicy& policy,
                       char* error, std::size_t errorSize) {
    const int absoluteIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, absoluteIndex, "allowUndoRedoAction");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        policy = PluginStrokeUndoPolicy::Grouped;
        return true;
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
        std::snprintf(error, errorSize, "Undo policy must be 'grouped', 'individual', or 'none'");
        lua_pop(state, 1);
        return false;
    }
    const std::string_view value = lua_tostring(state, -1);
    lua_pop(state, 1);
    if (value == "grouped") {
        policy = PluginStrokeUndoPolicy::Grouped;
    } else if (value == "individual") {
        policy = PluginStrokeUndoPolicy::Individual;
    } else if (value == "none") {
        policy = PluginStrokeUndoPolicy::None;
    } else {
        std::snprintf(error, errorSize, "Unrecognized undo option: %.*s", static_cast<int>(value.size()), value.data());
        return false;
    }
    return true;
}

struct CubicSplineSegment {
    Point start;
    Point firstControl;
    Point secondControl;
    Point end;
};

Point interpolateSplinePoint(const Point& start, const Point& end, float t) {
    const double amount = t;
    return {start.x * (1 - amount) + end.x * amount,
            start.y * (1 - amount) + end.y * amount};
}

std::pair<CubicSplineSegment, CubicSplineSegment> subdivideSpline(const CubicSplineSegment& segment, float t) {
    const Point b0 = interpolateSplinePoint(segment.start, segment.firstControl, t);
    const Point b1 = interpolateSplinePoint(segment.firstControl, segment.secondControl, t);
    const Point b2 = interpolateSplinePoint(segment.secondControl, segment.end, t);
    const Point c0 = interpolateSplinePoint(b0, b1, t);
    const Point c1 = interpolateSplinePoint(b1, b2, t);
    const Point midpoint = interpolateSplinePoint(c0, c1, t);
    return {{segment.start, b0, c0, midpoint}, {midpoint, c1, b2, segment.end}};
}

bool splineSegmentIsFlatEnough(const CubicSplineSegment& segment) {
    const auto distance = [](const Point& first, const Point& second) {
        return std::hypot(first.x - second.x, first.y - second.y);
    };
    const double firstEdge = distance(segment.start, segment.firstControl);
    const double controlEdge = distance(segment.firstControl, segment.secondControl);
    const double lastEdge = distance(segment.secondControl, segment.end);
    const double chord = distance(segment.start, segment.end);
    if (!std::isfinite(firstEdge) || !std::isfinite(controlEdge) || !std::isfinite(lastEdge) ||
        !std::isfinite(chord)) {
        return false;
    }
    constexpr double flatnessTolerance = 1.0001;
    constexpr double minimumKnotDistance = 0.3;
    return chord < minimumKnotDistance || firstEdge + controlEdge + lastEdge < flatnessTolerance * chord;
}

bool appendSplinePoints(const CubicSplineSegment& segment, std::vector<Point>& points, std::size_t maxPoints,
                        unsigned int depth = 0) {
    if (splineSegmentIsFlatEnough(segment)) {
        if (points.size() >= maxPoints) {
            return false;
        }
        // The upstream rasterizer emits each flat leaf's start and omits the final endpoint.
        points.push_back(segment.start);
        return true;
    }
    // Bound pathological coordinates while preserving the upstream flattening for normal curves.
    constexpr unsigned int maxSubdivisionDepth = 24;
    if (depth >= maxSubdivisionDepth) {
        return false;
    }
    const auto [first, second] = subdivideSpline(segment, 0.5F);
    return appendSplinePoints(first, points, maxPoints, depth + 1) &&
           appendSplinePoints(second, points, maxPoints, depth + 1);
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
            {"addEmptyPages", LuaPluginRuntime::luaAddEmptyPages},
            {"transformSelectedImages", LuaPluginRuntime::luaTransformSelectedImages},
            {"getDisplayDpi", LuaPluginRuntime::luaGetDisplayDpi},
            {"getZoom", LuaPluginRuntime::luaGetZoom},
            {"setZoom", LuaPluginRuntime::luaSetZoom},
            {"getStrokes", LuaPluginRuntime::luaGetStrokes},
            {"getTexts", LuaPluginRuntime::luaGetTexts},
            {"getLinks", LuaPluginRuntime::luaGetLinks},
            {"getImages", LuaPluginRuntime::luaGetImages},
            {"addToSelection", LuaPluginRuntime::luaAddToSelection},
            {"clearSelection", LuaPluginRuntime::luaClearSelection},
            {"addStrokes", LuaPluginRuntime::luaAddStrokes},
            {"addSplines", LuaPluginRuntime::luaAddSplines},
            {"addTexts", LuaPluginRuntime::luaAddTexts},
            {"addLinks", LuaPluginRuntime::luaAddLinks},
            {"addImages", LuaPluginRuntime::luaAddImages},
            {"setPageSize", LuaPluginRuntime::luaSetPageSize},
            {"setLayerVisibility", LuaPluginRuntime::luaSetLayerVisibility},
            {"setCurrentLayerName", LuaPluginRuntime::luaSetCurrentLayerName},
            {"export", LuaPluginRuntime::luaExport},
            {"quickScreenshot", LuaPluginRuntime::luaQuickScreenshot},
            {"getDocumentStructure", LuaPluginRuntime::luaGetDocumentStructure},
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
    lua_pushinteger(state, 0);
    lua_setfield(state, -2, "Alignment_left");
    lua_pushinteger(state, 1);
    lua_setfield(state, -2, "Alignment_center");
    lua_pushinteger(state, 2);
    lua_setfield(state, -2, "Alignment_right");
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

int LuaPluginRuntime::luaAddEmptyPages(lua_State* state) {
    if (!lua_isboolean(state, 1)) {
        return luaL_error(state, "addEmptyPages expects a boolean force flag");
    }
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    std::string error;
    if (!plugin->runtime->addEmptyPages(lua_toboolean(state, 1), error)) {
        return luaL_error(state, "%s", error.empty() ? "Adding note pages failed" : error.c_str());
    }
    return 0;
}

int LuaPluginRuntime::luaTransformSelectedImages(lua_State* state) {
    if (!lua_isinteger(state, 1)) {
        return luaL_error(state, "transformSelectedImages expects an integer action code");
    }
    const auto action = static_cast<std::int64_t>(lua_tointeger(state, 1));
    if (action < 1 || action > 10) {
        return luaL_error(state, "Unknown image action code");
    }
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    std::string error;
    if (!plugin->runtime->transformSelectedImages(action, error)) {
        return luaL_error(state, "%s", error.empty() ? "Image action failed" : error.c_str());
    }
    return 0;
}

int LuaPluginRuntime::luaGetDisplayDpi(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    int dpi = 0;
    std::string error;
    if (!plugin->runtime->getDisplayDpi(dpi, error)) {
        return luaL_error(state, "%s", error.empty() ? "Reading display DPI failed" : error.c_str());
    }
    lua_pushinteger(state, dpi);
    return 1;
}

int LuaPluginRuntime::luaGetZoom(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    double zoom = 0;
    std::string error;
    if (!plugin->runtime->getZoom(zoom, error)) {
        return luaL_error(state, "%s", error.empty() ? "Reading zoom failed" : error.c_str());
    }
    lua_pushnumber(state, zoom);
    return 1;
}

int LuaPluginRuntime::luaSetZoom(lua_State* state) {
    if (!lua_isnumber(state, 1)) {
        return luaL_error(state, "setZoom expects a number");
    }
    const double zoom = lua_tonumber(state, 1);
    if (!std::isfinite(zoom)) {
        return luaL_error(state, "Zoom must be a finite number");
    }
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    std::string error;
    if (!plugin->runtime->setZoom(zoom, error)) {
        return luaL_error(state, "%s", error.empty() ? "Changing zoom failed" : error.c_str());
    }
    return 0;
}

int LuaPluginRuntime::luaGetStrokes(lua_State* state) {
    const char* type = luaL_checkstring(state, 1);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginStroke> strokes;
        std::string hostError;
        if (!plugin->runtime->getStrokes(type, strokes, hostError)) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Stroke access is unavailable" : hostError.c_str());
        } else {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(strokes.size(),
                                                                          std::numeric_limits<int>::max())), 0);
            lua_Integer strokeIndex = 0;
            for (const auto& stroke: strokes) {
                lua_createtable(state, 0, 10);

                const auto pushCoordinates = [state](const char* field, const std::vector<double>& values) {
                    lua_createtable(state, static_cast<int>(std::min<std::size_t>(values.size(),
                                                                                 std::numeric_limits<int>::max())),
                                    0);
                    lua_Integer pointIndex = 0;
                    for (const double value: values) {
                        lua_pushnumber(state, value);
                        lua_rawseti(state, -2, ++pointIndex);
                    }
                    lua_setfield(state, -2, field);
                };
                pushCoordinates("x", stroke.x);
                pushCoordinates("y", stroke.y);
                if (!stroke.pressure.empty()) {
                    pushCoordinates("pressure", stroke.pressure);
                }

                lua_pushlstring(state, stroke.tool.data(), stroke.tool.size());
                lua_setfield(state, -2, "tool");
                lua_pushnumber(state, stroke.width);
                lua_setfield(state, -2, "width");
                lua_pushinteger(state, static_cast<lua_Integer>(stroke.color));
                lua_setfield(state, -2, "color");
                lua_pushinteger(state, stroke.fill);
                lua_setfield(state, -2, "fill");
                lua_pushlstring(state, stroke.lineStyle.data(), stroke.lineStyle.size());
                lua_setfield(state, -2, "lineStyle");
                if (stroke.reference != 0) {
                    lua_pushlightuserdata(state, reinterpret_cast<void*>(stroke.reference));
                    lua_setfield(state, -2, "ref");
                }
                if (stroke.page) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*stroke.page));
                    lua_setfield(state, -2, "page");
                }
                if (stroke.layer) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*stroke.layer));
                    lua_setfield(state, -2, "layer");
                }

                lua_rawseti(state, -2, ++strokeIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaGetTexts(lua_State* state) {
    const char* type = luaL_checkstring(state, 1);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginText> texts;
        std::string hostError;
        if (!plugin->runtime->getTexts(type, texts, hostError)) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Text access is unavailable" : hostError.c_str());
        } else {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(texts.size(),
                                                                          std::numeric_limits<int>::max())), 0);
            lua_Integer textIndex = 0;
            for (const auto& text: texts) {
                lua_createtable(state, 0, 11);
                lua_pushlstring(state, text.text.data(), text.text.size());
                lua_setfield(state, -2, "text");
                lua_createtable(state, 0, 2);
                lua_pushlstring(state, text.fontName.data(), text.fontName.size());
                lua_setfield(state, -2, "name");
                lua_pushnumber(state, text.fontSize);
                lua_setfield(state, -2, "size");
                lua_setfield(state, -2, "font");
                lua_pushinteger(state, static_cast<lua_Integer>(text.color));
                lua_setfield(state, -2, "color");
                lua_pushnumber(state, text.x);
                lua_setfield(state, -2, "x");
                lua_pushnumber(state, text.y);
                lua_setfield(state, -2, "y");
                if (text.wrap) {
                    lua_pushnumber(state, *text.wrap);
                    lua_setfield(state, -2, "wrap");
                }
                lua_pushnumber(state, text.width);
                lua_setfield(state, -2, "width");
                lua_pushnumber(state, text.height);
                lua_setfield(state, -2, "height");
                if (text.reference != 0) {
                    lua_pushlightuserdata(state, reinterpret_cast<void*>(text.reference));
                    lua_setfield(state, -2, "ref");
                }
                if (text.page) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*text.page));
                    lua_setfield(state, -2, "page");
                }
                if (text.layer) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*text.layer));
                    lua_setfield(state, -2, "layer");
                }
                lua_rawseti(state, -2, ++textIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaGetLinks(lua_State* state) {
    const char* type = luaL_checkstring(state, 1);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginLink> links;
        std::string hostError;
        if (!plugin->runtime->getLinks(type, links, hostError)) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Link access is unavailable" : hostError.c_str());
        } else {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(links.size(),
                                                                          std::numeric_limits<int>::max())), 0);
            lua_Integer linkIndex = 0;
            for (const auto& link: links) {
                lua_createtable(state, 0, 12);
                lua_pushlstring(state, link.text.data(), link.text.size());
                lua_setfield(state, -2, "text");
                lua_pushlstring(state, link.url.data(), link.url.size());
                lua_setfield(state, -2, "url");
                lua_pushinteger(state, link.alignment);
                lua_setfield(state, -2, "alignment");
                lua_createtable(state, 0, 2);
                lua_pushlstring(state, link.fontName.data(), link.fontName.size());
                lua_setfield(state, -2, "name");
                lua_pushnumber(state, link.fontSize);
                lua_setfield(state, -2, "size");
                lua_setfield(state, -2, "font");
                lua_pushinteger(state, static_cast<lua_Integer>(link.color));
                lua_setfield(state, -2, "color");
                lua_pushnumber(state, link.x);
                lua_setfield(state, -2, "x");
                lua_pushnumber(state, link.y);
                lua_setfield(state, -2, "y");
                lua_pushnumber(state, link.width);
                lua_setfield(state, -2, "width");
                lua_pushnumber(state, link.height);
                lua_setfield(state, -2, "height");
                if (link.reference != 0) {
                    lua_pushlightuserdata(state, reinterpret_cast<void*>(link.reference));
                    lua_setfield(state, -2, "ref");
                }
                if (link.page) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*link.page));
                    lua_setfield(state, -2, "page");
                }
                if (link.layer) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*link.layer));
                    lua_setfield(state, -2, "layer");
                }
                lua_rawseti(state, -2, ++linkIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaGetImages(lua_State* state) {
    const char* type = luaL_checkstring(state, 1);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginImage> images;
        std::string hostError;
        if (!plugin->runtime->getImages(type, images, hostError)) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Image access is unavailable" : hostError.c_str());
        } else {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(images.size(),
                                                                          std::numeric_limits<int>::max())), 0);
            lua_Integer imageIndex = 0;
            for (const auto& image: images) {
                lua_createtable(state, 0, 11);
                lua_pushnumber(state, image.x);
                lua_setfield(state, -2, "x");
                lua_pushnumber(state, image.y);
                lua_setfield(state, -2, "y");
                lua_pushnumber(state, image.width);
                lua_setfield(state, -2, "width");
                lua_pushnumber(state, image.height);
                lua_setfield(state, -2, "height");
                lua_pushlstring(state, image.data.data(), image.data.size());
                lua_setfield(state, -2, "data");
                lua_pushlstring(state, image.format.data(), image.format.size());
                lua_setfield(state, -2, "format");
                lua_pushinteger(state, image.imageWidth);
                lua_setfield(state, -2, "imageWidth");
                lua_pushinteger(state, image.imageHeight);
                lua_setfield(state, -2, "imageHeight");
                if (image.reference != 0) {
                    lua_pushlightuserdata(state, reinterpret_cast<void*>(image.reference));
                    lua_setfield(state, -2, "ref");
                }
                if (image.page) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*image.page));
                    lua_setfield(state, -2, "page");
                }
                if (image.layer) {
                    lua_pushinteger(state, static_cast<lua_Integer>(*image.layer));
                    lua_setfield(state, -2, "layer");
                }
                lua_rawseti(state, -2, ++imageIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaAddToSelection(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<std::uintptr_t> references;
        lua_pushnil(state);
        while (lua_next(state, 1) != 0) {
            if (lua_type(state, -1) != LUA_TLIGHTUSERDATA) {
                std::snprintf(error, sizeof(error), "addToSelection expects an array of element references");
                lua_pop(state, 2);
                break;
            }
            references.push_back(reinterpret_cast<std::uintptr_t>(lua_touserdata(state, -1)));
            lua_pop(state, 1);
        }
        if (error[0] == '\0') {
            std::string hostError;
            if (!plugin->runtime->addToSelection(references, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Adding elements to the selection failed" : hostError.c_str());
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaClearSelection(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }
    char error[512]{};
    {
        std::string hostError;
        if (!plugin->runtime->clearSelection(hostError)) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Clearing the selection failed" : hostError.c_str());
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaAddStrokes(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginStrokeInput> strokes;
        std::vector<std::uintptr_t> references;
        PluginStrokeUndoPolicy undoPolicy = PluginStrokeUndoPolicy::Grouped;
        constexpr std::size_t maxStrokeCount = 100000;
        constexpr std::size_t maxPointCount = 1000000;
        std::size_t totalPointCount = 0;

        lua_settop(state, 1);
        if (!lua_istable(state, 1)) {
            std::snprintf(error, sizeof(error), "addStrokes expects an options table");
        }
        if (error[0] == '\0') {
            lua_getfield(state, 1, "strokes");
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Missing stroke table!");
            }
        }

        const auto strokeCount = error[0] == '\0' ? lua_rawlen(state, -1) : 0;
        if (error[0] == '\0' && strokeCount > maxStrokeCount) {
            std::snprintf(error, sizeof(error), "Stroke batch exceeds the supported stroke count");
        }

        const auto readNumberArray = [state, &error, maxPointCount](int strokeIndex, const char* field,
                                                                    std::vector<double>& values, bool required) {
            lua_getfield(state, strokeIndex, field);
            if (!lua_istable(state, -1)) {
                if (!required && lua_isnil(state, -1)) {
                    lua_pop(state, 1);
                    return;
                }
                std::snprintf(error, 512, "Stroke field '%s' must be a number array", field);
                lua_pop(state, 1);
                return;
            }
            const int arrayIndex = lua_gettop(state);
            const auto count = lua_rawlen(state, arrayIndex);
            if (count > maxPointCount) {
                std::snprintf(error, 512, "Stroke field '%s' exceeds the supported point count", field);
                lua_pop(state, 1);
                return;
            }
            values.reserve(count);
            for (std::size_t point = 1; point <= count; ++point) {
                lua_rawgeti(state, arrayIndex, static_cast<lua_Integer>(point));
                if (!lua_isnumber(state, -1)) {
                    std::snprintf(error, 512, "Stroke field '%s' contains a non-number value", field);
                    lua_pop(state, 1);
                    break;
                }
                const double value = lua_tonumber(state, -1);
                lua_pop(state, 1);
                if (!std::isfinite(value)) {
                    std::snprintf(error, 512, "Stroke field '%s' contains a non-finite value", field);
                    break;
                }
                values.push_back(value);
            }
            lua_pop(state, 1);
        };

        for (std::size_t index = 1; error[0] == '\0' && index <= strokeCount; ++index) {
            lua_rawgeti(state, 2, static_cast<lua_Integer>(index));
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Stroke entry %zu must be a table", index);
                lua_pop(state, 1);
                break;
            }
            const int strokeIndex = lua_gettop(state);
            PluginStrokeInput stroke;
            readNumberArray(strokeIndex, "x", stroke.x, true);
            if (error[0] == '\0') {
                readNumberArray(strokeIndex, "y", stroke.y, true);
            }
            if (error[0] == '\0') {
                readNumberArray(strokeIndex, "pressure", stroke.pressure, false);
            }
            if (error[0] == '\0' && stroke.x.size() != stroke.y.size()) {
                std::snprintf(error, sizeof(error), "X and Y vectors are not equal length!");
            }
            if (error[0] == '\0' && !stroke.pressure.empty() && stroke.x.size() != stroke.pressure.size()) {
                std::snprintf(error, sizeof(error), "Pressure vector is not equal length!");
            }
            if (error[0] == '\0' && stroke.x.size() < 2) {
                std::snprintf(error, sizeof(error), "A stroke must contain at least two points");
            }
            if (error[0] == '\0' && stroke.x.size() > maxPointCount - totalPointCount) {
                std::snprintf(error, sizeof(error), "Stroke batch exceeds the supported point count");
            }
            totalPointCount += stroke.x.size();

            const auto readString = [state, strokeIndex, &error](const char* field,
                                                                 std::optional<std::string>& value) {
                lua_getfield(state, strokeIndex, field);
                if (lua_isnil(state, -1)) {
                    lua_pop(state, 1);
                    return;
                }
                if (lua_type(state, -1) != LUA_TSTRING) {
                    std::snprintf(error, 512, "Stroke field '%s' must be a string", field);
                } else {
                    value = lua_tostring(state, -1);
                }
                lua_pop(state, 1);
            };
            const auto readNumber = [state, strokeIndex, &error](const char* field,
                                                                 std::optional<double>& value) {
                lua_getfield(state, strokeIndex, field);
                if (lua_isnil(state, -1)) {
                    lua_pop(state, 1);
                    return;
                }
                if (!lua_isnumber(state, -1)) {
                    std::snprintf(error, 512, "Stroke field '%s' must be a number", field);
                } else {
                    const double number = lua_tonumber(state, -1);
                    if (!std::isfinite(number) || number <= 0) {
                        std::snprintf(error, 512, "Stroke field '%s' must be finite and positive", field);
                    } else {
                        value = number;
                    }
                }
                lua_pop(state, 1);
            };
            const auto readInteger = [state, strokeIndex, &error](const char* field, std::optional<int>& value) {
                lua_getfield(state, strokeIndex, field);
                if (lua_isnil(state, -1)) {
                    lua_pop(state, 1);
                    return;
                }
                if (!lua_isinteger(state, -1)) {
                    std::snprintf(error, 512, "Stroke field '%s' must be an integer", field);
                } else {
                    const auto integer = lua_tointeger(state, -1);
                    if (integer < std::numeric_limits<int>::min() || integer > std::numeric_limits<int>::max()) {
                        std::snprintf(error, 512, "Stroke field '%s' is out of range", field);
                    } else {
                        value = static_cast<int>(integer);
                    }
                }
                lua_pop(state, 1);
            };

            if (error[0] == '\0') {
                readString("tool", stroke.tool);
                readNumber("width", stroke.width);
                readString("lineStyle", stroke.lineStyle);
                std::optional<int> color;
                readInteger("color", color);
                if (color) {
                    if (*color < 0 || *color > 0xffffff) {
                        std::snprintf(error, sizeof(error), "Color must be an RGB hex code in the range 0x000000..0xFFFFFF");
                    } else {
                        stroke.color = static_cast<std::uint32_t>(*color);
                    }
                }
                readInteger("fill", stroke.fill);
                if (stroke.fill && (*stroke.fill < -1 || *stroke.fill > 255)) {
                    std::snprintf(error, sizeof(error), "Stroke field 'fill' must be -1 or an opacity from 0 to 255");
                }
            }
            lua_pop(state, 1);
            if (error[0] == '\0') {
                strokes.push_back(std::move(stroke));
            }
        }

        if (error[0] == '\0') {
            lua_getfield(state, 1, "allowUndoRedoAction");
            if (lua_isnil(state, -1)) {
                // The master API groups a batch by default.
            } else if (lua_type(state, -1) != LUA_TSTRING) {
                std::snprintf(error, sizeof(error), "Undo policy must be 'grouped', 'individual', or 'none'");
            } else {
                const std::string_view policy = lua_tostring(state, -1);
                if (policy == "grouped") {
                    undoPolicy = PluginStrokeUndoPolicy::Grouped;
                } else if (policy == "individual") {
                    undoPolicy = PluginStrokeUndoPolicy::Individual;
                } else if (policy == "none") {
                    undoPolicy = PluginStrokeUndoPolicy::None;
                } else {
                    std::snprintf(error, sizeof(error), "Unrecognized undo option: %.*s",
                                  static_cast<int>(policy.size()), policy.data());
                }
            }
            lua_pop(state, 1);
        }

        if (error[0] == '\0') {
            std::string hostError;
            if (!plugin->runtime->addStrokes(strokes, undoPolicy, references, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Adding strokes failed" : hostError.c_str());
            }
        }
        if (error[0] == '\0') {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(references.size(),
                                                                          std::numeric_limits<int>::max())),
                            0);
            lua_Integer referenceIndex = 0;
            for (const auto reference: references) {
                lua_pushlightuserdata(state, reinterpret_cast<void*>(reference));
                lua_rawseti(state, -2, ++referenceIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaAddSplines(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginStrokeInput> strokes;
        std::vector<std::uintptr_t> references;
        PluginStrokeUndoPolicy undoPolicy = PluginStrokeUndoPolicy::Grouped;
        constexpr std::size_t maxSplineCount = 100000;
        constexpr std::size_t maxSplineSegmentCount = 1000000;
        constexpr std::size_t maxPointCount = 1000000;
        std::size_t totalSplineSegmentCount = 0;
        std::size_t totalPointCount = 0;

        lua_settop(state, 1);
        if (!lua_istable(state, 1)) {
            std::snprintf(error, sizeof(error), "addSplines expects an options table");
        }
        if (error[0] == '\0') {
            lua_getfield(state, 1, "splines");
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Missing spline table!");
                lua_pop(state, 1);
            }
        }

        const int splinesIndex = error[0] == '\0' ? lua_absindex(state, -1) : 0;
        const std::size_t splineCount = splinesIndex ? lua_rawlen(state, splinesIndex) : 0;
        if (error[0] == '\0' && splineCount > maxSplineCount) {
            std::snprintf(error, sizeof(error), "Spline batch exceeds the supported spline count");
        }

        for (std::size_t index = 1; error[0] == '\0' && index <= splineCount; ++index) {
            lua_rawgeti(state, splinesIndex, static_cast<lua_Integer>(index));
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Spline entry %zu must be a table", index);
                lua_pop(state, 1);
                break;
            }
            const int splineIndex = lua_absindex(state, -1);
            lua_getfield(state, splineIndex, "coordinates");
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Missing coordinate table!");
                lua_pop(state, 1);
            }

            if (error[0] == '\0') {
                const int coordinatesIndex = lua_absindex(state, -1);
                const std::size_t coordinateCount = lua_rawlen(state, coordinatesIndex);
                if (coordinateCount % 8 != 0) {
                    std::snprintf(error, sizeof(error), "Point table incomplete!");
                } else if (coordinateCount / 8 > maxSplineSegmentCount - totalSplineSegmentCount) {
                    std::snprintf(error, sizeof(error), "Spline batch exceeds the supported segment count");
                }

                std::vector<double> coordinates;
                if (error[0] == '\0') {
                    totalSplineSegmentCount += coordinateCount / 8;
                    coordinates.reserve(coordinateCount);
                    for (std::size_t coordinateIndex = 1;
                         error[0] == '\0' && coordinateIndex <= coordinateCount; ++coordinateIndex) {
                        lua_rawgeti(state, coordinatesIndex, static_cast<lua_Integer>(coordinateIndex));
                        if (!lua_isnumber(state, -1)) {
                            std::snprintf(error, sizeof(error),
                                          "Spline coordinates must contain only finite numbers");
                            lua_pop(state, 1);
                            break;
                        }
                        const double coordinate = lua_tonumber(state, -1);
                        lua_pop(state, 1);
                        if (!std::isfinite(coordinate)) {
                            std::snprintf(error, sizeof(error),
                                          "Spline coordinates must contain only finite numbers");
                            break;
                        }
                        coordinates.push_back(coordinate);
                    }
                }
                lua_pop(state, 1);

                if (error[0] == '\0') {
                    std::vector<Point> points;
                    const std::size_t remainingPointCount = maxPointCount - totalPointCount;
                    for (std::size_t coordinateIndex = 0;
                         error[0] == '\0' && coordinateIndex < coordinates.size(); coordinateIndex += 8) {
                        const CubicSplineSegment segment{
                                {coordinates[coordinateIndex], coordinates[coordinateIndex + 1]},
                                {coordinates[coordinateIndex + 2], coordinates[coordinateIndex + 3]},
                                {coordinates[coordinateIndex + 4], coordinates[coordinateIndex + 5]},
                                {coordinates[coordinateIndex + 6], coordinates[coordinateIndex + 7]},
                        };
                        if (!appendSplinePoints(segment, points, remainingPointCount)) {
                            std::snprintf(error, sizeof(error),
                                          "Spline batch exceeds the supported point count or complexity");
                        }
                    }
                    totalPointCount += points.size();
                    if (error[0] == '\0' && points.size() >= 2) {
                        PluginStrokeInput stroke;
                        stroke.x.reserve(points.size());
                        stroke.y.reserve(points.size());
                        for (const auto& point: points) {
                            stroke.x.push_back(point.x);
                            stroke.y.push_back(point.y);
                        }

                        if (!readLuaStringField(state, splineIndex, "tool", stroke.tool, false, error,
                                                sizeof(error)) ||
                            !readLuaNumberField(state, splineIndex, "width", stroke.width, false, error,
                                                sizeof(error)) ||
                            !readLuaColor(state, splineIndex, stroke.color, error, sizeof(error)) ||
                            !readLuaStringField(state, splineIndex, "lineStyle", stroke.lineStyle, false, error,
                                                sizeof(error))) {
                            // The field reader has already described the invalid option.
                        }
                        std::optional<lua_Integer> fill;
                        if (error[0] == '\0' &&
                            !readLuaIntegerField(state, splineIndex, "fill", fill, error, sizeof(error))) {
                            // The field reader has already described the invalid option.
                        }
                        if (error[0] == '\0' && stroke.width && *stroke.width <= 0) {
                            std::snprintf(error, sizeof(error), "Stroke field 'width' must be finite and positive");
                        }
                        if (error[0] == '\0' && fill && (*fill < -1 || *fill > 255)) {
                            std::snprintf(error, sizeof(error),
                                          "Stroke field 'fill' must be -1 or an opacity from 0 to 255");
                        }
                        if (error[0] == '\0') {
                            if (fill) {
                                stroke.fill = static_cast<int>(*fill);
                            }
                            strokes.push_back(std::move(stroke));
                        }
                    }
                }
            }
            lua_pop(state, 1);
        }

        if (splinesIndex) {
            lua_pop(state, 1);
        }
        if (error[0] == '\0' && !readLuaUndoPolicy(state, 1, undoPolicy, error, sizeof(error))) {
            // The policy parser has already described the invalid value.
        }
        if (error[0] == '\0') {
            std::string hostError;
            if (!plugin->runtime->addStrokes(strokes, undoPolicy, references, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Adding splines failed" : hostError.c_str());
            }
        }
        if (error[0] == '\0') {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(references.size(),
                                                                          std::numeric_limits<int>::max())),
                            0);
            lua_Integer referenceIndex = 0;
            for (const auto reference: references) {
                lua_pushlightuserdata(state, reinterpret_cast<void*>(reference));
                lua_rawseti(state, -2, ++referenceIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaAddTexts(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginTextInput> texts;
        std::vector<std::uintptr_t> references;
        PluginStrokeUndoPolicy undoPolicy = PluginStrokeUndoPolicy::Grouped;
        constexpr std::size_t maxTextCount = 100000;
        constexpr std::size_t maxTextBytes = 16 * 1024 * 1024;
        std::size_t totalTextBytes = 0;

        lua_settop(state, 1);
        if (!lua_istable(state, 1)) {
            std::snprintf(error, sizeof(error), "addTexts expects an options table");
        }
        if (error[0] == '\0') {
            lua_getfield(state, 1, "texts");
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Missing text table!");
            }
        }

        const int textsIndex = error[0] == '\0' ? lua_absindex(state, -1) : 0;
        const std::size_t textCount = textsIndex ? lua_rawlen(state, textsIndex) : 0;
        if (error[0] == '\0' && textCount > maxTextCount) {
            std::snprintf(error, sizeof(error), "Text batch exceeds the supported text count");
        }

        for (std::size_t index = 1; error[0] == '\0' && index <= textCount; ++index) {
            lua_rawgeti(state, textsIndex, static_cast<lua_Integer>(index));
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Text entry %zu must be a table", index);
                lua_pop(state, 1);
                break;
            }
            const int textIndex = lua_absindex(state, -1);
            PluginTextInput input;
            std::optional<std::string> text;
            std::optional<double> x;
            std::optional<double> y;
            if (!readLuaStringField(state, textIndex, "text", text, true, error, sizeof(error)) ||
                !readLuaFont(state, textIndex, input.fontName, input.fontSize, error, sizeof(error)) ||
                !readLuaColor(state, textIndex, input.color, error, sizeof(error)) ||
                !readLuaNumberField(state, textIndex, "x", x, true, error, sizeof(error)) ||
                !readLuaNumberField(state, textIndex, "y", y, true, error, sizeof(error)) ||
                !readLuaNumberField(state, textIndex, "wrap", input.wrap, false, error, sizeof(error))) {
                lua_pop(state, 1);
                break;
            }
            if (!g_utf8_validate(text->data(), static_cast<gssize>(text->size()), nullptr)) {
                std::snprintf(error, sizeof(error), "Text entry %zu is not valid UTF-8", index);
                lua_pop(state, 1);
                break;
            }
            if (text->size() > maxTextBytes - totalTextBytes) {
                std::snprintf(error, sizeof(error), "Text batch exceeds the supported text size");
                lua_pop(state, 1);
                break;
            }
            totalTextBytes += text->size();
            input.text = std::move(*text);
            input.x = *x;
            input.y = *y;
            texts.push_back(std::move(input));
            lua_pop(state, 1);
        }

        if (error[0] == '\0' && !readLuaUndoPolicy(state, 1, undoPolicy, error, sizeof(error))) {
            // The policy parser has already described the invalid value.
        }
        if (error[0] == '\0') {
            std::string hostError;
            if (!plugin->runtime->addTexts(texts, undoPolicy, references, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Adding text failed" : hostError.c_str());
            } else if (references.size() != texts.size()) {
                std::snprintf(error, sizeof(error), "The GTK 4 host returned incomplete text references");
            }
        }
        if (error[0] == '\0') {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(references.size(),
                                                                          std::numeric_limits<int>::max())),
                            0);
            lua_Integer referenceIndex = 0;
            for (const auto reference: references) {
                lua_pushlightuserdata(state, reinterpret_cast<void*>(reference));
                lua_rawseti(state, -2, ++referenceIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaAddLinks(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginLinkInput> links;
        std::vector<std::uintptr_t> references;
        PluginStrokeUndoPolicy undoPolicy = PluginStrokeUndoPolicy::Grouped;
        constexpr std::size_t maxLinkCount = 100000;
        constexpr std::size_t maxLinkTextBytes = 16 * 1024 * 1024;
        std::size_t totalTextBytes = 0;

        lua_settop(state, 1);
        if (!lua_istable(state, 1)) {
            std::snprintf(error, sizeof(error), "addLinks expects an options table");
        }
        if (error[0] == '\0') {
            lua_getfield(state, 1, "links");
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Missing link table!");
            }
        }

        const int linksIndex = error[0] == '\0' ? lua_absindex(state, -1) : 0;
        const std::size_t linkCount = linksIndex ? lua_rawlen(state, linksIndex) : 0;
        if (error[0] == '\0' && linkCount > maxLinkCount) {
            std::snprintf(error, sizeof(error), "Link batch exceeds the supported link count");
        }

        for (std::size_t index = 1; error[0] == '\0' && index <= linkCount; ++index) {
            lua_rawgeti(state, linksIndex, static_cast<lua_Integer>(index));
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Link entry %zu must be a table", index);
                lua_pop(state, 1);
                break;
            }
            const int linkIndex = lua_absindex(state, -1);
            PluginLinkInput input;
            std::optional<std::string> text;
            std::optional<std::string> url;
            std::optional<double> x;
            std::optional<double> y;
            if (!readLuaStringField(state, linkIndex, "text", text, true, error, sizeof(error)) ||
                !readLuaStringField(state, linkIndex, "url", url, true, error, sizeof(error)) ||
                !readLuaFont(state, linkIndex, input.fontName, input.fontSize, error, sizeof(error)) ||
                !readLuaColor(state, linkIndex, input.color, error, sizeof(error)) ||
                !readLuaNumberField(state, linkIndex, "x", x, true, error, sizeof(error)) ||
                !readLuaNumberField(state, linkIndex, "y", y, true, error, sizeof(error))) {
                lua_pop(state, 1);
                break;
            }
            lua_getfield(state, linkIndex, "alignment");
            if (!lua_isnil(state, -1)) {
                int isInteger = 0;
                const lua_Integer alignment = lua_tointegerx(state, -1, &isInteger);
                if (!isInteger) {
                    std::snprintf(error, sizeof(error), "Link field 'alignment' must be an integer");
                } else {
                    if (alignment < 0 || alignment > 2) {
                        std::snprintf(error, sizeof(error), "Link alignment must be 0, 1, or 2");
                    } else {
                        input.alignment = static_cast<int>(alignment);
                    }
                }
            }
            lua_pop(state, 1);
            if (error[0] != '\0') {
                lua_pop(state, 1);
                break;
            }
            if (!g_utf8_validate(text->data(), static_cast<gssize>(text->size()), nullptr)) {
                std::snprintf(error, sizeof(error), "Link entry %zu text is not valid UTF-8", index);
                lua_pop(state, 1);
                break;
            }
            if (text->size() > maxLinkTextBytes - totalTextBytes) {
                std::snprintf(error, sizeof(error), "Link batch exceeds the supported text size");
                lua_pop(state, 1);
                break;
            }
            totalTextBytes += text->size();
            input.text = std::move(*text);
            input.url = std::move(*url);
            input.x = *x;
            input.y = *y;
            links.push_back(std::move(input));
            lua_pop(state, 1);
        }

        if (error[0] == '\0' && !readLuaUndoPolicy(state, 1, undoPolicy, error, sizeof(error))) {
            // The policy parser has already described the invalid value.
        }
        if (error[0] == '\0') {
            std::string hostError;
            if (!plugin->runtime->addLinks(links, undoPolicy, references, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Adding links failed" : hostError.c_str());
            } else if (references.size() != links.size()) {
                std::snprintf(error, sizeof(error), "The GTK 4 host returned incomplete link references");
            }
        }
        if (error[0] == '\0') {
            lua_createtable(state, static_cast<int>(std::min<std::size_t>(references.size(),
                                                                          std::numeric_limits<int>::max())),
                            0);
            lua_Integer referenceIndex = 0;
            for (const auto reference: references) {
                lua_pushlightuserdata(state, reinterpret_cast<void*>(reference));
                lua_rawseti(state, -2, ++referenceIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaAddImages(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::vector<PluginImageInput> images;
        std::vector<PluginImageResult> results;
        PluginStrokeUndoPolicy undoPolicy = PluginStrokeUndoPolicy::Grouped;
        constexpr std::size_t maxImageCount = 4096;
        constexpr std::size_t maxImageBytes = 64 * 1024 * 1024;

        lua_settop(state, 1);
        if (!lua_istable(state, 1)) {
            std::snprintf(error, sizeof(error), "addImages expects an options table");
        }
        if (error[0] == '\0') {
            lua_getfield(state, 1, "images");
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Missing image table!");
            }
        }

        const int imagesIndex = error[0] == '\0' ? lua_absindex(state, -1) : 0;
        const std::size_t imageCount = imagesIndex ? lua_rawlen(state, imagesIndex) : 0;
        if (error[0] == '\0' && imageCount > maxImageCount) {
            std::snprintf(error, sizeof(error), "Image batch exceeds the supported image count");
        }

        for (std::size_t index = 1; error[0] == '\0' && index <= imageCount; ++index) {
            lua_rawgeti(state, imagesIndex, static_cast<lua_Integer>(index));
            if (!lua_istable(state, -1)) {
                std::snprintf(error, sizeof(error), "Image entry %zu must be a table", index);
                lua_pop(state, 1);
                break;
            }
            const int imageIndex = lua_absindex(state, -1);
            PluginImageInput input;
            std::optional<std::string> path;
            std::optional<double> x;
            std::optional<double> y;
            std::optional<double> scale;
            std::optional<lua_Integer> maxWidth;
            std::optional<lua_Integer> maxHeight;
            if (!readLuaStringField(state, imageIndex, "path", path, false, error, sizeof(error)) ||
                !readLuaBinaryField(state, imageIndex, "data", input.data, error, sizeof(error)) ||
                !readLuaNumberField(state, imageIndex, "x", x, false, error, sizeof(error)) ||
                !readLuaNumberField(state, imageIndex, "y", y, false, error, sizeof(error)) ||
                !readLuaIntegerField(state, imageIndex, "maxWidth", maxWidth, error, sizeof(error)) ||
                !readLuaIntegerField(state, imageIndex, "maxHeight", maxHeight, error, sizeof(error)) ||
                !readLuaNumberField(state, imageIndex, "scale", scale, false, error, sizeof(error))) {
                lua_pop(state, 1);
                break;
            }
            if (!path && !input.data) {
                std::snprintf(error, sizeof(error), "No 'path' parameter or image 'data' was provided");
            } else if (path && input.data) {
                std::snprintf(error, sizeof(error), "Only one of 'path' or image 'data' can be provided");
            } else if (input.data && input.data->size() > maxImageBytes) {
                std::snprintf(error, sizeof(error), "Image data exceeds the supported 64 MiB limit");
            }

            if (error[0] == '\0') {
                const auto validDimension = [](const std::optional<lua_Integer>& dimension) {
                    return !dimension || (*dimension == -1 || (*dimension > 0 &&
                                                                  *dimension <= std::numeric_limits<int>::max()));
                };
                if (!validDimension(maxWidth) || !validDimension(maxHeight)) {
                    std::snprintf(error, sizeof(error),
                                  "Image dimensions must be positive integers or -1 to disable manual sizing");
                }
            }
            if (error[0] == '\0' && scale && *scale <= 0) {
                std::snprintf(error, sizeof(error), "Image scale must be positive");
            }

            if (error[0] == '\0') {
                input.path = std::move(path);
                input.x = x.value_or(0);
                input.y = y.value_or(0);
                input.maxWidth = static_cast<int>(maxWidth.value_or(-1));
                input.maxHeight = static_cast<int>(maxHeight.value_or(-1));
                input.scale = scale.value_or(1);
                lua_getfield(state, imageIndex, "aspectRatio");
                input.aspectRatio = lua_isnil(state, -1) || lua_toboolean(state, -1);
                lua_pop(state, 1);
                images.push_back(std::move(input));
            }
            lua_pop(state, 1);
        }

        if (error[0] == '\0' && !readLuaUndoPolicy(state, 1, undoPolicy, error, sizeof(error))) {
            // The policy parser has already described the invalid value.
        }
        if (error[0] == '\0') {
            std::string hostError;
            if (!plugin->runtime->addImages(images, undoPolicy, results, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Adding images failed" : hostError.c_str());
            } else if (results.size() != images.size()) {
                std::snprintf(error, sizeof(error), "The GTK 4 host returned incomplete image results");
            }
        }
        if (error[0] == '\0') {
            lua_createtable(state, static_cast<int>(results.size()), 0);
            lua_Integer resultIndex = 0;
            for (const auto& result: results) {
                if (result.reference && *result.reference != 0) {
                    lua_pushlightuserdata(state, reinterpret_cast<void*>(*result.reference));
                } else {
                    const auto& message = result.error.empty() ? "Error: image insertion failed" : result.error;
                    lua_pushlstring(state, message.data(), message.size());
                }
                lua_rawseti(state, -2, ++resultIndex);
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 1;
}

int LuaPluginRuntime::luaSetPageSize(lua_State* state) {
    if (!lua_isnumber(state, 1) || !lua_isnumber(state, 2)) {
        return luaL_error(state, "setPageSize expects width and height numbers");
    }
    const double width = lua_tonumber(state, 1);
    const double height = lua_tonumber(state, 2);
    bool relative = false;
    if (lua_isboolean(state, 3)) {
        relative = lua_toboolean(state, 3);
    }
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        if (!std::isfinite(width) || !std::isfinite(height)) {
            std::snprintf(error, sizeof(error), "Page dimensions must be finite");
        } else {
            std::string hostError;
            if (!plugin->runtime->setPageSize(width, height, relative, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Changing page size failed" : hostError.c_str());
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaSetLayerVisibility(lua_State* state) {
    bool visible = true;
    if (lua_isboolean(state, 1)) {
        visible = lua_toboolean(state, 1);
    } else if (!lua_isnil(state, 1)) {
        return luaL_error(state, "setLayerVisibility expects a boolean or nil");
    }
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::string hostError;
        if (!plugin->runtime->setLayerVisibility(visible, hostError)) {
            std::snprintf(error, sizeof(error), "%s",
                          hostError.empty() ? "Changing layer visibility failed" : hostError.c_str());
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaSetCurrentLayerName(lua_State* state) {
    if (!lua_isstring(state, 1)) {
        return 0;
    }
    const char* name = lua_tostring(state, 1);
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        std::string hostError;
        if (!plugin->runtime->setCurrentLayerName(name, hostError)) {
            std::snprintf(error, sizeof(error), "%s", hostError.empty() ? "Renaming layer failed" : hostError.c_str());
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaExport(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    char error[512]{};
    {
        DocumentExportOptions options;
        if (!lua_istable(state, 1)) {
            std::snprintf(error, sizeof(error), "export expects an options table");
        }

        const auto readString = [state, &error](const char* field, std::string& value, bool required,
                                                const char* fallback = "") {
            if (error[0] != '\0') {
                return;
            }
            lua_getfield(state, 1, field);
            if (lua_isnil(state, -1)) {
                if (required) {
                    std::snprintf(error, sizeof(error), "Missing %s!", field);
                } else {
                    value = fallback;
                }
                lua_pop(state, 1);
                return;
            }
            if (!lua_isstring(state, -1)) {
                std::snprintf(error, sizeof(error), "Export option '%s' must be a string", field);
                lua_pop(state, 1);
                return;
            }
            const char* text = lua_tostring(state, -1);
            value = text ? text : "";
            lua_pop(state, 1);
        };
        const auto readOptionalString = [state, &error](const char* field,
                                                        std::optional<std::string>& value) {
            if (error[0] != '\0') {
                return;
            }
            lua_getfield(state, 1, field);
            if (lua_isnil(state, -1)) {
                value.reset();
            } else if (!lua_isstring(state, -1)) {
                std::snprintf(error, sizeof(error), "Export option '%s' must be a string", field);
            } else {
                const char* text = lua_tostring(state, -1);
                value = text ? text : "";
            }
            lua_pop(state, 1);
        };
        const auto readInteger = [state, &error](const char* field, int& value, int fallback) {
            if (error[0] != '\0') {
                return;
            }
            lua_getfield(state, 1, field);
            if (lua_isnil(state, -1)) {
                value = fallback;
            } else {
                int isInteger = 0;
                const auto integer = lua_tointegerx(state, -1, &isInteger);
                if (!isInteger) {
                    std::snprintf(error, sizeof(error), "Export option '%s' must be an integer", field);
                } else if (integer < std::numeric_limits<int>::min() ||
                           integer > std::numeric_limits<int>::max()) {
                    std::snprintf(error, sizeof(error), "Export option '%s' is out of range", field);
                } else {
                    value = static_cast<int>(integer);
                }
            }
            lua_pop(state, 1);
        };

        readString("outputFile", options.outputFile, true);
        readOptionalString("range", options.range);
        readOptionalString("layerRange", options.layerRange);
        readString("background", options.background, false, "all");
        readString("backend", options.backend, false, "default");
        if (error[0] == '\0') {
            lua_getfield(state, 1, "progressiveMode");
            options.progressiveMode = lua_toboolean(state, -1);
            lua_pop(state, 1);
            readInteger("pngDpi", options.pngDpi, -1);
            readInteger("pngWidth", options.pngWidth, -1);
            // The GTK 3 implementation reads the legacy misspelling "dpiHeight";
            // accept the documented name as well, preferring it when both are present.
            lua_getfield(state, 1, "pngHeight");
            const bool hasPngHeight = !lua_isnil(state, -1);
            lua_pop(state, 1);
            if (hasPngHeight) {
                readInteger("pngHeight", options.pngHeight, -1);
            } else {
                readInteger("dpiHeight", options.pngHeight, -1);
            }
        }

        if (error[0] == '\0' &&
            (options.outputFile.size() > 64 * 1024 || (options.range && options.range->size() > 64 * 1024) ||
             (options.layerRange && options.layerRange->size() > 64 * 1024))) {
            std::snprintf(error, sizeof(error), "Export options exceed the supported size limit");
        }

        if (error[0] == '\0') {
            std::string hostError;
            if (!plugin->runtime->exportDocument(options, hostError)) {
                std::snprintf(error, sizeof(error), "%s",
                              hostError.empty() ? "Document export failed" : hostError.c_str());
            }
        }
    }
    if (error[0] != '\0') {
        lua_pushstring(state, error);
        return lua_error(state);
    }
    return 0;
}

int LuaPluginRuntime::luaQuickScreenshot(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    std::string error;
    if (!plugin->runtime->quickScreenshot(plugin->name, error)) {
        return luaL_error(state, "%s", error.empty() ? "Screen-region capture failed" : error.c_str());
    }
    return 0;
}

int LuaPluginRuntime::luaGetDocumentStructure(lua_State* state) {
    auto* plugin = pluginForState(state);
    if (!plugin) {
        return luaL_error(state, "No owning plugin state");
    }

    const auto structure = plugin->runtime->documentStructure();
    lua_createtable(state, 0, 4);

    lua_createtable(state, static_cast<int>(std::min<std::size_t>(
                                 structure.pages.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))),
                    0);
    for (std::size_t pageIndex = 0; pageIndex < structure.pages.size(); ++pageIndex) {
        const auto& page = structure.pages[pageIndex];
        lua_pushinteger(state, static_cast<lua_Integer>(pageIndex + 1));
        lua_createtable(state, 0, 9);
        lua_pushnumber(state, page.width);
        lua_setfield(state, -2, "pageWidth");
        lua_pushnumber(state, page.height);
        lua_setfield(state, -2, "pageHeight");
        lua_pushboolean(state, page.isAnnotated);
        lua_setfield(state, -2, "isAnnotated");
        lua_pushlstring(state, page.pageTypeFormat.data(), page.pageTypeFormat.size());
        lua_setfield(state, -2, "pageTypeFormat");
        lua_pushlstring(state, page.pageTypeConfig.data(), page.pageTypeConfig.size());
        lua_setfield(state, -2, "pageTypeConfig");
        lua_pushinteger(state, static_cast<lua_Integer>(page.backgroundColor));
        lua_setfield(state, -2, "backgroundColor");
        lua_pushinteger(state, static_cast<lua_Integer>(page.pdfBackgroundPageNo));
        lua_setfield(state, -2, "pdfBackgroundPageNo");
        lua_createtable(state, 0,
                        static_cast<int>(std::min<std::size_t>(
                                page.layers.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))));
        for (std::size_t layerIndex = 0; layerIndex < page.layers.size(); ++layerIndex) {
            const auto& layer = page.layers[layerIndex];
            lua_pushinteger(state, static_cast<lua_Integer>(layerIndex));
            lua_createtable(state, 0, layer.isAnnotated ? 3 : 2);
            lua_pushboolean(state, layer.visible);
            lua_setfield(state, -2, "isVisible");
            lua_pushlstring(state, layer.name.data(), layer.name.size());
            lua_setfield(state, -2, "name");
            if (layer.isAnnotated) {
                lua_pushboolean(state, *layer.isAnnotated);
                lua_setfield(state, -2, "isAnnotated");
            }
            lua_settable(state, -3);
        }
        lua_setfield(state, -2, "layers");
        lua_pushinteger(state, static_cast<lua_Integer>(page.currentLayer));
        lua_setfield(state, -2, "currentLayer");
        lua_settable(state, -3);
    }
    lua_setfield(state, -2, "pages");

    lua_pushinteger(state, static_cast<lua_Integer>(structure.currentPage));
    lua_setfield(state, -2, "currentPage");
    lua_pushlstring(state, structure.pdfBackgroundFilename.data(), structure.pdfBackgroundFilename.size());
    lua_setfield(state, -2, "pdfBackgroundFilename");
    lua_pushlstring(state, structure.xoppFilename.data(), structure.xoppFilename.size());
    lua_setfield(state, -2, "xoppFilename");
    return 1;
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
