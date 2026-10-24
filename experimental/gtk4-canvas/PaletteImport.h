#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prototype {

struct PaletteSwatch {
    std::string name;
    std::array<double, 3> color{};
    std::string cssHex;
};

struct PaletteParseResult {
    bool ok = false;
    std::string paletteName = "Xournal Palette";
    std::optional<int> columns;
    std::vector<PaletteSwatch> swatches;
    std::string error;
};

struct PaletteImportResult {
    std::string paletteName = "Xournal Palette";
    int columns = 6;
    std::vector<PaletteSwatch> swatches;
    std::filesystem::path loadedPath;
    bool usedFallback = true;
    std::string warning;
};

namespace detail {

inline constexpr std::size_t maxPaletteBytes = 1024 * 1024;

inline std::string_view trimAscii(std::string_view value) {
    constexpr std::string_view ws = " \t\r\n\v\f";
    const auto first = value.find_first_not_of(ws);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

inline std::string formatCssHex(uint8_t r, uint8_t g, uint8_t b) {
    static constexpr char hexDigits[] = "0123456789abcdef";
    std::string out = "#000000";
    out[1] = hexDigits[(r >> 4) & 0x0f];
    out[2] = hexDigits[r & 0x0f];
    out[3] = hexDigits[(g >> 4) & 0x0f];
    out[4] = hexDigits[g & 0x0f];
    out[5] = hexDigits[(b >> 4) & 0x0f];
    out[6] = hexDigits[b & 0x0f];
    return out;
}

inline PaletteSwatch makeSwatch(std::string name, uint8_t r, uint8_t g, uint8_t b) {
    auto hex = formatCssHex(r, g, b);
    if (name.empty()) {
        name = hex;
    }
    return {std::move(name), {r / 255.0, g / 255.0, b / 255.0}, std::move(hex)};
}

}  // namespace detail

// Exact 11-color default palette from Palette::default_palette() in
// src/core/gui/toolbarMenubar/model/ColorPalette.cpp and palettes/xournal.gpl.
inline std::vector<PaletteSwatch> defaultPaletteSwatches() {
    return {
            detail::makeSwatch("Black", 0, 0, 0),
            detail::makeSwatch("Green", 0, 128, 0),
            detail::makeSwatch("Light Blue", 0, 192, 255),
            detail::makeSwatch("Light Green", 0, 255, 0),
            detail::makeSwatch("Blue", 51, 51, 204),
            detail::makeSwatch("Gray", 128, 128, 128),
            detail::makeSwatch("Red", 255, 0, 0),
            detail::makeSwatch("Magenta", 255, 0, 255),
            detail::makeSwatch("Orange", 255, 128, 0),
            detail::makeSwatch("Yellow", 255, 255, 0),
            detail::makeSwatch("White", 255, 255, 255),
    };
}

// Strictly read-only, locale-independent parser for GIMP .gpl palettes.
// Eliminates legacy UB in ColorPalette.cpp where parseCommentLine() called
// line.front() before parseEmptyLine() on blank lines.
inline PaletteParseResult parsePaletteText(std::string_view content) {
    PaletteParseResult result;
    if (content.empty()) {
        result.error = ".gpl file needs to start with \"GIMP Palette\" in the first line";
        return result;
    }

    std::size_t cursor = 0;
    int lineNumber = 0;
    auto nextLine = [&](std::string_view& out) -> bool {
        if (cursor > content.size()) {
            return false;
        }
        if (cursor == content.size()) {
            cursor = content.size() + 1;
            return false;
        }
        const auto nl = content.find('\n', cursor);
        if (nl == std::string_view::npos) {
            out = content.substr(cursor);
            cursor = content.size() + 1;
        } else {
            out = content.substr(cursor, nl - cursor);
            cursor = nl + 1;
        }
        if (!out.empty() && out.back() == '\r') {
            out.remove_suffix(1);
        }
        ++lineNumber;
        return true;
    };

    std::string_view firstLine;
    if (!nextLine(firstLine) || detail::trimAscii(firstLine) != "GIMP Palette") {
        result.error = ".gpl file needs to start with \"GIMP Palette\" in the first line";
        return result;
    }

    std::string_view line;
    while (nextLine(line)) {
        const auto trimmed = detail::trimAscii(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.front() == '#') {
            continue;
        }

        // Attempt R G B Name color parsing first when leading tokens are integers.
        const char* p = line.data();
        const char* const end = line.data() + line.size();
        auto skipSpaces = [&]() {
            while (p < end && (*p == ' ' || *p == '\t')) {
                ++p;
            }
        };
        skipSpaces();

        int channels[3]{};
        bool parsedThreeInts = true;
        for (int c = 0; c < 3; ++c) {
            if (c > 0) {
                if (p >= end || (*p != ' ' && *p != '\t')) {
                    parsedThreeInts = false;
                    break;
                }
                skipSpaces();
            }
            if (p >= end || *p == '+' || *p == '-') {
                parsedThreeInts = false;
                break;
            }
            const auto [next, ec] = std::from_chars(p, end, channels[c]);
            if (ec != std::errc{}) {
                parsedThreeInts = false;
                break;
            }
            p = next;
        }

        if (parsedThreeInts && (p == end || *p == ' ' || *p == '\t')) {
            // Mirror NamedColor::operator>> in src/util/NamedColor.cpp: a color
            // entry requires trailing characters after the blue channel.
            if (p == end) {
                result.error = "The line " + std::to_string(lineNumber) + " is malformed.";
                return result;
            }
            if (channels[0] < 0 || channels[0] > 255 || channels[1] < 0 || channels[1] > 255 || channels[2] < 0 ||
                channels[2] > 255) {
                result.error = "RGB values bigger than 255 are not supported.";
                return result;
            }
            const auto swatchName = std::string(detail::trimAscii(std::string_view(p, static_cast<size_t>(end - p))));
            result.swatches.push_back(detail::makeSwatch(swatchName, static_cast<uint8_t>(channels[0]),
                                                         static_cast<uint8_t>(channels[1]),
                                                         static_cast<uint8_t>(channels[2])));
            continue;
        }

        if (const auto colon = line.find(':'); colon != std::string_view::npos) {
            const auto attr = detail::trimAscii(line.substr(0, colon));
            const auto val = detail::trimAscii(line.substr(colon + 1));
            if (attr == "Name" && !val.empty()) {
                result.paletteName = std::string(val);
            } else if (attr == "Columns" && !val.empty()) {
                int cols = 0;
                const auto [next, ec] = std::from_chars(val.data(), val.data() + val.size(), cols);
                if (ec == std::errc{} && next == val.data() + val.size() && cols >= 1 && cols <= 16) {
                    result.columns = cols;
                }
            }
            continue;
        }

        result.error = "The line " + std::to_string(lineNumber) + " is malformed.";
        return result;
    }

    if (result.swatches.empty()) {
        result.error = "Your Palettefile has no parsable color. It needs at least one!";
        return result;
    }

    result.ok = true;
    return result;
}

inline PaletteParseResult loadPaletteFile(const std::filesystem::path& path) {
    PaletteParseResult result;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found) {
        result.error = "Palette file does not exist: " + path.string();
        return result;
    }
    if (status.type() == std::filesystem::file_type::symlink) {
        result.error = "Refusing symlink palette file: " + path.string();
        return result;
    }
    if (!std::filesystem::is_regular_file(status)) {
        result.error = "Palette path is not a regular file: " + path.string();
        return result;
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > detail::maxPaletteBytes) {
        result.error = "Palette file exceeds size limit or cannot be stat'd: " + path.string();
        return result;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "Cannot open palette file: " + path.string();
        return result;
    }
    std::string content(static_cast<std::size_t>(bytes), '\0');
    if (bytes > 0 && !in.read(content.data(), static_cast<std::streamsize>(bytes))) {
        result.error = "Failed reading palette file: " + path.string();
        return result;
    }
    return parsePaletteText(content);
}

