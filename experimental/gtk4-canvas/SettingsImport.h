#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>

#include "BehaviorDefaults.h"
#include "CanvasState.h"
#include "PaletteImport.h"

namespace prototype {

enum class ThemeVariant {
    UseSystem,
    ForceLight,
    ForceDark,
};

struct ImportedSettings {
    Tool activeTool = Tool::Pen;
    std::array<double, 3> penColor = behavior::defaultPenColor;
    double penWidth = behavior::penWidths[static_cast<size_t>(behavior::ToolSize::Medium)];
    std::array<double, 3> highlighterColor = behavior::defaultHighlighterColor;
    double highlighterWidth = behavior::highlighterWidths[static_cast<size_t>(behavior::ToolSize::Medium)];
    double eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::Medium)];
    EraserMode eraserMode = EraserMode::Standard;

    bool pressureSensitivity = true;
    double minimumPressure = behavior::minimumPressure;
    double pressureMultiplier = behavior::pressureMultiplier;
    double zoomStep = behavior::zoomStep;
    double scrollZoomStep = behavior::scrollZoomStep;

    bool showSidebar = true;
    ThemeVariant themeVariant = ThemeVariant::UseSystem;
    bool autosaveEnabled = true;
    int autosaveTimeoutMinutes = 3;
    std::filesystem::path colorPalettePath;

    int effectiveAutosaveSeconds() const {
        if (!autosaveEnabled || autosaveTimeoutMinutes <= 0) {
            return 0;
        }
        return static_cast<int>(std::clamp<long long>(autosaveTimeoutMinutes * 60LL, 1LL, 86400LL));
    }
};

struct SettingsParseResult {
    bool ok = false;
    ImportedSettings settings;
    std::string error;
};

struct SettingsImportResult {
    ImportedSettings settings;
    PaletteImportResult palette;
    bool foundSettingsFile = false;
    bool usedSettingsFallback = false;
    std::string warning;
};

namespace settings_detail {

inline constexpr std::size_t maxSettingsBytes = 4 * 1024 * 1024;

inline std::optional<double> parseAsciiDouble(std::string_view raw) {
    const auto trimmed = detail::trimAscii(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    std::string nulTerminated(trimmed);
    char* end = nullptr;
    const double val = g_ascii_strtod(nulTerminated.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(val)) {
        return std::nullopt;
    }
    return val;
}

inline std::optional<long long> parseAsciiInt(std::string_view raw) {
    const auto trimmed = detail::trimAscii(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    long long val = 0;
    const auto [end, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), val);
    if (ec != std::errc{} || end != trimmed.data() + trimmed.size()) {
        return std::nullopt;
    }
    return val;
}

inline std::optional<std::array<double, 3>> parsePackedColor(std::string_view type, std::string_view raw) {
    const auto trimmed = detail::trimAscii(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }
    uint32_t packed = 0;
    if (type == "hex") {
        auto hex = trimmed;
        if (hex.starts_with("0x") || hex.starts_with("0X")) {
            hex.remove_prefix(2);
        }
        if (hex.empty()) {
            return std::nullopt;
        }
        const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), packed, 16);
        if (ec != std::errc{} || end != hex.data() + hex.size()) {
            return std::nullopt;
        }
    } else if (type == "int") {
        const auto parsed = parseAsciiInt(trimmed);
        if (!parsed) {
            return std::nullopt;
        }
        packed = static_cast<uint32_t>(*parsed);
    } else {
        return std::nullopt;
    }
    return std::array<double, 3>{
            ((packed >> 16) & 0xffU) / 255.0,
            ((packed >> 8) & 0xffU) / 255.0,
            (packed & 0xffU) / 255.0,
    };
}

inline std::optional<behavior::ToolSize> parseToolSizeName(std::string_view value) {
    if (value == "VERY_FINE") {
        return behavior::ToolSize::VeryFine;
    }
    if (value == "THIN" || value == "FINE") {
        return behavior::ToolSize::Fine;
    }
    if (value == "MEDIUM") {
        return behavior::ToolSize::Medium;
    }
    if (value == "BIG" || value == "THICK") {
        return behavior::ToolSize::Thick;
    }
    if (value == "VERY_BIG" || value == "VERY_THICK") {
        return behavior::ToolSize::VeryThick;
    }
    return std::nullopt;
}

inline const char* findAttr(const gchar** names, const gchar** values, std::string_view key) {
    if (!names || !values) {
        return nullptr;
    }
    for (std::size_t i = 0; names[i] != nullptr; ++i) {
        if (key == names[i]) {
            return values[i];
        }
    }
    return nullptr;
}

struct XmlState {
    ImportedSettings candidate;
    std::vector<std::string> elementStack;
    std::vector<std::string> dataStack;
    bool sawSettingsRoot = false;
};

