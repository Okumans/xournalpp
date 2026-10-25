#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "LuaPluginRuntime.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void writePlugin(const std::filesystem::path& directory, const std::string& manifest, const std::string& script) {
    std::filesystem::create_directories(directory);
    std::ofstream ini(directory / "plugin.ini", std::ios::binary);
    ini << manifest;
    std::ofstream main(directory / "main.lua", std::ios::binary);
    main << script;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("xournalpp-gtk4-lua-plugins-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(root);

    const auto colorCycle = root / "ColorCycle";
    writePlugin(colorCycle,
                "[about]\nauthor=Test author\ndescription=Color cycle test\nversion=1\n"
                "[default]\nenabled=false\n[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Cycle', callback='cycle', mode=7, "
                "parentPath='Tools/Test', accelerator='<Control>t', shortcut='<Shift>r'})\n"
                "end\n"
                "function cycle(mode)\n"
                "  app.changeToolColor({color=0x123456, tool='pen', selection=true})\n"
                "  app.changeActionState('test-mode', mode)\n"
                "end\n");
    const auto defaultPlugin = root / "DefaultPlugin";
    writePlugin(defaultPlugin,
                "[about]\nauthor=Test author\ndescription=Default enabled\nversion=1\n"
                "[default]\nenabled=true\n[plugin]\nmainfile=main.lua\n",
                "value = 42\n");
    const auto invalidApi = root / "InvalidApi";
    writePlugin(invalidApi,
                "[about]\ndescription=Invalid API values\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Broken', callback='broken', mode='not-an-integer'})\n"
                "end\n");
    const auto unavailableAction = root / "UnavailableAction";
    writePlugin(unavailableAction,
                "[about]\ndescription=Unavailable host action\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi() app.registerUi({menu='Unavailable', callback='run'}) end\n"
                "function run() app.activateAction('missing-action') end\n");
    writePlugin(root / "InvalidMain",
                "[about]\ndescription=Invalid\n[default]\nenabled=false\n[plugin]\nmainfile=../outside.lua\n", "");

    prototype::LuaPluginRuntime runtime;
    runtime.discover({root}, colorCycle.string(), {});
    const auto discovered = runtime.plugins();
    require(discovered.size() == 5, "Plugin discovery did not enumerate all plugin directories");
    const auto colorSummary = runtime.findPlugin("ColorCycle");
    require(colorSummary && colorSummary->valid && colorSummary->enabled && !colorSummary->loaded,
            "Explicitly enabled plugin path did not override the disabled default");
    const auto invalidSummary = runtime.findPlugin("InvalidMain");
    require(invalidSummary && !invalidSummary->valid && !invalidSummary->error.empty(),
            "Invalid plugin mainfile was not rejected with a diagnostic");
    const auto invalidApiSummary = runtime.findPlugin("InvalidApi");
    require(invalidApiSummary && invalidApiSummary->valid && invalidApiSummary->enabled,
            "Invalid plugin API fixture was not discovered as an enabled plugin");

    std::uint32_t changedColor = 0;
    bool changedSelection = false;
    std::string changedTool;
    std::int64_t changedMode = -1;
    prototype::PluginRuntimeHost host;
    host.changeToolColor = [&](const std::optional<std::string>& tool, std::uint32_t color, bool selection,
                               std::string&) {
        changedTool = tool.value_or("");
        changedColor = color;
        changedSelection = selection;
        return true;
    };
    host.changeActionState = [&](std::string_view action, const prototype::PluginActionValue& value, std::string&) {
        if (action != "test-mode" || !std::holds_alternative<std::int64_t>(value)) {
            return false;
        }
        changedMode = std::get<std::int64_t>(value);
        return true;
    };
    runtime.loadEnabled(std::move(host));

    const auto loadedColorSummary = runtime.findPlugin("ColorCycle");
    require(loadedColorSummary && loadedColorSummary->loaded && loadedColorSummary->menuItems == 1,
            "Enabled Lua plugin did not run initUi and register its menu callback");
    const auto loadedDefaultSummary = runtime.findPlugin("DefaultPlugin");
    require(loadedDefaultSummary && loadedDefaultSummary->loaded, "Default-enabled plugin did not load");
    require(!runtime.findPlugin("InvalidMain")->loaded, "Invalid plugin mainfile unexpectedly loaded");
    const auto loadedInvalidApi = runtime.findPlugin("InvalidApi");
    require(loadedInvalidApi && !loadedInvalidApi->loaded &&
                    loadedInvalidApi->error.find("must be an integer") != std::string::npos,
            "Invalid Lua plugin API values did not produce a runtime diagnostic");
    require(!runtime.invokeMenu("UnavailableAction", 0),
            "A failed host action should be reported as a Lua callback error");
    const auto unavailableSummary = runtime.findPlugin("UnavailableAction");
    require(unavailableSummary && unavailableSummary->error.find("Actions are unavailable") != std::string::npos,
            "Unavailable host action did not retain a plugin diagnostic");

    require(runtime.invokeMenu("ColorCycle", 0), "Registered plugin menu callback did not execute");
    require(changedColor == 0x123456 && changedTool == "pen" && changedSelection && changedMode == 7,
            "Lua app API arguments did not reach the GTK4 host callback");
    require(runtime.handleKeyPress(GDK_KEY_R, GDK_SHIFT_MASK, false) && changedMode == 7,
            "Plugin canvas shortcut did not run with legacy shifted-key matching");
    require(!runtime.handleKeyPress(GDK_KEY_r, GdkModifierType(0), true),
            "Plugin shortcuts must not steal keyboard input from text editing");

    auto* actionGroup = g_simple_action_group_new();
    auto* menu = g_menu_new();
    require(runtime.appendMenuSections(menu, G_ACTION_MAP(actionGroup), nullptr) == 2 &&
                    g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 2,
            "Loaded plugin menus were not exposed as a named menu section");
    g_object_unref(menu);
    g_object_unref(actionGroup);

    runtime.resetPendingStates();
    runtime.setPendingEnabled("DefaultPlugin", false);
    const auto [enabledPaths, disabledPaths] = runtime.pendingPluginPaths();
    require(enabledPaths.find(std::filesystem::absolute(colorCycle).string()) != std::string::npos &&
                    disabledPaths.find(std::filesystem::absolute(defaultPlugin).string()) != std::string::npos,
            "Plugin manager persistence did not serialize deviations from the default enablement state");
    require(runtime.applyPendingStates() && !runtime.findPlugin("DefaultPlugin")->enabled,
            "Applying plugin manager state did not update the desired enablement state");

    std::filesystem::remove_all(root);
    return 0;
}