// Resolve a configured .gpl palette path strictly read-only without creating
// directories or files on disk, falling back cleanly to defaultPaletteSwatches().
inline PaletteImportResult resolvePalette(const std::filesystem::path& configuredPath,
                                          const std::filesystem::path& configDir = {}) {
    PaletteImportResult fallback;
    fallback.swatches = defaultPaletteSwatches();
    if (configuredPath.empty()) {
        return fallback;
    }

    std::vector<std::filesystem::path> candidates;
    if (configuredPath.is_absolute()) {
        candidates.push_back(configuredPath);
        if (!configDir.empty() && configuredPath.has_filename()) {
            candidates.push_back(configDir / "palettes" / configuredPath.filename());
        }
    } else if (!configDir.empty()) {
        candidates.push_back(configDir / configuredPath);
        if (configuredPath.parent_path().empty()) {
            candidates.push_back(configDir / "palettes" / configuredPath);
        }
    } else {
        candidates.push_back(configuredPath);
    }

    std::string firstProblem;
    for (const auto& candidate: candidates) {
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(candidate, ec);
        if (ec || status.type() == std::filesystem::file_type::not_found) {
            if (firstProblem.empty()) {
                firstProblem = "Palette file does not exist: " + candidate.string();
            }
            continue;
        }
        auto parsed = loadPaletteFile(candidate);
        if (parsed.ok) {
            PaletteImportResult imported;
            imported.paletteName = std::move(parsed.paletteName);
            imported.columns = parsed.columns ? std::clamp(*parsed.columns, 4, 8) : 6;
            imported.swatches = std::move(parsed.swatches);
            imported.loadedPath = candidate;
            imported.usedFallback = false;
            return imported;
        }
        firstProblem = candidate.string() + ": " + parsed.error;
        break;
    }

    fallback.warning = std::move(firstProblem);
    return fallback;
}

}  // namespace prototype