inline void applyProperty(ImportedSettings& out, std::string_view name, std::string_view value) {
    if (name == "presureSensitivity" || name == "pressureSensitivity") {
        out.pressureSensitivity = (value == "true");
    } else if (name == "minimumPressure") {
        if (const auto v = parseAsciiDouble(value)) {
            out.minimumPressure = std::clamp(std::max(0.01, *v), 0.01, 1.0);
        }
    } else if (name == "pressureMultiplier") {
        if (const auto v = parseAsciiDouble(value); v && *v > 0.0) {
            out.pressureMultiplier = std::clamp(*v, 0.05, 10.0);
        }
    } else if (name == "zoomStep") {
        // Settings.cpp stores zoomStep in percent (default 10.0 -> 0.10).
        if (const auto pct = parseAsciiDouble(value); pct && *pct > 0.0) {
            out.zoomStep = std::clamp(*pct / 100.0, 0.005, 2.0);
        }
    } else if (name == "zoomStepScroll") {
        // Settings.cpp stores zoomStepScroll in percent (default 2.0 -> 0.02).
        if (const auto pct = parseAsciiDouble(value); pct && *pct > 0.0) {
            out.scrollZoomStep = std::clamp(*pct / 100.0, 0.005, 2.0);
        }
    } else if (name == "showSidebar") {
        out.showSidebar = (value == "true");
    } else if (name == "themeVariant") {
        if (value == "forceLight") {
            out.themeVariant = ThemeVariant::ForceLight;
        } else if (value == "forceDark") {
            out.themeVariant = ThemeVariant::ForceDark;
        } else {
            out.themeVariant = ThemeVariant::UseSystem;
        }
    } else if (name == "autosaveEnabled") {
        out.autosaveEnabled = (value == "true");
    } else if (name == "autosaveTimeout") {
        if (const auto minutes = parseAsciiInt(value)) {
            out.autosaveTimeoutMinutes = static_cast<int>(std::clamp<long long>(*minutes, 0LL, 1440LL));
        }
    } else if (name == "colorPalette") {
        if (!value.empty()) {
            out.colorPalettePath = std::filesystem::path(std::string(value));
        }
    }
}

inline void applyToolAttribute(ImportedSettings& out, const std::vector<std::string>& dataStack, std::string_view name,
                               std::string_view type, std::string_view value) {
    if (dataStack.empty() || dataStack[0] != "tools") {
        return;
    }
    if (dataStack.size() == 1) {
        if (name == "current" && type == "string") {
            if (value == "pen") {
                out.activeTool = Tool::Pen;
            } else if (value == "highlighter") {
                out.activeTool = Tool::Highlighter;
            } else if (value == "eraser") {
                out.activeTool = Tool::Eraser;
            }
        }
        return;
    }
    if (dataStack.size() != 2) {
        return;
    }

    const auto& toolName = dataStack[1];
    if (toolName == "pen") {
        if (name == "color") {
            if (const auto color = parsePackedColor(type, value)) {
                out.penColor = *color;
            }
        } else if (name == "size" && type == "string") {
            if (const auto sz = parseToolSizeName(value)) {
                out.penWidth = behavior::penWidths[static_cast<size_t>(*sz)];
            }
        }
    } else if (toolName == "highlighter") {
        if (name == "color") {
            if (const auto color = parsePackedColor(type, value)) {
                out.highlighterColor = *color;
            }
        } else if (name == "size" && type == "string") {
            if (const auto sz = parseToolSizeName(value)) {
                out.highlighterWidth = behavior::highlighterWidths[static_cast<size_t>(*sz)];
            }
        }
    } else if (toolName == "eraser") {
        if (name == "size" && type == "string") {
            if (const auto sz = parseToolSizeName(value)) {
                out.eraserWidth = behavior::eraserWidths[static_cast<size_t>(*sz)];
            }
        } else if (name == "type" && type == "string") {
            if (value == "deleteStroke") {
                out.eraserMode = EraserMode::DeleteStroke;
            } else if (value == "whiteout") {
                out.eraserMode = EraserMode::Whiteout;
            } else {
                out.eraserMode = EraserMode::Standard;
            }
        }
    }
}

struct MarkupContextDeleter {
    void operator()(GMarkupParseContext* ctx) const {
        if (ctx) {
            g_markup_parse_context_free(ctx);
        }
    }
};

}  // namespace settings_detail

