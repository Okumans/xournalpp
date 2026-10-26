#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include <gio/gio.h>

#include "SettingsImport.h"

namespace prototype {

struct PreferencesStoreResult {
    bool found = false;
    std::string warning;
};

namespace preferences_detail {

inline constexpr std::size_t maxPreferencesBytes = 1024 * 1024;

struct KeyFileDeleter {
    void operator()(GKeyFile* keyFile) const {
        if (keyFile) {
            g_key_file_unref(keyFile);
        }
    }
};

inline void readBool(GKeyFile* keyFile, const char* group, const char* name, bool& value) {
    if (!g_key_file_has_key(keyFile, group, name, nullptr)) {
        return;
    }
    GError* error = nullptr;
    const auto candidate = g_key_file_get_boolean(keyFile, group, name, &error);
    if (!error) {
        value = candidate;
    }
    g_clear_error(&error);
}

inline void readInt(GKeyFile* keyFile, const char* group, const char* name, int& value, int minimum, int maximum) {
    if (!g_key_file_has_key(keyFile, group, name, nullptr)) {
        return;
    }
    GError* error = nullptr;
    const auto candidate = g_key_file_get_integer(keyFile, group, name, &error);
    if (!error && candidate >= minimum && candidate <= maximum) {
        value = candidate;
    }
    g_clear_error(&error);
}

inline void readPackedColor(GKeyFile* keyFile, const char* name, std::uint32_t& value) {
    if (!g_key_file_has_key(keyFile, "Pointer Highlight", name, nullptr)) {
        return;
    }
    GError* error = nullptr;
    const guint64 candidate = g_key_file_get_uint64(keyFile, "Pointer Highlight", name, &error);
    if (!error && candidate <= std::numeric_limits<std::uint32_t>::max()) {
        value = static_cast<std::uint32_t>(candidate);
    }
    g_clear_error(&error);
}

inline void readDouble(GKeyFile* keyFile, const char* group, const char* name, double& value, double minimum,
                       double maximum) {
    if (!g_key_file_has_key(keyFile, group, name, nullptr)) {
        return;
    }
    GError* error = nullptr;
    const auto candidate = g_key_file_get_double(keyFile, group, name, &error);
    if (!error && std::isfinite(candidate) && candidate >= minimum && candidate <= maximum) {
        value = candidate;
    }
    g_clear_error(&error);
}

inline void readString(GKeyFile* keyFile, const char* group, const char* name, std::string& value,
                       std::size_t maximumBytes) {
    if (!g_key_file_has_key(keyFile, group, name, nullptr)) {
        return;
    }
    GError* error = nullptr;
    gchar* candidate = g_key_file_get_string(keyFile, group, name, &error);
    if (!error && candidate && std::strlen(candidate) <= maximumBytes && g_utf8_validate(candidate, -1, nullptr)) {
        value = candidate;
    }
    g_clear_error(&error);
    g_free(candidate);
}

inline std::string pathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

inline std::filesystem::path pathFromUtf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character: value) {
        encoded.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(encoded);
}

inline ButtonToolAction readButtonAction(GKeyFile* keyFile, const char* name, ButtonToolAction fallback) {
    int value = static_cast<int>(fallback);
    readInt(keyFile, "Stylus", name, value, static_cast<int>(ButtonToolAction::None),
            static_cast<int>(ButtonToolAction::FloatingToolbox));
    return static_cast<ButtonToolAction>(value);
}

inline void readFloatingToolboxRows(GKeyFile* keyFile, FloatingToolboxRows& rows) {
    static constexpr std::array<const char*, 4> keys = {"row-1", "row-2", "row-3", "row-4"};
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (!g_key_file_has_key(keyFile, "Floating Toolbox", keys[index], nullptr)) {
            continue;
        }
        GError* error = nullptr;
        gchar* value = g_key_file_get_string(keyFile, "Floating Toolbox", keys[index], &error);
        if (error) {
            g_clear_error(&error);
            continue;
        }
        const auto parsed = parseFloatingToolboxRow(value ? value : "");
        if (parsed) {
            rows[index] = *parsed;
        }
        g_free(value);
    }
}

