#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "SettingsImport.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto defaultRows = prototype::defaultFloatingToolboxRows();
    require(defaultRows[0] == std::vector<std::string>({"PEN", "HIGHLIGHTER", "LASER_POINTER", "PDF_TOOL", "UNDO"}) &&
                    defaultRows[1] == std::vector<std::string>({"ZOOM_SLIDER", "DRAW_ELLIPSE"}) &&
                    defaultRows[2] == std::vector<std::string>({"TOOL_FILL", "FINE", "THICK", "DRAW_RECTANGLE"}) &&
                    defaultRows[3] == std::vector<std::string>({"COLOR(10)", "COLOR(1)", "COLOR(8)", "COLOR(4)",
                                                                "DRAW_ARROW"}),
            "GTK 4 defaults do not match the four master floating-toolbar rows");

    const auto parsed = prototype::parseFloatingToolboxRow(" pen, HIGHLIGHTER, COLOR(10), custom_action ");
    require(parsed && *parsed == std::vector<std::string>({"PEN", "HIGHLIGHTER", "COLOR(10)", "CUSTOM_ACTION"}),
            "Toolbar row parsing did not trim and normalize item names");
    require(prototype::serializeFloatingToolboxRow(*parsed) == "PEN,HIGHLIGHTER,COLOR(10),CUSTOM_ACTION",
            "Toolbar row serialization was not stable");
    require(!prototype::parseFloatingToolboxRow("PEN,,ERASER") &&
                    !prototype::parseFloatingToolboxRow("PEN,COLOR(foo)") &&
                    !prototype::parseFloatingToolboxRow("PEN,TOOL NAME"),
            "Malformed toolbar items were accepted");

    const auto directory = std::filesystem::temp_directory_path() /
                           ("xournalpp-gtk4-toolbar-config-" + std::to_string(::getpid()));
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const auto path = directory / "toolbar.ini";
    {
        std::ofstream out(path);
        out << "[Floating Toolbox (experimental)]\n"
               "toolbarFloat1=PEN, SMART_SELECT,UNDO\n"
               "toolbarFloat2=\n"
               "toolbarFloat4=COLOR(3),DRAW_ARROW\n";
    }
    auto imported = prototype::importFloatingToolboxConfig(directory);
    require(imported.found && imported.warning.empty() &&
                    imported.rows[0] == std::vector<std::string>({"PEN", "SMART_SELECT", "UNDO"}) &&
                    imported.rows[1].empty() &&
                    imported.rows[2] == defaultRows[2] &&
                    imported.rows[3] == std::vector<std::string>({"COLOR(3)", "DRAW_ARROW"}),
            "Legacy toolbar.ini rows did not import independently with defaults for absent rows");
    const auto importedSettings = prototype::importSettingsFromConfigDir(directory);
    require(importedSettings.settings.floatingToolboxRows == imported.rows,
            "Settings import did not expose the legacy floating-toolbox rows");

    {
        std::ofstream out(path);
        out << "[Floating Toolbox (experimental)]\n"
               "toolbarFloat1=PEN,,ERASER\n"
               "toolbarFloat2=HIGHLIGHTER\n";
    }
    imported = prototype::importFloatingToolboxConfig(directory);
    require(!imported.warning.empty() && imported.rows[0] == defaultRows[0] &&
                    imported.rows[1] == std::vector<std::string>({"HIGHLIGHTER"}),
            "An invalid toolbar row did not fall back independently");

    const auto outside = directory / "outside.ini";
    std::filesystem::rename(path, outside);
    std::filesystem::create_symlink(outside, path);
    imported = prototype::importFloatingToolboxConfig(directory);
    require(imported.found && !imported.warning.empty() && imported.rows == defaultRows,
            "A symlink toolbar.ini must be refused without changing default rows");

    std::filesystem::remove_all(directory);
    return 0;
}