// Strictly read-only, non-network XML parser for Xournal++ settings.xml.
// Eliminates three legacy Settings.cpp / ToolHandler.cpp bugs:
//   1. Never writes settings.xml on load or mutates process environment (setenv).
//   2. Null-checks all <data> and <attribute> XML properties instead of crashing.
//   3. Imports per-tool <data name="pen|highlighter|eraser"> blocks even when
//      <attribute name="current"> is omitted or names a non-migrated tool.
inline SettingsParseResult parseSettingsXml(std::string_view xmlContent) {
    SettingsParseResult result;
    if (detail::trimAscii(xmlContent).empty()) {
        result.error = "Settings XML is empty";
        return result;
    }

    settings_detail::XmlState state;
    GMarkupParser parser{};
    parser.start_element = +[](GMarkupParseContext*, const gchar* elementName, const gchar** attrNames,
                               const gchar** attrValues, gpointer userData, GError** error) {
        auto* st = static_cast<settings_detail::XmlState*>(userData);
        const std::string_view tag = elementName ? elementName : "";
        if (st->elementStack.empty()) {
            if (tag != "settings") {
                g_set_error(error, G_MARKUP_ERROR, G_MARKUP_ERROR_UNKNOWN_ELEMENT,
                            "Expected <settings> root element, got <%s>", elementName ? elementName : "");
                return;
            }
            st->sawSettingsRoot = true;
            st->elementStack.emplace_back(tag);
            return;
        }

        if (st->elementStack.size() == 1 && tag == "property") {
            const char* propName = settings_detail::findAttr(attrNames, attrValues, "name");
            const char* propValue = settings_detail::findAttr(attrNames, attrValues, "value");
            if (propName && propValue) {
                settings_detail::applyProperty(st->candidate, propName, propValue);
            }
        } else if (tag == "data") {
            const char* dataName = settings_detail::findAttr(attrNames, attrValues, "name");
            st->dataStack.emplace_back(dataName ? dataName : "");
        } else if (tag == "attribute" && !st->dataStack.empty()) {
            const char* attrName = settings_detail::findAttr(attrNames, attrValues, "name");
            const char* attrType = settings_detail::findAttr(attrNames, attrValues, "type");
            const char* attrValue = settings_detail::findAttr(attrNames, attrValues, "value");
            if (attrName && attrType && attrValue) {
                settings_detail::applyToolAttribute(st->candidate, st->dataStack, attrName, attrType, attrValue);
            }
        }
        st->elementStack.emplace_back(tag);
    };

    parser.end_element = +[](GMarkupParseContext*, const gchar* elementName, gpointer userData, GError**) {
        auto* st = static_cast<settings_detail::XmlState*>(userData);
        const std::string_view tag = elementName ? elementName : "";
        if (tag == "data" && !st->dataStack.empty()) {
            st->dataStack.pop_back();
        }
        if (!st->elementStack.empty()) {
            st->elementStack.pop_back();
        }
    };

    std::unique_ptr<GMarkupParseContext, settings_detail::MarkupContextDeleter> context(
            g_markup_parse_context_new(&parser, GMarkupParseFlags(0), &state, nullptr));
    GError* error = nullptr;
    if (!g_markup_parse_context_parse(context.get(), xmlContent.data(), static_cast<gssize>(xmlContent.size()),
                                      &error) ||
        !g_markup_parse_context_end_parse(context.get(), &error) || !state.sawSettingsRoot) {
        result.error = error ? error->message : "Missing <settings> root element";
        g_clear_error(&error);
        return result;
    }

    result.ok = true;
    result.settings = std::move(state.candidate);
    return result;
}

inline SettingsImportResult importSettingsFromConfigDir(const std::filesystem::path& configDir) {
    SettingsImportResult out;
    out.palette.swatches = defaultPaletteSwatches();
    if (configDir.empty()) {
        return out;
    }

    const auto settingsPath = configDir / "settings.xml";
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(settingsPath, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found) {
        return out;
    }
    out.foundSettingsFile = true;
    if (status.type() == std::filesystem::file_type::symlink) {
        out.usedSettingsFallback = true;
        out.warning = "Refusing symlink settings.xml: " + settingsPath.string();
        return out;
    }
    if (!std::filesystem::is_regular_file(status)) {
        out.usedSettingsFallback = true;
        out.warning = "settings.xml is not a regular file: " + settingsPath.string();
        return out;
    }
    const auto bytes = std::filesystem::file_size(settingsPath, ec);
    if (ec || bytes > settings_detail::maxSettingsBytes) {
        out.usedSettingsFallback = true;
        out.warning = "settings.xml exceeds size limit or cannot be stat'd: " + settingsPath.string();
        return out;
    }

    std::ifstream in(settingsPath, std::ios::binary);
    if (!in) {
        out.usedSettingsFallback = true;
        out.warning = "Cannot open settings.xml: " + settingsPath.string();
        return out;
    }
    std::string content(static_cast<std::size_t>(bytes), '\0');
    if (bytes > 0 && !in.read(content.data(), static_cast<std::streamsize>(bytes))) {
        out.usedSettingsFallback = true;
        out.warning = "Failed reading settings.xml: " + settingsPath.string();
        return out;
    }

    auto parsed = parseSettingsXml(content);
    if (!parsed.ok) {
        out.usedSettingsFallback = true;
        out.warning = settingsPath.string() + ": " + parsed.error;
        return out;
    }

    out.settings = std::move(parsed.settings);
    out.palette = resolvePalette(out.settings.colorPalettePath, configDir);
    if (!out.palette.warning.empty()) {
        out.warning = out.palette.warning;
    }
    return out;
}

inline void applyImportedSettingsToCanvas(CanvasState& state, const ImportedSettings& settings) {
    state.pressureSensitivity = settings.pressureSensitivity;
    state.minimumPressure = settings.minimumPressure;
    state.pressureMultiplier = settings.pressureMultiplier;
    state.zoomStep = settings.zoomStep;
    state.scrollZoomStep = settings.scrollZoomStep;
    state.eraserMode = settings.eraserMode;
    state.eraserWidth = settings.eraserWidth;
}

}  // namespace prototype