inline const char* themeName(ThemeVariant theme) {
    switch (theme) {
        case ThemeVariant::ForceLight:
            return "light";
        case ThemeVariant::ForceDark:
            return "dark";
        case ThemeVariant::UseSystem:
        default:
            return "system";
    }
}

inline ThemeVariant parseTheme(const char* theme, ThemeVariant fallback) {
    if (!theme) {
        return fallback;
    }
    if (g_str_equal(theme, "light")) {
        return ThemeVariant::ForceLight;
    }
    if (g_str_equal(theme, "dark")) {
        return ThemeVariant::ForceDark;
    }
    if (g_str_equal(theme, "system")) {
        return ThemeVariant::UseSystem;
    }
    return fallback;
}

}  // namespace preferences_detail

inline PreferencesStoreResult loadGtk4Preferences(const std::filesystem::path& configDir,
                                                  ImportedSettings& settings) {
    PreferencesStoreResult result;
    if (configDir.empty()) {
        return result;
    }

    const auto path = configDir / "gtk4-canvas.ini";
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if ((!ec && status.type() == std::filesystem::file_type::not_found) ||
        ec == std::errc::no_such_file_or_directory) {
        return result;
    }
    result.found = true;
    if (ec || status.type() == std::filesystem::file_type::symlink || !std::filesystem::is_regular_file(status)) {
        result.warning = "Refusing non-regular GTK 4 preferences file: " + path.string();
        return result;
    }
    const auto length = std::filesystem::file_size(path, ec);
    if (ec || length > preferences_detail::maxPreferencesBytes) {
        result.warning = "GTK 4 preferences file exceeds its size limit: " + path.string();
        return result;
    }

    std::unique_ptr<GKeyFile, preferences_detail::KeyFileDeleter> keyFile(g_key_file_new());
    GError* error = nullptr;
    if (!g_key_file_load_from_file(keyFile.get(), path.c_str(), G_KEY_FILE_NONE, &error)) {
        result.warning = error ? error->message : "Cannot read GTK 4 preferences";
        g_clear_error(&error);
        return result;
    }

    auto& detail = preferences_detail::readBool;
    std::string penLineStyle;
    preferences_detail::readString(keyFile.get(), "Pen", "line-style", penLineStyle, 512);
    if (const auto lineDashes = parseLineStyle(penLineStyle)) {
        settings.penLineStyle = formatLineStyle(*lineDashes);
    }
    detail(keyFile.get(), "Tool Fill", "pen-enabled", settings.penFillEnabled);
    preferences_detail::readInt(keyFile.get(), "Tool Fill", "pen-opacity", settings.penFillOpacity, 0, 255);
    detail(keyFile.get(), "Tool Fill", "highlighter-enabled", settings.highlighterFillEnabled);
    preferences_detail::readInt(keyFile.get(), "Tool Fill", "highlighter-opacity", settings.highlighterFillOpacity,
                                0, 255);
    detail(keyFile.get(), "General", "show-sidebar", settings.showSidebar);
    auto& defaultView = settings.viewModes[static_cast<std::size_t>(ViewModePreset::Default)];
    auto& fullscreenView = settings.viewModes[static_cast<std::size_t>(ViewModePreset::Fullscreen)];
    auto& presentationView = settings.viewModes[static_cast<std::size_t>(ViewModePreset::Presentation)];
    defaultView.showSidebar = settings.showSidebar;
    detail(keyFile.get(), "View Modes", "default-go-fullscreen", defaultView.goFullscreen);
    detail(keyFile.get(), "View Modes", "default-show-menubar", defaultView.showMenubar);
    detail(keyFile.get(), "View Modes", "default-show-toolbar", defaultView.showToolbar);
    detail(keyFile.get(), "View Modes", "default-show-sidebar", defaultView.showSidebar);
    detail(keyFile.get(), "View Modes", "fullscreen-go-fullscreen", fullscreenView.goFullscreen);
    detail(keyFile.get(), "View Modes", "fullscreen-show-menubar", fullscreenView.showMenubar);
    detail(keyFile.get(), "View Modes", "fullscreen-show-toolbar", fullscreenView.showToolbar);
    detail(keyFile.get(), "View Modes", "fullscreen-show-sidebar", fullscreenView.showSidebar);
    detail(keyFile.get(), "View Modes", "presentation-go-fullscreen", presentationView.goFullscreen);
    detail(keyFile.get(), "View Modes", "presentation-show-menubar", presentationView.showMenubar);
    detail(keyFile.get(), "View Modes", "presentation-show-toolbar", presentationView.showToolbar);
    detail(keyFile.get(), "View Modes", "presentation-show-sidebar", presentationView.showSidebar);
    int layoutColumns = static_cast<int>(settings.pageLayout.columns);
    int layoutRows = static_cast<int>(settings.pageLayout.rows);
    preferences_detail::readInt(keyFile.get(), "Layout", "columns", layoutColumns, 1, 4096);
    preferences_detail::readInt(keyFile.get(), "Layout", "rows", layoutRows, 1, 4096);
    settings.pageLayout.columns = static_cast<std::size_t>(layoutColumns);
    settings.pageLayout.rows = static_cast<std::size_t>(layoutRows);
    detail(keyFile.get(), "Layout", "fixed-rows", settings.pageLayout.fixedRows);
    detail(keyFile.get(), "Layout", "paired-pages", settings.pageLayout.pairedPages);
    preferences_detail::readInt(keyFile.get(), "Layout", "paired-page-offset", settings.pageLayout.pairedPageOffset,
                                0, 100);
    detail(keyFile.get(), "Layout", "vertical", settings.pageLayout.vertical);
    detail(keyFile.get(), "Layout", "right-to-left", settings.pageLayout.rightToLeft);
    detail(keyFile.get(), "Layout", "bottom-to-top", settings.pageLayout.bottomToTop);
    detail(keyFile.get(), "Layout", "presentation-mode", settings.pageLayout.presentationMode);
    detail(keyFile.get(), "Input", "pressure-sensitivity", settings.pressureSensitivity);
    detail(keyFile.get(), "Pointer Highlight", "enabled", settings.highlightPosition);
    preferences_detail::readPackedColor(keyFile.get(), "fill-color", settings.cursorHighlightColor);
    preferences_detail::readPackedColor(keyFile.get(), "border-color", settings.cursorHighlightBorderColor);
    preferences_detail::readDouble(keyFile.get(), "Pointer Highlight", "radius", settings.cursorHighlightRadius, 0.0,
                                   30.0);
    preferences_detail::readDouble(keyFile.get(), "Pointer Highlight", "border-width",
                                   settings.cursorHighlightBorderWidth, 0.0, 30.0);
    detail(keyFile.get(), "Editing", "autosave-enabled", settings.autosaveEnabled);
    preferences_detail::readInt(keyFile.get(), "Editing", "autosave-timeout-minutes", settings.autosaveTimeoutMinutes,
                                0, 1440);
    preferences_detail::readDouble(keyFile.get(), "Input", "minimum-pressure", settings.minimumPressure, 0.01, 1.0);
    preferences_detail::readDouble(keyFile.get(), "Input", "pressure-multiplier", settings.pressureMultiplier, 0.05,
                                   10.0);
    preferences_detail::readDouble(keyFile.get(), "Navigation", "zoom-step", settings.zoomStep, 0.005, 2.0);
    preferences_detail::readDouble(keyFile.get(), "Navigation", "scroll-zoom-step", settings.scrollZoomStep, 0.005,
                                   2.0);
    int preloadPagesBefore = static_cast<int>(settings.preloadPagesBefore);
    int preloadPagesAfter = static_cast<int>(settings.preloadPagesAfter);
    preferences_detail::readInt(keyFile.get(), "Navigation", "preload-pages-before", preloadPagesBefore, 0, 99);
    preferences_detail::readInt(keyFile.get(), "Navigation", "preload-pages-after", preloadPagesAfter, 0, 99);
    settings.preloadPagesBefore = static_cast<unsigned int>(preloadPagesBefore);
    settings.preloadPagesAfter = static_cast<unsigned int>(preloadPagesAfter);
    preferences_detail::readInt(keyFile.get(), "Rendering", "full-detail-cache-pages",
                                settings.fullDetailCachePages, 1, 8);
    preferences_detail::readInt(keyFile.get(), "Rendering", "medium-detail-cache-pages",
                                settings.mediumDetailCachePages, 1, 64);
    preferences_detail::readInt(keyFile.get(), "Rendering", "medium-detail-cache-megabytes",
                                settings.mediumDetailCacheMegabytes, 1, 512);
    preferences_detail::readInt(keyFile.get(), "Rendering", "low-detail-cache-pages",
                                settings.lowDetailCachePages, 1, 512);
    preferences_detail::readInt(keyFile.get(), "Rendering", "low-detail-cache-megabytes",
                                settings.lowDetailCacheMegabytes, 1, 512);
    preferences_detail::readInt(keyFile.get(), "Rendering", "overview-zoom-threshold-percent",
                                settings.overviewZoomThresholdPercent, 5, 100);
    preferences_detail::readInt(keyFile.get(), "Rendering", "low-detail-zoom-threshold-percent",
                                settings.lowDetailZoomThresholdPercent, 5, 50);
    int preloadDetail = static_cast<int>(settings.preloadDetail);
    preferences_detail::readInt(keyFile.get(), "Rendering", "preload-detail", preloadDetail, 0, 2);
    settings.preloadDetail = static_cast<CanvasPreloadDetail>(preloadDetail);
    preferences_detail::readDouble(keyFile.get(), "Navigation", "edge-pan-speed", settings.edgePanSpeed, 0.0, 100.0);
    preferences_detail::readDouble(keyFile.get(), "Navigation", "edge-pan-max-multiplier",
                                   settings.edgePanMaxMultiplier, 0.1, 10.0);
    preferences_detail::readDouble(keyFile.get(), "Selection", "rotation-snap-tolerance",
                                   settings.snapRotationTolerance, 0.0, 1.0);
    detail(keyFile.get(), "Selection", "snap-rotation", settings.snapRotation);
    detail(keyFile.get(), "Selection", "snap-grid", settings.snapGrid);
    preferences_detail::readDouble(keyFile.get(), "Selection", "snap-grid-size", settings.snapGridSize, 0.1,
                                   1000.0);
    preferences_detail::readDouble(keyFile.get(), "Selection", "snap-grid-tolerance", settings.snapGridTolerance,
                                   0.0, 1.0);
    preferences_detail::readInt(keyFile.get(), "Text", "alignment", settings.textAlignment, 0, 2);
    detail(keyFile.get(), "Text", "justify", settings.textJustify);
    if (g_key_file_has_key(keyFile.get(), "Appearance", "theme", nullptr)) {
        gchar* value = g_key_file_get_string(keyFile.get(), "Appearance", "theme", nullptr);
        settings.themeVariant = preferences_detail::parseTheme(value, settings.themeVariant);
        g_free(value);
    }
    settings.stylusButtonOne = preferences_detail::readButtonAction(keyFile.get(), "button-one", settings.stylusButtonOne);
    settings.stylusButtonTwo = preferences_detail::readButtonAction(keyFile.get(), "button-two", settings.stylusButtonTwo);
    const auto readPluginPaths = [&](const char* key, std::string& destination) {
        if (!g_key_file_has_key(keyFile.get(), "Plugins", key, nullptr)) {
            return;
        }
        GError* error = nullptr;
        gchar* value = g_key_file_get_string(keyFile.get(), "Plugins", key, &error);
        if (!error && value) {
            destination = value;
        }
        g_clear_error(&error);
        g_free(value);
    };
    readPluginPaths("enabled", settings.pluginsEnabled);
    readPluginPaths("disabled", settings.pluginsDisabled);
    preferences_detail::readFloatingToolboxRows(keyFile.get(), settings.floatingToolboxRows);
    if (g_key_file_has_key(keyFile.get(), "Toolbar", "main-tool-rail", nullptr)) {
        GError* railError = nullptr;
        gchar* value = g_key_file_get_string(keyFile.get(), "Toolbar", "main-tool-rail", &railError);
        if (!railError && value) {
            const auto parsed = parseMainToolRail(value);
            if (parsed) {
                settings.mainToolRail = *parsed;
            }
        }
        g_clear_error(&railError);
        g_free(value);
    }
    detail(keyFile.get(), "LaTeX", "auto-check-dependencies", settings.latexAutoCheckDependencies);
    preferences_detail::readString(keyFile.get(), "LaTeX", "default-text", settings.latexDefaultText,
                                   maxCanvasLatexSourceBytes);
    std::string latexTemplatePath = preferences_detail::pathToUtf8(settings.latexTemplatePath);
    preferences_detail::readString(keyFile.get(), "LaTeX", "template-path", latexTemplatePath, 4096);
    settings.latexTemplatePath = preferences_detail::pathFromUtf8(latexTemplatePath);
    preferences_detail::readString(keyFile.get(), "LaTeX", "generator-command", settings.latexGeneratorCommand,
                                   16384);
    preferences_detail::readString(keyFile.get(), "LaTeX", "source-view-theme", settings.latexSourceViewThemeId,
                                   128);
    detail(keyFile.get(), "LaTeX", "source-view-auto-indent", settings.latexSourceViewAutoIndent);
    detail(keyFile.get(), "LaTeX", "source-view-syntax-highlight", settings.latexSourceViewSyntaxHighlight);
    detail(keyFile.get(), "LaTeX", "source-view-line-numbers", settings.latexSourceViewShowLineNumbers);
    preferences_detail::readString(keyFile.get(), "LaTeX", "editor-font", settings.latexEditorFont, 256);
    detail(keyFile.get(), "LaTeX", "use-custom-editor-font", settings.latexUseCustomEditorFont);
    detail(keyFile.get(), "LaTeX", "editor-word-wrap", settings.latexEditorWordWrap);
    detail(keyFile.get(), "LaTeX", "use-external-editor", settings.latexUseExternalEditor);
    detail(keyFile.get(), "LaTeX", "external-editor-auto-confirm", settings.latexExternalEditorAutoConfirm);
    preferences_detail::readString(keyFile.get(), "LaTeX", "external-editor-command",
                                   settings.latexExternalEditorCommand, 16384);
    preferences_detail::readString(keyFile.get(), "LaTeX", "temporary-file-extension",
                                   settings.latexTemporaryFileExtension, 64);
    return result;
}

