#pragma once

#include <filesystem>
#include <cmath>
#include <memory>
#include <string>

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

inline ButtonToolAction readButtonAction(GKeyFile* keyFile, const char* name, ButtonToolAction fallback) {
    int value = static_cast<int>(fallback);
    readInt(keyFile, "Stylus", name, value, static_cast<int>(ButtonToolAction::None),
            static_cast<int>(ButtonToolAction::Hand));
    return static_cast<ButtonToolAction>(value);
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
    detail(keyFile.get(), "General", "show-sidebar", settings.showSidebar);
    detail(keyFile.get(), "Input", "pressure-sensitivity", settings.pressureSensitivity);
    detail(keyFile.get(), "Editing", "autosave-enabled", settings.autosaveEnabled);
    preferences_detail::readInt(keyFile.get(), "Editing", "autosave-timeout-minutes", settings.autosaveTimeoutMinutes,
                                0, 1440);
    preferences_detail::readDouble(keyFile.get(), "Input", "minimum-pressure", settings.minimumPressure, 0.01, 1.0);
    preferences_detail::readDouble(keyFile.get(), "Input", "pressure-multiplier", settings.pressureMultiplier, 0.05,
                                   10.0);
    preferences_detail::readDouble(keyFile.get(), "Navigation", "zoom-step", settings.zoomStep, 0.005, 2.0);
    preferences_detail::readDouble(keyFile.get(), "Navigation", "scroll-zoom-step", settings.scrollZoomStep, 0.005,
                                   2.0);
    if (g_key_file_has_key(keyFile.get(), "Appearance", "theme", nullptr)) {
        gchar* value = g_key_file_get_string(keyFile.get(), "Appearance", "theme", nullptr);
        settings.themeVariant = preferences_detail::parseTheme(value, settings.themeVariant);
        g_free(value);
    }
    settings.stylusButtonOne = preferences_detail::readButtonAction(keyFile.get(), "button-one", settings.stylusButtonOne);
    settings.stylusButtonTwo = preferences_detail::readButtonAction(keyFile.get(), "button-two", settings.stylusButtonTwo);
    return result;
}

inline bool saveGtk4Preferences(const std::filesystem::path& configDir, const ImportedSettings& settings,
                                std::string& errorMessage) {
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
    g_key_file_set_string(keyFile.get(), "Appearance", "theme", preferences_detail::themeName(settings.themeVariant));
    g_key_file_set_boolean(keyFile.get(), "General", "show-sidebar", settings.showSidebar);
    g_key_file_set_boolean(keyFile.get(), "Input", "pressure-sensitivity", settings.pressureSensitivity);
    g_key_file_set_double(keyFile.get(), "Input", "minimum-pressure", settings.minimumPressure);
    g_key_file_set_double(keyFile.get(), "Input", "pressure-multiplier", settings.pressureMultiplier);
    g_key_file_set_double(keyFile.get(), "Navigation", "zoom-step", settings.zoomStep);
    g_key_file_set_double(keyFile.get(), "Navigation", "scroll-zoom-step", settings.scrollZoomStep);
    g_key_file_set_boolean(keyFile.get(), "Editing", "autosave-enabled", settings.autosaveEnabled);
    g_key_file_set_integer(keyFile.get(), "Editing", "autosave-timeout-minutes", settings.autosaveTimeoutMinutes);
    g_key_file_set_integer(keyFile.get(), "Stylus", "button-one", static_cast<int>(settings.stylusButtonOne));
    g_key_file_set_integer(keyFile.get(), "Stylus", "button-two", static_cast<int>(settings.stylusButtonTwo));
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
