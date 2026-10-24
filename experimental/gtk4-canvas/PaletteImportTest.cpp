#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "PaletteImport.h"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

}  // namespace

int main() {
    const auto repoRoot = std::filesystem::path(XOURNALPP_REPO_ROOT);

    // 1. Default 11-color palette matches Palette::default_palette() and palettes/xournal.gpl.
    const auto defaults = prototype::defaultPaletteSwatches();
    require(defaults.size() == 11, "Default palette must have 11 swatches");
    require(defaults[0].name == "Black" && defaults[0].cssHex == "#000000", "Default swatch 0 must be Black");
    require(defaults[4].name == "Blue" && defaults[4].cssHex == "#3333cc" && near(defaults[4].color[0], 51 / 255.0) &&
                    near(defaults[4].color[2], 204 / 255.0),
            "Default swatch 4 must be Xournal Blue #3333cc");
    require(defaults[10].name == "White" && defaults[10].cssHex == "#ffffff", "Default swatch 10 must be White");

    const auto xournalGpl = prototype::loadPaletteFile(repoRoot / "palettes/xournal.gpl");
    require(xournalGpl.ok && xournalGpl.swatches.size() == defaults.size(), "palettes/xournal.gpl failed to load");
    for (size_t i = 0; i < defaults.size(); ++i) {
        require(xournalGpl.swatches[i].name == defaults[i].name && xournalGpl.swatches[i].cssHex == defaults[i].cssHex,
                "palettes/xournal.gpl diverged from defaultPaletteSwatches");
    }

    // 2. Built-in Xournal++ palette with leading channel padding (e.g. "  0  46 153 Dark Blue").
    const auto xoppGpl = prototype::loadPaletteFile(repoRoot / "palettes/xournalpp.gpl");
    require(xoppGpl.ok && xoppGpl.paletteName == "Xournal++ Palette" && xoppGpl.swatches.size() == 11,
            "palettes/xournalpp.gpl failed to parse");
    require(xoppGpl.swatches[2].name == "Purple" && xoppGpl.swatches[2].cssHex == "#cd9ef7",
            "xournalpp.gpl Purple swatch mismatch");
    require(xoppGpl.swatches[8].name == "Dark Blue" && xoppGpl.swatches[8].cssHex == "#002e99",
            "xournalpp.gpl Dark Blue swatch mismatch");

    // 3. Parity with test/unit_tests/model/ColorPaletteTest.cpp fixtures.
    const auto rainbow = prototype::loadPaletteFile(repoRoot / "test/files/palettes/rainbow.gpl");
    require(rainbow.ok && rainbow.paletteName == "Rainbow" && rainbow.swatches.size() == 6,
            "rainbow.gpl failed to parse");
    require(rainbow.swatches[4].name == "Royal Blue" && rainbow.swatches[4].cssHex == "#004dff",
            "rainbow.gpl Royal Blue mismatch");

    const auto notBrokenAttr = prototype::loadPaletteFile(repoRoot / "test/files/palettes/not_broken_attribute.gpl");
    require(notBrokenAttr.ok && notBrokenAttr.swatches.size() == 2 && notBrokenAttr.swatches[0].name == "Gray",
            "not_broken_attribute.gpl should parse cleanly");

    require(!prototype::loadPaletteFile(repoRoot / "test/files/palettes/empty.gpl").ok, "empty.gpl must be rejected");
    require(!prototype::loadPaletteFile(repoRoot / "test/files/palettes/wrong_header.gpl").ok,
            "wrong_header.gpl must be rejected");
    require(!prototype::loadPaletteFile(repoRoot / "test/files/palettes/broken_attribute.gpl").ok,
            "broken_attribute.gpl must be rejected");

    const auto bad1 = prototype::loadPaletteFile(repoRoot / "test/files/palettes/bad_color_1.gpl");
    require(!bad1.ok && bad1.error == "RGB values bigger than 255 are not supported.",
            "bad_color_1.gpl error mismatch");
    const auto bad2 = prototype::loadPaletteFile(repoRoot / "test/files/palettes/bad_color_2.gpl");
    require(!bad2.ok && bad2.error == "The line 4 is malformed.", "bad_color_2.gpl error mismatch");
    const auto bad3 = prototype::loadPaletteFile(repoRoot / "test/files/palettes/bad_color_3.gpl");
    require(!bad3.ok && bad3.error == "The line 4 is malformed.", "bad_color_3.gpl error mismatch");

    // 4. Blank lines, whitespace-only lines, CRLF endings, Columns header, and colons inside color names
    // (eliminating legacy ColorPalette.cpp parseCommentLine line.front() UB on empty lines).
    const auto crlfWithBlanks = prototype::parsePaletteText(
            "GIMP Palette\r\n"
            "\r\n"
            "Name: Clean Studio\r\n"
            "Columns: 5\r\n"
            "   \t  \r\n"
            "# Comment after blank line\r\n"
            "16 32 64 Accent: Midnight\r\n"
            "\r\n"
            "240 128 0   \r\n");
    require(crlfWithBlanks.ok && crlfWithBlanks.paletteName == "Clean Studio" && crlfWithBlanks.columns == 5 &&
                    crlfWithBlanks.swatches.size() == 2,
            "CRLF + blank lines palette failed to parse");
    require(crlfWithBlanks.swatches[0].name == "Accent: Midnight" && crlfWithBlanks.swatches[0].cssHex == "#102040",
            "Colon-bearing swatch name mismatch");
    require(crlfWithBlanks.swatches[1].name == "#f08000" && crlfWithBlanks.swatches[1].cssHex == "#f08000",
            "Unnamed trailing-space swatch did not fall back to hex label");

    // 5. Read-only path resolution, symlink refusal, and zero writes to config directory.
    const auto scratch = std::filesystem::temp_directory_path() /
                         ("xournalpp-palette-import-test-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch / "palettes");
    {
        std::ofstream custom(scratch / "palettes" / "custom.gpl", std::ios::binary);
        custom << "GIMP Palette\nName: Custom User\nColumns: 4\n#\n12 34 56 Deep Slate\n";
    }
    std::filesystem::create_symlink(scratch / "palettes" / "custom.gpl", scratch / "palettes" / "linked.gpl");

    const auto byShortName = prototype::resolvePalette("custom.gpl", scratch);
    require(!byShortName.usedFallback && byShortName.paletteName == "Custom User" && byShortName.columns == 4 &&
                    byShortName.swatches.size() == 1 && byShortName.swatches[0].name == "Deep Slate",
            "Short filename in <config-dir>/palettes was not resolved");

    const auto symlinkResult = prototype::resolvePalette(scratch / "palettes" / "linked.gpl", scratch);
    require(symlinkResult.usedFallback && symlinkResult.swatches.size() == 11 && !symlinkResult.warning.empty(),
            "Symlink palette must be refused and fall back to default swatches");

    const auto missingDir = scratch / "uncreated-config-dir";
    const auto missingResult = prototype::resolvePalette("missing.gpl", missingDir);
    require(missingResult.usedFallback && missingResult.swatches.size() == 11 && !std::filesystem::exists(missingDir),
            "resolvePalette must never create missing config directories");

    std::filesystem::remove_all(scratch);
    return 0;
}