inline bool saveGtk4Preferences(const std::filesystem::path& configDir, const ImportedSettings& settings,
                                std::string& errorMessage) {
    if (settings.latexDefaultText.size() > maxCanvasLatexSourceBytes || settings.penLineStyle.size() > 512 ||
        !parseLineStyle(settings.penLineStyle) ||
        settings.penFillOpacity < 0 || settings.penFillOpacity > 255 || settings.highlighterFillOpacity < 0 ||
        settings.highlighterFillOpacity > 255 ||
        preferences_detail::pathToUtf8(settings.latexTemplatePath).size() > 4096 ||
        settings.latexGeneratorCommand.size() > 16384 || settings.latexSourceViewThemeId.size() > 128 ||
        settings.latexEditorFont.size() > 256 || settings.latexExternalEditorCommand.size() > 16384 ||
        settings.latexTemporaryFileExtension.size() > 64 || settings.pageLayout.columns < 1 ||
        settings.pageLayout.columns > 4096 || settings.pageLayout.rows < 1 || settings.pageLayout.rows > 4096 ||
        settings.pageLayout.pairedPageOffset < 0 || settings.pageLayout.pairedPageOffset > 100) {
        errorMessage = "A GTK 4 preference is invalid or exceeds its size limit";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (ec) {
        errorMessage = "Cannot create configuration directory: " + ec.message();
        return false;
    }

    const auto path = configDir / "gtk4-canvas.ini";
    const auto status = std::filesystem::symlink_status(path, ec);
    if (!ec && status.type() != std::filesystem::file_type::not_found &&
        (status.type() == std::filesystem::file_type::symlink || !std::filesystem::is_regular_file(status))) {
        errorMessage = "Refusing to replace non-regular GTK 4 preferences file: " + path.string();
        return false;
    }
    if (ec == std::errc::no_such_file_or_directory) {
        ec.clear();
    }
    if (ec) {
        errorMessage = "Cannot inspect GTK 4 preferences file: " + ec.message();
        return false;
    }

    std::unique_ptr<GKeyFile, preferences_detail::KeyFileDeleter> keyFile(g_key_file_new());
    g_key_file_set_string(keyFile.get(), "Pen", "line-style", settings.penLineStyle.c_str());
    g_key_file_set_boolean(keyFile.get(), "Tool Fill", "pen-enabled", settings.penFillEnabled);
    g_key_file_set_integer(keyFile.get(), "Tool Fill", "pen-opacity", settings.penFillOpacity);
    g_key_file_set_boolean(keyFile.get(), "Tool Fill", "highlighter-enabled", settings.highlighterFillEnabled);
    g_key_file_set_integer(keyFile.get(), "Tool Fill", "highlighter-opacity", settings.highlighterFillOpacity);
    g_key_file_set_string(keyFile.get(), "Appearance", "theme", preferences_detail::themeName(settings.themeVariant));
    g_key_file_set_boolean(keyFile.get(), "General", "show-sidebar", settings.showSidebar);
    const auto& defaultView = settings.viewModes[static_cast<std::size_t>(ViewModePreset::Default)];
    const auto& fullscreenView = settings.viewModes[static_cast<std::size_t>(ViewModePreset::Fullscreen)];
    const auto& presentationView = settings.viewModes[static_cast<std::size_t>(ViewModePreset::Presentation)];
    g_key_file_set_boolean(keyFile.get(), "View Modes", "default-go-fullscreen", defaultView.goFullscreen);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "default-show-menubar", defaultView.showMenubar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "default-show-toolbar", defaultView.showToolbar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "default-show-sidebar", defaultView.showSidebar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "fullscreen-go-fullscreen", fullscreenView.goFullscreen);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "fullscreen-show-menubar", fullscreenView.showMenubar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "fullscreen-show-toolbar", fullscreenView.showToolbar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "fullscreen-show-sidebar", fullscreenView.showSidebar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "presentation-go-fullscreen", presentationView.goFullscreen);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "presentation-show-menubar", presentationView.showMenubar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "presentation-show-toolbar", presentationView.showToolbar);
    g_key_file_set_boolean(keyFile.get(), "View Modes", "presentation-show-sidebar", presentationView.showSidebar);
    g_key_file_set_integer(keyFile.get(), "Layout", "columns", static_cast<int>(settings.pageLayout.columns));
    g_key_file_set_integer(keyFile.get(), "Layout", "rows", static_cast<int>(settings.pageLayout.rows));
    g_key_file_set_boolean(keyFile.get(), "Layout", "fixed-rows", settings.pageLayout.fixedRows);
    g_key_file_set_boolean(keyFile.get(), "Layout", "paired-pages", settings.pageLayout.pairedPages);
    g_key_file_set_integer(keyFile.get(), "Layout", "paired-page-offset", settings.pageLayout.pairedPageOffset);
    g_key_file_set_boolean(keyFile.get(), "Layout", "vertical", settings.pageLayout.vertical);
    g_key_file_set_boolean(keyFile.get(), "Layout", "right-to-left", settings.pageLayout.rightToLeft);
    g_key_file_set_boolean(keyFile.get(), "Layout", "bottom-to-top", settings.pageLayout.bottomToTop);
    g_key_file_set_boolean(keyFile.get(), "Layout", "presentation-mode", settings.pageLayout.presentationMode);
    g_key_file_set_boolean(keyFile.get(), "Input", "pressure-sensitivity", settings.pressureSensitivity);
    g_key_file_set_boolean(keyFile.get(), "Pointer Highlight", "enabled", settings.highlightPosition);
    g_key_file_set_uint64(keyFile.get(), "Pointer Highlight", "fill-color", settings.cursorHighlightColor);
    g_key_file_set_double(keyFile.get(), "Pointer Highlight", "radius", settings.cursorHighlightRadius);
    g_key_file_set_uint64(keyFile.get(), "Pointer Highlight", "border-color", settings.cursorHighlightBorderColor);
    g_key_file_set_double(keyFile.get(), "Pointer Highlight", "border-width", settings.cursorHighlightBorderWidth);
    g_key_file_set_double(keyFile.get(), "Input", "minimum-pressure", settings.minimumPressure);
    g_key_file_set_double(keyFile.get(), "Input", "pressure-multiplier", settings.pressureMultiplier);
    g_key_file_set_double(keyFile.get(), "Navigation", "zoom-step", settings.zoomStep);
    g_key_file_set_double(keyFile.get(), "Navigation", "scroll-zoom-step", settings.scrollZoomStep);
    g_key_file_set_integer(keyFile.get(), "Navigation", "preload-pages-before",
                           static_cast<int>(settings.preloadPagesBefore));
    g_key_file_set_integer(keyFile.get(), "Navigation", "preload-pages-after",
                           static_cast<int>(settings.preloadPagesAfter));
    g_key_file_set_integer(keyFile.get(), "Rendering", "full-detail-cache-pages", settings.fullDetailCachePages);
    g_key_file_set_integer(keyFile.get(), "Rendering", "medium-detail-cache-pages", settings.mediumDetailCachePages);
    g_key_file_set_integer(keyFile.get(), "Rendering", "medium-detail-cache-megabytes",
                           settings.mediumDetailCacheMegabytes);
    g_key_file_set_integer(keyFile.get(), "Rendering", "low-detail-cache-pages", settings.lowDetailCachePages);
    g_key_file_set_integer(keyFile.get(), "Rendering", "low-detail-cache-megabytes",
                           settings.lowDetailCacheMegabytes);
    g_key_file_set_integer(keyFile.get(), "Rendering", "overview-zoom-threshold-percent",
                           settings.overviewZoomThresholdPercent);
    g_key_file_set_integer(keyFile.get(), "Rendering", "low-detail-zoom-threshold-percent",
                           settings.lowDetailZoomThresholdPercent);
    g_key_file_set_integer(keyFile.get(), "Rendering", "preload-detail", static_cast<int>(settings.preloadDetail));
    g_key_file_set_double(keyFile.get(), "Navigation", "edge-pan-speed", settings.edgePanSpeed);
    g_key_file_set_double(keyFile.get(), "Navigation", "edge-pan-max-multiplier", settings.edgePanMaxMultiplier);
    g_key_file_set_double(keyFile.get(), "Selection", "rotation-snap-tolerance", settings.snapRotationTolerance);
    g_key_file_set_boolean(keyFile.get(), "Selection", "snap-rotation", settings.snapRotation);
    g_key_file_set_boolean(keyFile.get(), "Selection", "snap-grid", settings.snapGrid);
    g_key_file_set_double(keyFile.get(), "Selection", "snap-grid-size", settings.snapGridSize);
    g_key_file_set_double(keyFile.get(), "Selection", "snap-grid-tolerance", settings.snapGridTolerance);
    g_key_file_set_integer(keyFile.get(), "Text", "alignment", settings.textAlignment);
    g_key_file_set_boolean(keyFile.get(), "Text", "justify", settings.textJustify);
    g_key_file_set_boolean(keyFile.get(), "Editing", "autosave-enabled", settings.autosaveEnabled);
    g_key_file_set_integer(keyFile.get(), "Editing", "autosave-timeout-minutes", settings.autosaveTimeoutMinutes);
    g_key_file_set_integer(keyFile.get(), "Stylus", "button-one", static_cast<int>(settings.stylusButtonOne));
    g_key_file_set_integer(keyFile.get(), "Stylus", "button-two", static_cast<int>(settings.stylusButtonTwo));
    g_key_file_set_string(keyFile.get(), "Plugins", "enabled", settings.pluginsEnabled.c_str());
    g_key_file_set_string(keyFile.get(), "Plugins", "disabled", settings.pluginsDisabled.c_str());
    static constexpr std::array<const char*, 4> toolboxKeys = {"row-1", "row-2", "row-3", "row-4"};
    for (std::size_t index = 0; index < toolboxKeys.size(); ++index) {
        const auto value = serializeFloatingToolboxRow(settings.floatingToolboxRows[index]);
        g_key_file_set_string(keyFile.get(), "Floating Toolbox", toolboxKeys[index], value.c_str());
    }
    const auto mainToolRail = serializeMainToolRail(settings.mainToolRail);
    if (mainToolRail.empty()) {
        errorMessage = "Cannot save an invalid GTK 4 main tool rail layout";
        return false;
    }
    g_key_file_set_string(keyFile.get(), "Toolbar", "main-tool-rail", mainToolRail.c_str());
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "auto-check-dependencies", settings.latexAutoCheckDependencies);
    g_key_file_set_string(keyFile.get(), "LaTeX", "default-text", settings.latexDefaultText.c_str());
    const auto latexTemplatePath = preferences_detail::pathToUtf8(settings.latexTemplatePath);
    g_key_file_set_string(keyFile.get(), "LaTeX", "template-path", latexTemplatePath.c_str());
    g_key_file_set_string(keyFile.get(), "LaTeX", "generator-command", settings.latexGeneratorCommand.c_str());
    g_key_file_set_string(keyFile.get(), "LaTeX", "source-view-theme", settings.latexSourceViewThemeId.c_str());
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "source-view-auto-indent", settings.latexSourceViewAutoIndent);
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "source-view-syntax-highlight",
                           settings.latexSourceViewSyntaxHighlight);
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "source-view-line-numbers",
                           settings.latexSourceViewShowLineNumbers);
    g_key_file_set_string(keyFile.get(), "LaTeX", "editor-font", settings.latexEditorFont.c_str());
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "use-custom-editor-font", settings.latexUseCustomEditorFont);
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "editor-word-wrap", settings.latexEditorWordWrap);
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "use-external-editor", settings.latexUseExternalEditor);
    g_key_file_set_boolean(keyFile.get(), "LaTeX", "external-editor-auto-confirm",
                           settings.latexExternalEditorAutoConfirm);
    g_key_file_set_string(keyFile.get(), "LaTeX", "external-editor-command",
                          settings.latexExternalEditorCommand.c_str());
    g_key_file_set_string(keyFile.get(), "LaTeX", "temporary-file-extension",
                          settings.latexTemporaryFileExtension.c_str());
    gsize length = 0;
    gchar* data = g_key_file_to_data(keyFile.get(), &length, nullptr);
    if (!data) {
        errorMessage = "Cannot encode GTK 4 preferences";
        return false;
    }

    GFile* file = g_file_new_for_path(path.c_str());
    gchar* etag = nullptr;
    GError* error = nullptr;
    const bool saved = g_file_replace_contents(file, data, length, nullptr, false,
                                               G_FILE_CREATE_REPLACE_DESTINATION, &etag, nullptr, &error);
    g_free(etag);
    g_object_unref(file);
    g_free(data);
    if (!saved) {
        errorMessage = error ? error->message : "Cannot save GTK 4 preferences";
        g_clear_error(&error);
        return false;
    }
    return true;
}

}  // namespace prototype
