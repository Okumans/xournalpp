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

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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
    const auto dialogPlugin = root / "DialogPlugin";
    writePlugin(dialogPlugin,
                "[about]\ndescription=Asynchronous plugin dialog\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Dialog', callback='showDialog'})\n"
                "  app.registerUi({menu='Error Dialog', callback='showErrorDialog'})\n"
                "end\n"
                "function showDialog() app.openDialog('Choose one', {'First', 'Second'}, 'dialogResult') end\n"
                "function showErrorDialog() app.openDialog('Problem', {'OK'}, '', true) end\n"
                "function dialogResult(result) app.changeActionState('test-mode', result) end\n");
    const auto backgroundPlugin = root / "BackgroundPlugin";
    writePlugin(backgroundPlugin,
                "[about]\ndescription=Page background action\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi() app.registerUi({menu='Set paper', callback='setPaper'}) end\n"
                "function setPaper() app.changeCurrentPageBackground('graph', 'r1=14.17') end\n");
    const auto beamerPlugin = root / "BeamerPresentation";
    writePlugin(beamerPlugin,
                "[about]\ndescription=Beamer presentation controls\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                readFile(std::filesystem::path(XOURNALPP_REPO_ROOT) / "plugins/BeamerPresentation/main.lua"));
    const auto layerActionsPlugin = root / "LayerActions";
    writePlugin(layerActionsPlugin,
                "[about]\ndescription=Layer operations\n[default]\nenabled=false\n"
                "[plugin]\nmainfile=main.lua\n",
                readFile(std::filesystem::path(XOURNALPP_REPO_ROOT) / "plugins/LayerActions/main.lua"));
    const auto strokeApiPlugin = root / "StrokeApiPlugin";
    writePlugin(strokeApiPlugin,
                "[about]\ndescription=Read editable strokes\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Read strokes', callback='readStrokes'})\n"
                "  app.registerUi({menu='Read selection', callback='readSelection'})\n"
                "end\n"
                "function readStrokes()\n"
                "  local strokes = app.getStrokes('all')\n"
                "  assert(#strokes == 2)\n"
                "  assert(strokes[1].x[2] == 12.5 and strokes[1].y[2] == 24.5)\n"
                "  assert(strokes[1].pressure[2] == 0.75 and strokes[1].tool == 'pen')\n"
                "  assert(strokes[1].width == 2.25 and strokes[1].color == 0x123456)\n"
                "  assert(strokes[1].fill == -1 and strokes[1].lineStyle == 'plain')\n"
                "  assert(strokes[1].ref ~= nil and strokes[1].page == 2 and strokes[1].layer == 3)\n"
                "  assert(strokes[2].pressure == nil and strokes[2].page == 1 and strokes[2].layer == 1)\n"
                "  app.changeActionState('stroke-count', #strokes)\n"
                "end\n"
                "function readSelection() app.getStrokes('selection') end\n");
    const auto strokeMutationPlugin = root / "StrokeMutationPlugin";
    writePlugin(strokeMutationPlugin,
                "[about]\ndescription=Create strokes and edit page layers\n[default]\nenabled=true\n"
                "[plugin]\nmainfile=main.lua\n",
                "function initUi()\n"
                "  app.registerUi({menu='Mutate strokes', callback='mutate'})\n"
                "  app.registerUi({menu='Reject malformed strokes', callback='malformed'})\n"
                "end\n"
                "function mutate()\n"
                "  local refs = app.addStrokes({strokes={\n"
                "    {x={1,2}, y={3,4}, pressure={0.5,0.75}, tool='pen', width=2.5, color=0x123456, fill=-1, lineStyle='plain'},\n"
                "    {x={5,6}, y={7,8}, tool='highlighter'}\n"
                "  }, allowUndoRedoAction='individual'})\n"
                "  assert(#refs == 2 and refs[1] ~= nil and refs[2] ~= nil)\n"
                "  app.setPageSize(50,60,true)\n"
                "  app.setLayerVisibility(false)\n"
                "  app.setCurrentLayerName('moved for fitting')\n"
                "  app.activateAction('layer-new-above-current')\n"
                "end\n"
                "function malformed() app.addStrokes({strokes={{x={1,2}, y={3}}}}) end\n");
    writePlugin(root / "InvalidMain",
                "[about]\ndescription=Invalid\n[default]\nenabled=false\n[plugin]\nmainfile=../outside.lua\n", "");

    prototype::LuaPluginRuntime runtime;
    runtime.discover({root}, colorCycle.string() + "," + layerActionsPlugin.string(), {});
    const auto discovered = runtime.plugins();
    require(discovered.size() == 11, "Plugin discovery did not enumerate all plugin directories");
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
    std::int64_t strokeCount = -1;
    std::vector<prototype::PluginStrokeInput> addedStrokes;
    prototype::PluginStrokeUndoPolicy addedStrokePolicy = prototype::PluginStrokeUndoPolicy::Grouped;
    double pageWidthChange = 0;
    double pageHeightChange = 0;
    bool pageSizeRelative = false;
    bool layerVisible = true;
    std::string renamedLayer;
    std::string dialogPluginName;
    std::string dialogMessage;
    std::vector<prototype::PluginDialogButton> dialogButtons;
    bool dialogError = true;
    std::function<void(int)> dialogResponse;
    std::string changedBackgroundFormat;
    std::string changedBackgroundConfig;
    std::vector<std::string> activatedActions;
    prototype::PluginRuntimeHost host;
    host.changeToolColor = [&](const std::optional<std::string>& tool, std::uint32_t color, bool selection,
                               std::string&) {
        changedTool = tool.value_or("");
        changedColor = color;
        changedSelection = selection;
        return true;
    };
    host.changeActionState = [&](std::string_view action, const prototype::PluginActionValue& value, std::string&) {
        if ((action != "test-mode" && action != "stroke-count") ||
            !std::holds_alternative<std::int64_t>(value)) {
            return false;
        }
        if (action == "test-mode") {
            changedMode = std::get<std::int64_t>(value);
        } else {
            strokeCount = std::get<std::int64_t>(value);
        }
        return true;
    };
    host.getStrokes = [](std::string_view type, std::vector<prototype::PluginStroke>& strokes,
                         std::string& error) {
        if (type == "selection") {
            error = "There is no selection";
            return false;
        }
        if (type != "all") {
            error = "Unexpected stroke query type";
            return false;
        }
        strokes = {
                {{10, 12.5}, {22, 24.5}, {0.25, 0.75}, "pen", 2.25, 0x123456, -1, "plain", 0x42, 2, 3},
                {{31, 40}, {42, 51}, {}, "highlighter", 8.5, 0xffcc00, -1, "plain", 0x43, 1, 1},
        };
        return true;
    };
    host.addStrokes = [&](const std::vector<prototype::PluginStrokeInput>& strokes,
                          prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                          std::string&) {
        addedStrokes = strokes;
        addedStrokePolicy = policy;
        references = {0x51, 0x52};
        return true;
    };
    host.setPageSize = [&](double width, double height, bool relative, std::string&) {
        pageWidthChange = width;
        pageHeightChange = height;
        pageSizeRelative = relative;
        return true;
    };
    host.setLayerVisibility = [&](bool visible, std::string&) {
        layerVisible = visible;
        return true;
    };
    host.setCurrentLayerName = [&](std::string_view name, std::string&) {
        renamedLayer = name;
        return true;
    };
    host.changeCurrentPageBackground = [&](std::string_view format, std::string_view config, std::string&) {
        changedBackgroundFormat = format;
        changedBackgroundConfig = config;
        return true;
    };
    host.activateAction = [&](std::string_view action, std::string& error) {
        if (action != "beamer-next" && action != "beamer-previous" && action != "beamer-clean" &&
            action != "layer-actions-clone-next" && action != "layer-actions-hide" &&
            action != "layer-actions-add-top" && action != "layer-new-above-current") {
            error = "Actions are unavailable in this GTK 4 host";
            return false;
        }
        activatedActions.emplace_back(action);
        return true;
    };
    host.openDialog = [&](std::string_view pluginName, std::string_view message,
                          const std::vector<prototype::PluginDialogButton>& buttons, bool isError,
                          std::function<void(int)> response, std::string&) {
        dialogPluginName = pluginName;
        dialogMessage = message;
        dialogButtons = buttons;
        dialogError = isError;
        dialogResponse = std::move(response);
        return true;
    };
    runtime.loadEnabled(std::move(host));

    const auto loadedColorSummary = runtime.findPlugin("ColorCycle");
    require(loadedColorSummary && loadedColorSummary->loaded && loadedColorSummary->menuItems == 1,
            "Enabled Lua plugin did not run initUi and register its menu callback");
    const auto loadedDefaultSummary = runtime.findPlugin("DefaultPlugin");
    require(loadedDefaultSummary && loadedDefaultSummary->loaded, "Default-enabled plugin did not load");
    const auto loadedDialogPlugin = runtime.findPlugin("DialogPlugin");
    require(loadedDialogPlugin && loadedDialogPlugin->loaded && loadedDialogPlugin->menuItems == 2,
            "Plugin using app.openDialog did not load and register its menu callback");
    const auto loadedStrokeApiPlugin = runtime.findPlugin("StrokeApiPlugin");
    require(loadedStrokeApiPlugin && loadedStrokeApiPlugin->loaded && loadedStrokeApiPlugin->menuItems == 2,
            "Plugin using app.getStrokes did not load and register its menu callbacks");
    const auto loadedStrokeMutationPlugin = runtime.findPlugin("StrokeMutationPlugin");
    require(loadedStrokeMutationPlugin && loadedStrokeMutationPlugin->loaded &&
                    loadedStrokeMutationPlugin->menuItems == 2,
            "Plugin using stroke creation and layer/page APIs did not load its callbacks");
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
    require(runtime.invokeMenu("BackgroundPlugin", 0) && changedBackgroundFormat == "graph" &&
                    changedBackgroundConfig == "r1=14.17",
            "Page background type and configuration did not reach the GTK4 host callback");
    require(runtime.invokeMenu("StrokeApiPlugin", 0) && strokeCount == 2,
            "app.getStrokes did not preserve point arrays, pressure, style fields, references, and page/layer data");
    require(!runtime.invokeMenu("StrokeApiPlugin", 1) &&
                    runtime.findPlugin("StrokeApiPlugin")->error.find("There is no selection") != std::string::npos,
            "app.getStrokes selection errors were not reported to the plugin");
    require(runtime.invokeMenu("StrokeMutationPlugin", 0) && addedStrokes.size() == 2 &&
                    addedStrokePolicy == prototype::PluginStrokeUndoPolicy::Individual &&
                    addedStrokes[0].x == std::vector<double>({1, 2}) &&
                    addedStrokes[0].y == std::vector<double>({3, 4}) &&
                    addedStrokes[0].pressure == std::vector<double>({0.5, 0.75}) &&
                    addedStrokes[0].tool == std::optional<std::string>("pen") &&
                    addedStrokes[0].width == std::optional<double>(2.5) &&
                    addedStrokes[0].color == std::optional<std::uint32_t>(0x123456) &&
                    addedStrokes[0].fill == std::optional<int>(-1) &&
                    addedStrokes[0].lineStyle == std::optional<std::string>("plain") &&
                    addedStrokes[1].pressure.empty() &&
                    addedStrokes[1].tool == std::optional<std::string>("highlighter") &&
                    pageWidthChange == 50 && pageHeightChange == 60 && pageSizeRelative && !layerVisible &&
                    renamedLayer == "moved for fitting" && activatedActions.back() == "layer-new-above-current",
            "FitToContent stroke, page-size, and layer APIs did not preserve arguments and references");
    activatedActions.clear();
    require(!runtime.invokeMenu("StrokeMutationPlugin", 1) &&
                    runtime.findPlugin("StrokeMutationPlugin")->error.find("not equal length") != std::string::npos,
            "app.addStrokes accepted malformed point arrays");
    require(runtime.invokeMenu("BeamerPresentation", 0) && activatedActions == std::vector<std::string>{"beamer-next"},
            "Beamer +1 did not dispatch the next-animation host action");
    require(runtime.invokeMenu("BeamerPresentation", 1) &&
                    activatedActions == std::vector<std::string>({"beamer-next", "beamer-previous"}),
            "Beamer -1 did not dispatch the previous-animation host action");
    require(runtime.invokeMenu("BeamerPresentation", 2) &&
                    activatedActions == std::vector<std::string>({"beamer-next", "beamer-previous", "beamer-clean"}),
            "Beamer clean did not dispatch the cleanup host action");
    require(runtime.invokeMenu("LayerActions", 0) &&
                    activatedActions.back() == "layer-actions-clone-next",
            "LayerActions clone did not dispatch its GTK host action");
    require(runtime.invokeMenu("LayerActions", 1) && activatedActions.back() == "layer-actions-hide",
            "LayerActions hide did not dispatch its GTK host action");
    require(runtime.invokeMenu("LayerActions", 2) && activatedActions.back() == "layer-actions-add-top",
            "LayerActions add did not dispatch its GTK host action");
    require(runtime.invokeMenu("DialogPlugin", 0) && dialogPluginName == "DialogPlugin" &&
                    dialogMessage == "Choose one" && !dialogError && dialogButtons.size() == 2 &&
                    dialogButtons[0].response == 1 && dialogButtons[0].label == "First" &&
                    dialogButtons[1].response == 2 && dialogButtons[1].label == "Second" && dialogResponse,
            "app.openDialog did not preserve its message, ordered buttons, plugin name, and response callback");
    dialogResponse(0);
    require(changedMode == 7, "Closing a plugin dialog must not invoke its button callback");
    dialogResponse(2);
    require(changedMode == 2, "Plugin dialog button responses must reach the Lua callback as integers");
    require(runtime.invokeMenu("DialogPlugin", 1) && dialogError && dialogMessage == "Problem" &&
                    dialogButtons.size() == 1 && dialogButtons[0].response == 1 &&
                    dialogButtons[0].label == "OK" && dialogResponse,
            "Error dialogs must preserve their error flag and button response data");
    dialogResponse(1);
    require(changedMode == 2, "A plugin dialog without a callback must not activate unrelated plugin actions");
    require(runtime.handleKeyPress(GDK_KEY_R, GDK_SHIFT_MASK, false) && changedMode == 7,
            "Plugin canvas shortcut did not run with legacy shifted-key matching");
    require(!runtime.handleKeyPress(GDK_KEY_r, GdkModifierType(0), true),
            "Plugin shortcuts must not steal keyboard input from text editing");

    auto* actionGroup = g_simple_action_group_new();
    auto* menu = g_menu_new();
    require(runtime.appendMenuSections(menu, G_ACTION_MAP(actionGroup), nullptr) == 15 &&
                    g_menu_model_get_n_items(G_MENU_MODEL(menu)) == 8,
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
