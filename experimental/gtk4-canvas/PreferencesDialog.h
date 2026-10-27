#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <adwaita.h>

#include "MainToolRail.h"
#include "SettingsImport.h"

namespace prototype::gtk4 {

enum class PreferenceKey : guint {
    Theme = 1,
    Sidebar,
    FullscreenMenubar,
    FullscreenToolbar,
    FullscreenSidebar,
    PresentationGoFullscreen,
    PresentationMenubar,
    PresentationToolbar,
    PresentationSidebar,
    LayoutFlow,
    LayoutGridDimension,
    LayoutGridCount,
    LayoutPairedPages,
    LayoutPairedPageOffset,
    LayoutHorizontalDirection,
    LayoutVerticalDirection,
    MainToolRail,
    PressureSensitivity,
    MinimumPressure,
    PressureMultiplier,
    StylusButtonOne,
    StylusButtonTwo,
    AutosaveEnabled,
    AutosaveTimeout,
    ZoomStep,
    ScrollZoomStep,
    PreloadPagesBefore,
    PreloadPagesAfter,
    FullDetailCachePages,
    MediumDetailCachePages,
    MediumDetailCacheMegabytes,
    LowDetailCachePages,
    LowDetailCacheMegabytes,
    OverviewZoomThreshold,
    LowDetailZoomThreshold,
    PreloadDetail,
    EdgePanSpeed,
    EdgePanMaxMultiplier,
    RotationSnapTolerance,
    SnapRotation,
    SnapGrid,
    SnapGridSize,
    SnapGridTolerance,
    FloatingToolboxRow1,
    FloatingToolboxRow2,
    FloatingToolboxRow3,
    FloatingToolboxRow4,
    PositionHighlight,
    CursorHighlightColor,
    CursorHighlightRadius,
    CursorHighlightBorderColor,
    CursorHighlightBorderWidth,
    LatexAutoCheckDependencies,
    LatexDefaultText,
    LatexTemplatePath,
    LatexGeneratorCommand,
    LatexSourceViewTheme,
    LatexSourceViewAutoIndent,
    LatexSourceViewSyntaxHighlight,
    LatexSourceViewShowLineNumbers,
    LatexEditorFont,
    LatexUseCustomEditorFont,
    LatexEditorWordWrap,
    LatexUseExternalEditor,
    LatexExternalEditorAutoConfirm,
    LatexExternalEditorCommand,
    LatexTemporaryFileExtension,
    Count,
};

class PreferencesDialog {
public:
    struct Callbacks {
        std::function<void(PreferenceKey, GtkWidget*)> changePreference;
        std::function<void()> chooseLatexTemplate;
        std::function<void()> applyFloatingToolboxRows;
        std::function<void()> applyMainToolRail;
        std::function<bool()> applyLatexPreferences;
        std::function<void()> testLatexSetup;
    };

    PreferencesDialog() = default;
    PreferencesDialog(const PreferencesDialog&) = delete;
    PreferencesDialog& operator=(const PreferencesDialog&) = delete;
    ~PreferencesDialog();

    void open(GtkWindow* parent, const prototype::ImportedSettings& settings, Callbacks callbacks);
    void clear();

    AdwPreferencesDialog* widget() const { return dialog_; }
    GtkWidget* control(PreferenceKey key) const;
    GtkWidget* floatingToolboxApplyButton() const { return floatingToolboxApplyButton_; }
    GtkWidget* mainToolRailApplyButton() const { return mainToolRailApplyButton_; }
    GtkWidget* mainToolRailVisibleList() const { return mainToolRailVisibleList_; }
    GtkWidget* mainToolRailAvailableList() const { return mainToolRailAvailableList_; }
    GtkWidget* latexSetupTestButton() const { return latexSetupTestButton_; }
    GtkWidget* latexSettingsApplyButton() const { return latexSettingsApplyButton_; }
    GtkWidget* latexSettingsApplyStatus() const { return latexSettingsApplyStatus_; }
    GtkWidget* latexTemplatePathEntry() const { return latexTemplatePathEntry_; }
    bool syncingControls() const { return syncingControls_; }

    const std::vector<std::string>& latexSourceViewThemeIds() const { return latexSourceViewThemeIds_; }
    void setPendingMainToolRail(std::vector<prototype::MainToolAction> actions);
    bool applyMainToolRailDropForTest(std::string_view actionToken, bool toRail, std::size_t targetIndex) {
        return applyMainToolRailDrop(actionToken, toRail, targetIndex);
    }

    void markLatexTextPreferencesPending();
    void setLatexSettingsSaved();
    void setLatexSetupRunning(bool running);
    void setLatexSetupResult(bool success);
    void updateLatexPreferenceSensitivity(const prototype::ImportedSettings& settings);
    void setPositionHighlight(bool enabled);

    bool emitMainToolRailDropForTest(GtkWidget* list, std::string_view actionToken, double y);

private:
    GtkWidget* preferenceCombo(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                               const char* const* options, guint selected, PreferenceKey key);
    GtkWidget* preferenceSwitch(AdwPreferencesGroup* group, const char* title, const char* subtitle, bool active,
                                PreferenceKey key);
    GtkWidget* preferenceSpin(AdwPreferencesGroup* group, const char* title, const char* subtitle, double value,
                              double minimum, double maximum, double step, guint digits, const char* suffix,
                              PreferenceKey key);
    GtkWidget* preferenceColor(AdwPreferencesGroup* group, const char* title, const char* subtitle, std::uint32_t color,
                               PreferenceKey key);
    GtkWidget* preferenceTextEntry(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                                   const std::string& value, PreferenceKey key);
    GtkWidget* preferenceTemplatePath(AdwPreferencesGroup* group, const std::string& value);
    GtkWidget* preferenceFont(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                              const std::string& value);
    GtkWidget* preferenceLatexTheme(AdwPreferencesGroup* group, const std::string& selectedTheme);

    GtkWidget* createMainToolRailEditorRow(prototype::MainToolAction action);
    GtkWidget* createMainToolRailEditorList(bool visible);
    void rebuildMainToolRailEditor();
    bool applyMainToolRailDrop(std::string_view actionToken, bool toRail, std::size_t targetIndex);

    void setLatexSettingsStatus(const char* text, const char* styleClass);

    AdwPreferencesDialog* dialog_ = nullptr;
    GtkWidget* latexTemplatePathEntry_ = nullptr;
    GtkWidget* latexSetupTestButton_ = nullptr;
    GtkWidget* latexSetupTestStatus_ = nullptr;
    GtkWidget* latexSettingsApplyButton_ = nullptr;
    GtkWidget* latexSettingsApplyStatus_ = nullptr;
    GtkWidget* floatingToolboxApplyButton_ = nullptr;
    GtkWidget* mainToolRailApplyButton_ = nullptr;
    GtkWidget* mainToolRailVisibleList_ = nullptr;
    GtkWidget* mainToolRailAvailableList_ = nullptr;
    std::vector<std::string> latexSourceViewThemeIds_;
    std::array<GtkWidget*, static_cast<size_t>(PreferenceKey::Count)> controls_{};
    std::vector<prototype::MainToolAction> pendingMainToolRail_;
    Callbacks callbacks_;
    bool syncingControls_ = false;
};

}  // namespace prototype::gtk4
