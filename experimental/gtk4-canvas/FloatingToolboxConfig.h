#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <glib.h>

#include "CanvasDrawing.h"

namespace prototype {

using FloatingToolboxRows = std::array<std::vector<std::string>, 4>;

struct FloatingDrawingModeAction {
    std::string_view token;
    DrawingMode mode;
    std::string_view label;
};

inline constexpr std::array<FloatingDrawingModeAction, 8> floatingDrawingModeActions = {{
        {"RULER", DrawingMode::Line, "Line"},
        {"DRAW_RECTANGLE", DrawingMode::Rectangle, "Rectangle"},
        {"DRAW_ELLIPSE", DrawingMode::Ellipse, "Ellipse"},
        {"DRAW_ARROW", DrawingMode::Arrow, "Arrow"},
        {"DRAW_DOUBLE_ARROW", DrawingMode::DoubleArrow, "Double arrow"},
        {"DRAW_COORDINATE_SYSTEM", DrawingMode::CoordinateSystem, "Coordinate system"},
        {"DRAW_SPLINE", DrawingMode::Spline, "Spline"},
        {"SHAPE_RECOGNIZER", DrawingMode::ShapeRecognizer, "Shape recognizer"},
}};

inline std::optional<FloatingDrawingModeAction> floatingDrawingModeAction(std::string_view token) {
    if (token == "DRAW_LINE") {
        return floatingDrawingModeActions[0];
    }
    if (token == "DRAW_RECT") {
        return floatingDrawingModeActions[1];
    }

    const auto action = std::find_if(floatingDrawingModeActions.begin(), floatingDrawingModeActions.end(),
                                     [token](const auto& candidate) { return candidate.token == token; });
    return action == floatingDrawingModeActions.end() ? std::nullopt : std::optional(*action);
}

inline constexpr std::array<std::string_view, 4> defaultFloatingToolboxRowText = {
        "PEN,HIGHLIGHTER,LASER_POINTER,PDF_TOOL,UNDO",
        "ZOOM_SLIDER,DRAW_ELLIPSE",
        "TOOL_FILL,FINE,THICK,DRAW_RECTANGLE",
        "COLOR(10),COLOR(1),COLOR(8),COLOR(4),DRAW_ARROW",
};

inline constexpr std::size_t maxFloatingToolboxConfigBytes = 1024 * 1024;
inline constexpr std::size_t maxFloatingToolboxItemsPerRow = 64;
inline constexpr std::size_t maxFloatingToolboxTokenBytes = 64;

namespace floating_toolbox_detail {

inline std::string_view trimAscii(std::string_view value) {
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

inline std::optional<std::string> normalizeToken(std::string_view raw) {
    auto tokenView = trimAscii(raw);
    if (tokenView.empty() || tokenView.size() > maxFloatingToolboxTokenBytes) {
        return std::nullopt;
    }

    constexpr std::string_view pluginPrefix = "Plugin::";
    if (tokenView.size() > pluginPrefix.size()) {
        bool isPluginToken = true;
        for (std::size_t index = 0; index < pluginPrefix.size(); ++index) {
            const auto actual = static_cast<unsigned char>(tokenView[index]);
            const auto expected = static_cast<unsigned char>(pluginPrefix[index]);
            if (static_cast<unsigned char>(std::tolower(actual)) !=
                static_cast<unsigned char>(std::tolower(expected))) {
                isPluginToken = false;
                break;
            }
        }
        if (isPluginToken) {
            const auto id = tokenView.substr(pluginPrefix.size());
            if (id.size() + pluginPrefix.size() > maxFloatingToolboxTokenBytes ||
                std::any_of(id.begin(), id.end(), [](unsigned char character) {
                    return !((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
                             (character >= '0' && character <= '9') || character == '_' || character == '-' ||
                             character == '.');
                })) {
                return std::nullopt;
            }
            return std::string(pluginPrefix) + std::string(id);
        }
    }

    std::string token;
    token.reserve(tokenView.size());
    for (const unsigned char character: tokenView) {
        if (character >= 'a' && character <= 'z') {
            token.push_back(static_cast<char>(character - 'a' + 'A'));
        } else {
            token.push_back(static_cast<char>(character));
        }
    }

    const auto openParen = token.find('(');
    if (openParen != std::string::npos) {
        if (token.compare(0, 6, "COLOR(") != 0 || token.back() != ')' || openParen != 5 || token.size() <= 7) {
            return std::nullopt;
        }
        for (std::size_t index = 6; index + 1 < token.size(); ++index) {
            if (token[index] < '0' || token[index] > '9') {
                return std::nullopt;
            }
        }
    } else {
        for (const unsigned char character: token) {
            if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
                  character == '_')) {
                return std::nullopt;
            }
        }
    }
    return token;
}

}  // namespace floating_toolbox_detail

inline FloatingToolboxRows defaultFloatingToolboxRows() {
    FloatingToolboxRows rows;
    for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
        const auto text = defaultFloatingToolboxRowText[rowIndex];
        std::size_t start = 0;
        while (start < text.size()) {
            const auto end = text.find(',', start);
            const auto item = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
            if (auto token = floating_toolbox_detail::normalizeToken(item)) {
                rows[rowIndex].push_back(std::move(*token));
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }
    return rows;
}

inline std::optional<std::vector<std::string>> parseFloatingToolboxRow(std::string_view text) {
    std::vector<std::string> row;
    if (floating_toolbox_detail::trimAscii(text).empty()) {
        return row;
    }

    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(',', start);
        const auto item = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        auto token = floating_toolbox_detail::normalizeToken(item);
        if (!token || row.size() >= maxFloatingToolboxItemsPerRow) {
            return std::nullopt;
        }
        row.push_back(std::move(*token));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return row;
}

inline std::string serializeFloatingToolboxRow(const std::vector<std::string>& row) {
    std::string text;
    for (const auto& token: row) {
        if (!text.empty()) {
            text.push_back(',');
        }
        text += token;
    }
    return text;
}

struct FloatingToolboxConfigResult {
    FloatingToolboxRows rows = defaultFloatingToolboxRows();
    bool found = false;
    std::string warning;
};

inline FloatingToolboxConfigResult importFloatingToolboxConfig(const std::filesystem::path& configDir) {
    FloatingToolboxConfigResult result;
    if (configDir.empty()) {
        return result;
    }

    const auto path = configDir / "toolbar.ini";
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if ((!ec && status.type() == std::filesystem::file_type::not_found) ||
        ec == std::errc::no_such_file_or_directory) {
        return result;
    }
    result.found = true;
    if (ec || status.type() == std::filesystem::file_type::symlink || !std::filesystem::is_regular_file(status)) {
        result.warning = "Refusing non-regular toolbar configuration: " + path.string();
        return result;
    }

    const auto length = std::filesystem::file_size(path, ec);
    if (ec || length > maxFloatingToolboxConfigBytes) {
        result.warning = "Toolbar configuration exceeds its size limit: " + path.string();
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.warning = "Cannot read toolbar configuration: " + path.string();
        return result;
    }
    std::string contents(static_cast<std::size_t>(length), '\0');
    if (length > 0 && !input.read(contents.data(), static_cast<std::streamsize>(length))) {
        result.warning = "Failed reading toolbar configuration: " + path.string();
        return result;
    }

    GKeyFile* keyFile = g_key_file_new();
    GError* error = nullptr;
    if (!g_key_file_load_from_data(keyFile, contents.data(), contents.size(), G_KEY_FILE_NONE, &error)) {
        result.warning = path.string() + ": " + (error ? error->message : "invalid key file");
        g_clear_error(&error);
        g_key_file_unref(keyFile);
        return result;
    }

    static constexpr std::array<const char*, 4> keys = {
            "toolbarFloat1", "toolbarFloat2", "toolbarFloat3", "toolbarFloat4",
    };
    constexpr const char* group = "Floating Toolbox (experimental)";
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (!g_key_file_has_key(keyFile, group, keys[index], nullptr)) {
            continue;
        }
        gchar* value = g_key_file_get_string(keyFile, group, keys[index], &error);
        if (error) {
            result.warning = path.string() + ": " + error->message;
            g_clear_error(&error);
            continue;
        }
        const auto parsed = parseFloatingToolboxRow(value ? value : "");
        if (parsed) {
            result.rows[index] = *parsed;
        } else {
            result.warning = path.string() + ": invalid " + keys[index] + " value";
        }
        g_free(value);
    }
    g_key_file_unref(keyFile);
    return result;
}

}  // namespace prototype
