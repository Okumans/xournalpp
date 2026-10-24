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
  <data name="buttonConfig">
    <data name="stylus"><attribute name="tool" type="string" value="highlighter"/></data>
    <data name="stylus2"><attribute name="tool" type="string" value="hand"/></data>
  </data>
</settings>
)xml");
    require(legacy.ok && legacy.settings.stylusButtonOne == prototype::ButtonToolAction::Highlighter &&
                    legacy.settings.stylusButtonTwo == prototype::ButtonToolAction::Hand,
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
    saved.autosaveEnabled = true;
    saved.autosaveTimeoutMinutes = 11;
    saved.stylusButtonOne = prototype::ButtonToolAction::Eraser;
    saved.stylusButtonTwo = prototype::ButtonToolAction::Pen;
    std::string error;
    require(prototype::saveGtk4Preferences(directory, saved, error), "GTK 4 preferences could not be saved");

    prototype::ImportedSettings loaded;
    loaded.themeVariant = prototype::ThemeVariant::ForceLight;
    const auto loadedResult = prototype::loadGtk4Preferences(directory, loaded);
    require(loadedResult.found && loadedResult.warning.empty(), "Saved GTK 4 preferences could not be loaded");
    require(loaded.themeVariant == prototype::ThemeVariant::ForceDark && !loaded.showSidebar &&
                    !loaded.pressureSensitivity && near(loaded.minimumPressure, 0.17) &&
                    near(loaded.pressureMultiplier, 1.75) && near(loaded.zoomStep, 0.22) &&
                    near(loaded.scrollZoomStep, 0.035) && loaded.autosaveTimeoutMinutes == 11 &&
                    loaded.stylusButtonOne == prototype::ButtonToolAction::Eraser &&
                    loaded.stylusButtonTwo == prototype::ButtonToolAction::Pen,
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
    return 0;
}
