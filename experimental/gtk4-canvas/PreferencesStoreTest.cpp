#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "PreferencesStore.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(double left, double right) { return std::abs(left - right) < 1e-8; }

}  // namespace

int main() {
    const auto legacy = prototype::parseSettingsXml(R"xml(
<settings>
  <data name="tools"><attribute name="current" type="string" value="showFloatingToolbox"/></data>
  <data name="buttonConfig">
    <data name="stylus"><attribute name="tool" type="string" value="highlighter"/></data>
    <data name="stylus2"><attribute name="tool" type="string" value="showFloatingToolbox"/></data>
  </data>
</settings>
)xml");
    require(legacy.ok && legacy.settings.stylusButtonOne == prototype::ButtonToolAction::Highlighter &&
                    legacy.settings.stylusButtonTwo == prototype::ButtonToolAction::FloatingToolbox &&
                    legacy.settings.activeTool == prototype::Tool::FloatingToolbox,
            "Legacy stylus button mappings were not imported");

    const auto directory = std::filesystem::temp_directory_path() /
                           ("xournalpp-gtk4-preferences-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    prototype::ImportedSettings saved;
    saved.themeVariant = prototype::ThemeVariant::ForceDark;
    saved.showSidebar = false;
    saved.pressureSensitivity = false;
    saved.minimumPressure = 0.17;
    saved.pressureMultiplier = 1.75;
    saved.zoomStep = 0.22;
    saved.scrollZoomStep = 0.035;
    saved.edgePanSpeed = 31;
    saved.edgePanMaxMultiplier = 3.5;
    saved.snapRotation = false;
    saved.snapRotationTolerance = 0.45;
    saved.snapGrid = false;
    saved.snapGridSize = 23.5;
    saved.snapGridTolerance = 0.35;
    saved.autosaveEnabled = true;
    saved.autosaveTimeoutMinutes = 11;
    saved.stylusButtonOne = prototype::ButtonToolAction::Eraser;
    saved.stylusButtonTwo = prototype::ButtonToolAction::FloatingToolbox;
    saved.mainToolRail = {prototype::MainToolAction::Pen, prototype::MainToolAction::SmartSelect,
                          prototype::MainToolAction::Text, prototype::MainToolAction::Hand};
    saved.floatingToolboxRows = {{{"SMART_SELECT", "PEN"}, {}, {"COLOR(8)", "DRAW_ARROW"}, {"UNDO"}}};
    std::string error;
    require(prototype::saveGtk4Preferences(directory, saved, error), "GTK 4 preferences could not be saved");

    prototype::ImportedSettings loaded;
    loaded.themeVariant = prototype::ThemeVariant::ForceLight;
    const auto loadedResult = prototype::loadGtk4Preferences(directory, loaded);
    require(loadedResult.found && loadedResult.warning.empty(), "Saved GTK 4 preferences could not be loaded");
    require(loaded.themeVariant == prototype::ThemeVariant::ForceDark && !loaded.showSidebar &&
                    !loaded.pressureSensitivity && near(loaded.minimumPressure, 0.17) &&
                    near(loaded.pressureMultiplier, 1.75) && near(loaded.zoomStep, 0.22) &&
                    near(loaded.scrollZoomStep, 0.035) && near(loaded.edgePanSpeed, 31) &&
                    near(loaded.edgePanMaxMultiplier, 3.5) && !loaded.snapRotation &&
                    near(loaded.snapRotationTolerance, 0.45) &&
                    !loaded.snapGrid && near(loaded.snapGridSize, 23.5) &&
                    near(loaded.snapGridTolerance, 0.35) &&
                    loaded.autosaveTimeoutMinutes == 11 &&
                    loaded.stylusButtonOne == prototype::ButtonToolAction::Eraser &&
                    loaded.stylusButtonTwo == prototype::ButtonToolAction::FloatingToolbox &&
                    loaded.mainToolRail == saved.mainToolRail &&
                    loaded.floatingToolboxRows == saved.floatingToolboxRows,
            "Saved GTK 4 preference values did not roundtrip");

    const auto preferencePath = directory / "gtk4-canvas.ini";
    const auto outside = directory / "outside.ini";
    std::filesystem::rename(preferencePath, outside);
    std::filesystem::create_symlink(outside, preferencePath);
    loaded.showSidebar = true;
    const auto symlinkResult = prototype::loadGtk4Preferences(directory, loaded);
    require(symlinkResult.found && !symlinkResult.warning.empty() && loaded.showSidebar,
            "A symlink preferences file must be refused without changing imported settings");
    error.clear();
    require(!prototype::saveGtk4Preferences(directory, saved, error) && !error.empty(),
            "Saving must refuse to replace a symlink preferences file");

    std::filesystem::remove_all(directory);

    std::filesystem::create_directories(directory);
    {
        std::ofstream invalidRail(directory / "gtk4-canvas.ini");
        invalidRail << "[Toolbar]\nmain-tool-rail=pen,unknown\n";
    }
    prototype::ImportedSettings invalidLayout;
    const auto invalidLayoutResult = prototype::loadGtk4Preferences(directory, invalidLayout);
    require(invalidLayoutResult.found && invalidLayoutResult.warning.empty() &&
                    invalidLayout.mainToolRail == std::vector<prototype::MainToolAction>(
                            prototype::defaultMainToolRail.begin(), prototype::defaultMainToolRail.end()),
            "Invalid main tool rail preferences should leave the default layout intact");

    std::filesystem::remove_all(directory);
    return 0;
}
