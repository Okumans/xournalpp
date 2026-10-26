#include "Prototype.h"

namespace prototype::gtk4 {

gboolean Prototype::smokePreferencesVerify() {
    const auto control = [&](PreferenceKey key) { return preferencesUi.controls[static_cast<size_t>(key)]; };
    const prototype::FloatingToolboxRows floatingModeTestRows = {{
            {"SMART_SELECT", "PEN", "DRAW", "PAGE_SPIN", "LAYER"},
            {"ZOOM_SLIDER", "DRAW_DOUBLE_ARROW", "DRAW_COORDINATE_SYSTEM", "DRAW_SPLINE", "SHAPE_RECOGNIZER", "RULER"},
            {"TOOL_FILL", "FINE", "DRAW_RECTANGLE", "DRAW_ELLIPSE"},
            {"DRAW_ARROW", "FILL_OPACITY"},
    }};
    if (preferencesVerifyMode == "edit" && ((preferencesVerifyStep >= 4 && !floatingToolboxSmokeReady) ||
                                            (preferencesVerifyStep >= 5 && !floatingToolboxDismissSmokeReady))) {
        return G_SOURCE_CONTINUE;
    }
    if (preferencesVerifyMode == "edit") {
        switch (preferencesVerifyStep++) {
            case 0:
                require(importedConfig.settings.stylusButtonOne == prototype::ButtonToolAction::Highlighter &&
                                importedConfig.settings.stylusButtonTwo == prototype::ButtonToolAction::Hand,
                        "Legacy stylus mappings were not imported before opening Preferences");
                require(!importedConfig.settings.showSidebar && !adw_overlay_split_view_get_show_sidebar(splitView) &&
                                importedConfig.settings.mainToolRail ==
                                        std::vector<prototype::MainToolAction>(prototype::defaultMainToolRail.begin(),
                                                                               prototype::defaultMainToolRail.end()) &&
                                importedConfig.settings.themeVariant == prototype::ThemeVariant::ForceDark &&
                                paletteSwatchButtons.size() == importedConfig.palette.swatches.size(),
                        "Fresh GTK 4 startup did not use the canvas-first tool and color rail defaults");
                capture("main-tool-rail-default");
                openPreferences();
                break;
            case 1: {
                require(control(PreferenceKey::Theme) && control(PreferenceKey::StylusButtonOne) &&
                                control(PreferenceKey::StylusButtonTwo) && control(PreferenceKey::AutosaveTimeout) &&
                                control(PreferenceKey::PositionHighlight) &&
                                control(PreferenceKey::CursorHighlightColor) &&
                                control(PreferenceKey::CursorHighlightRadius) &&
                                control(PreferenceKey::CursorHighlightBorderColor) &&
                                control(PreferenceKey::CursorHighlightBorderWidth) &&
                                control(PreferenceKey::RotationSnapTolerance) && control(PreferenceKey::SnapRotation) &&
                                control(PreferenceKey::SnapGrid) && control(PreferenceKey::SnapGridSize) &&
                                control(PreferenceKey::SnapGridTolerance) && control(PreferenceKey::EdgePanSpeed) &&
                                control(PreferenceKey::EdgePanMaxMultiplier),
                        "Preferences dialog is missing appearance, stylus or autosave controls");
                require(control(PreferenceKey::FloatingToolboxRow1) && control(PreferenceKey::FloatingToolboxRow2) &&
                                control(PreferenceKey::FloatingToolboxRow3) &&
                                control(PreferenceKey::FloatingToolboxRow4) && floatingToolboxApplyButton,
                        "Preferences dialog is missing the four floating toolbox rows");
                require(control(PreferenceKey::LatexAutoCheckDependencies) &&
                                control(PreferenceKey::LatexDefaultText) && control(PreferenceKey::LatexTemplatePath) &&
                                control(PreferenceKey::LatexGeneratorCommand) &&
                                control(PreferenceKey::LatexSourceViewTheme) &&
                                control(PreferenceKey::LatexSourceViewAutoIndent) &&
                                control(PreferenceKey::LatexSourceViewSyntaxHighlight) &&
                                control(PreferenceKey::LatexSourceViewShowLineNumbers) &&
                                control(PreferenceKey::LatexEditorFont) &&
                                control(PreferenceKey::LatexUseCustomEditorFont) &&
                                control(PreferenceKey::LatexEditorWordWrap) &&
                                control(PreferenceKey::LatexUseExternalEditor) &&
                                control(PreferenceKey::LatexExternalEditorAutoConfirm) &&
                                control(PreferenceKey::LatexExternalEditorCommand) &&
                                control(PreferenceKey::LatexTemporaryFileExtension) && preferencesUi.latexSettingsApplyButton &&
                                preferencesUi.latexSetupTestButton,
                        "LaTeX Preferences page is missing an imported setting or editor control");
                require(control(PreferenceKey::MainToolRail) && mainToolRailApplyButton && mainToolRailVisibleList &&
                                mainToolRailAvailableList &&
                                std::string(
                                        gtk_editable_get_text(GTK_EDITABLE(control(PreferenceKey::MainToolRail)))) ==
                                        "SMART_SELECT,PEN,HIGHLIGHTER,ERASER,TEXT,HAND",
                        "Preferences dialog did not show the canvas-first main tool rail layout");
                const auto listRows = [](GtkWidget* list) {
                    size_t count = 0;
                    for (auto* row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row)) {
                        ++count;
                    }
                    return count;
                };
                require(listRows(mainToolRailVisibleList) == 6 && listRows(mainToolRailAvailableList) == 6,
                        "Main tool rail editor did not split visible and available tools");
                require(std::string(gtk_editable_get_text(GTK_EDITABLE(control(PreferenceKey::FloatingToolboxRow1)))) ==
                                        "PEN,HIGHLIGHTER,LASER_POINTER,PDF_TOOL,UNDO" &&
                                std::string(gtk_editable_get_text(
                                        GTK_EDITABLE(control(PreferenceKey::FloatingToolboxRow2)))) ==
                                        "ZOOM_SLIDER,DRAW_ELLIPSE,FORMAT_ALIGN_LEFT,FORMAT_ALIGN_CENTER,"
                                        "FORMAT_ALIGN_RIGHT,FORMAT_JUSTIFY",
                        "Preferences rows did not show the imported legacy floating toolbar layout");
                require(adw_combo_row_get_selected(ADW_COMBO_ROW(control(PreferenceKey::StylusButtonOne))) ==
                                        static_cast<guint>(prototype::ButtonToolAction::Highlighter) &&
                                adw_combo_row_get_selected(ADW_COMBO_ROW(control(PreferenceKey::StylusButtonTwo))) ==
                                        static_cast<guint>(prototype::ButtonToolAction::Hand),
                        "Preferences rows did not show imported stylus mappings");
                require(adw_switch_row_get_active(ADW_SWITCH_ROW(control(PreferenceKey::SnapRotation))),
                        "Rotation snapping should be enabled by default in Preferences");
                capture("preferences-dialog", GTK_WIDGET(preferencesUi.dialog), true);
                adw_preferences_dialog_set_visible_page_name(preferencesUi.dialog, "latex");
                require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesUi.dialog)) == "latex",
                        "Preferences dialog did not open the LaTeX page");
                capture("preferences-latex", GTK_WIDGET(preferencesUi.dialog), true);
                adw_preferences_dialog_set_visible_page_name(preferencesUi.dialog, "toolbar");
                require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesUi.dialog)) == "toolbar",
                        "Preferences dialog did not switch to the Toolbar page");
                break;
            }
            case 2:
                capture("preferences-toolbar");
                {
                    GtkWidget* pageScroll = nullptr;
                    for (auto* parent = mainToolRailVisibleList ? gtk_widget_get_parent(mainToolRailVisibleList) :
                                                                  nullptr;
                         parent; parent = gtk_widget_get_parent(parent)) {
                        if (GTK_IS_SCROLLED_WINDOW(parent)) {
                            pageScroll = parent;
                        }
                    }
                    require(pageScroll, "Toolbar Preferences page does not provide a scroll container");
                    auto* adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(pageScroll));
                    gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment) -
                                                                 gtk_adjustment_get_page_size(adjustment));
                    capture("preferences-main-tool-rail", GTK_WIDGET(preferencesUi.dialog), true);
                }
                adw_preferences_dialog_set_visible_page_name(preferencesUi.dialog, "input");
                require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesUi.dialog)) == "input",
                        "Preferences dialog did not switch to the Input page");
                break;
            case 3: {
                capture("preferences-input");
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::PositionHighlight)), true);
                const auto fill = unpackArgbColor(0x7f123456U);
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(control(PreferenceKey::CursorHighlightColor)),
                                                 &fill);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::CursorHighlightRadius)), 17.0);
                const auto border = unpackArgbColor(0xc0abcdefU);
                gtk_color_dialog_button_set_rgba(
                        GTK_COLOR_DIALOG_BUTTON(control(PreferenceKey::CursorHighlightBorderColor)), &border);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::CursorHighlightBorderWidth)), 2.0);
                adw_preferences_dialog_set_visible_page_name(preferencesUi.dialog, "behavior");
                require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesUi.dialog)) == "behavior",
                        "Preferences dialog did not switch to the Behavior page");
                capture("preferences-behavior");
                adw_combo_row_set_selected(ADW_COMBO_ROW(control(PreferenceKey::Theme)), 1);
                adw_combo_row_set_selected(ADW_COMBO_ROW(control(PreferenceKey::StylusButtonOne)),
                                           static_cast<guint>(prototype::ButtonToolAction::Pen));
                adw_combo_row_set_selected(ADW_COMBO_ROW(control(PreferenceKey::StylusButtonTwo)),
                                           static_cast<guint>(prototype::ButtonToolAction::FloatingToolbox));
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::PressureSensitivity)), false);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::AutosaveTimeout)), 9);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::ZoomStep)), 22.0);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::RotationSnapTolerance)), 0.45);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::SnapRotation)), false);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::SnapGrid)), false);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::SnapGridSize)), 24.0);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::SnapGridTolerance)), 0.4);
                gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexDefaultText)), "\\frac{\\alpha}{2}");
                gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexTemplatePath)),
                                      "/tmp/template with spaces.tex");
                gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexGeneratorCommand)),
                                      "pdflatex -halt-on-error -interaction=nonstopmode '{}'");
                gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexExternalEditorCommand)),
                                      "'/tmp/fake latex editor' --wait");
                gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexTemporaryFileExtension)), "tex");
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexAutoCheckDependencies)), false);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexUseCustomEditorFont)), true);
                require(gtk_widget_get_sensitive(control(PreferenceKey::LatexEditorFont)),
                        "Enabling the custom LaTeX font did not enable its font selector");
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexEditorWordWrap)), false);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewAutoIndent)), false);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewSyntaxHighlight)),
                                          false);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewShowLineNumbers)), true);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexUseExternalEditor)), true);
                adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::LatexExternalEditorAutoConfirm)), true);
                require(!gtk_widget_get_sensitive(control(PreferenceKey::LatexEditorFont)) &&
                                !gtk_widget_get_sensitive(control(PreferenceKey::LatexSourceViewTheme)) &&
                                gtk_widget_get_sensitive(control(PreferenceKey::LatexExternalEditorCommand)) &&
                                gtk_widget_get_sensitive(control(PreferenceKey::LatexExternalEditorAutoConfirm)),
                        "LaTeX preference sensitivity did not follow custom font and external-editor settings");
                g_signal_emit_by_name(preferencesUi.latexSettingsApplyButton, "clicked");
                require(importedConfig.settings.latexDefaultText == "\\frac{\\alpha}{2}" &&
                                prototype::preferences_detail::pathToUtf8(importedConfig.settings.latexTemplatePath) ==
                                        "/tmp/template with spaces.tex" &&
                                importedConfig.settings.latexGeneratorCommand ==
                                        "pdflatex -halt-on-error -interaction=nonstopmode '{}'" &&
                                importedConfig.settings.latexExternalEditorCommand ==
                                        "'/tmp/fake latex editor' --wait" &&
                                !importedConfig.settings.latexAutoCheckDependencies &&
                                importedConfig.settings.latexUseCustomEditorFont &&
                                !importedConfig.settings.latexEditorWordWrap &&
                                !importedConfig.settings.latexSourceViewAutoIndent &&
                                !importedConfig.settings.latexSourceViewSyntaxHighlight &&
                                importedConfig.settings.latexSourceViewShowLineNumbers &&
                                importedConfig.settings.latexUseExternalEditor &&
                                importedConfig.settings.latexExternalEditorAutoConfirm &&
                                importedConfig.settings.latexTemporaryFileExtension == "tex" &&
                                std::string(gtk_label_get_text(GTK_LABEL(preferencesUi.latexSettingsApplyStatus))) == "Saved",
                        "Applying LaTeX preferences did not persist the text fields and editor options");
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanSpeed)), 31.0);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanMaxMultiplier)), 3.5);
                const std::array<PreferenceKey, 4> rowKeys = {
                        PreferenceKey::FloatingToolboxRow1,
                        PreferenceKey::FloatingToolboxRow2,
                        PreferenceKey::FloatingToolboxRow3,
                        PreferenceKey::FloatingToolboxRow4,
                };
                for (size_t index = 0; index < rowKeys.size(); ++index) {
                    const auto serialized = prototype::serializeFloatingToolboxRow(floatingModeTestRows[index]);
                    gtk_editable_set_text(GTK_EDITABLE(control(rowKeys[index])), serialized.c_str());
                }
                g_signal_emit_by_name(floatingToolboxApplyButton, "clicked");
                require(importedConfig.settings.floatingToolboxRows == floatingModeTestRows,
                        "Applying toolbar rows did not update the active layout");
                g_signal_emit_by_name(floatingToolboxApplyButton, "clicked");
                const auto listRows = [](GtkWidget* list) {
                    size_t count = 0;
                    for (auto* row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row)) {
                        ++count;
                    }
                    return count;
                };
                auto* penEditorRow = gtk_list_box_get_row_at_index(GTK_LIST_BOX(mainToolRailVisibleList), 1);
                auto* penDragSource = penEditorRow ? GTK_DRAG_SOURCE(g_object_get_data(G_OBJECT(penEditorRow),
                                                                                       "gtk4-main-tool-drag-source")) :
                                                     nullptr;
                GdkContentProvider* dragProvider = nullptr;
                if (penDragSource) {
                    g_signal_emit_by_name(penDragSource, "prepare", 0.0, 0.0, &dragProvider);
                }
                auto* dragFormats = dragProvider ? gdk_content_provider_ref_formats(dragProvider) : nullptr;
                require(dragFormats && gdk_content_formats_contain_gtype(dragFormats, G_TYPE_STRING),
                        "Main tool rail drag source did not provide a string action token");
                gdk_content_formats_unref(dragFormats);
                g_object_unref(dragProvider);

                auto* highlighterRow = gtk_list_box_get_row_at_index(GTK_LIST_BOX(mainToolRailVisibleList), 2);
                graphene_rect_t highlighterRowBounds{};
                require(highlighterRow && gtk_widget_compute_bounds(GTK_WIDGET(highlighterRow), mainToolRailVisibleList,
                                                                    &highlighterRowBounds),
                        "Could not locate the target row for rail reordering");
                const double reorderTextY = highlighterRowBounds.origin.y + highlighterRowBounds.size.height * 0.25;
                const bool reorderAccepted = emitMainToolRailDropForTest(mainToolRailVisibleList, "TEXT", reorderTextY);
                const std::string reorderedLayout =
                        gtk_editable_get_text(GTK_EDITABLE(control(PreferenceKey::MainToolRail)));
                require(reorderAccepted && reorderedLayout == "SMART_SELECT,PEN,TEXT,HIGHLIGHTER,ERASER,HAND",
                        "Dropping a visible rail tool did not reorder the action layout");
                require(applyMainToolRailDrop("LINK", true, 1) &&
                                std::string(
                                        gtk_editable_get_text(GTK_EDITABLE(control(PreferenceKey::MainToolRail)))) ==
                                        "SMART_SELECT,LINK,PEN,TEXT,HIGHLIGHTER,ERASER,HAND" &&
                                listRows(mainToolRailVisibleList) == 7 && listRows(mainToolRailAvailableList) == 5,
                        "Adding an available tool did not update both lists and the saved order");
                require(applyMainToolRailDrop("LINK", false, 0) &&
                                std::string(
                                        gtk_editable_get_text(GTK_EDITABLE(control(PreferenceKey::MainToolRail)))) ==
                                        "SMART_SELECT,PEN,TEXT,HIGHLIGHTER,ERASER,HAND" &&
                                listRows(mainToolRailVisibleList) == 6 && listRows(mainToolRailAvailableList) == 6,
                        "Removing a visible tool did not return it to More tools");
                require(!applyMainToolRailDrop("SMART_SELECT", false, 0),
                        "Smart Select was accepted for removal from the main tool rail");
                gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::MainToolRail)), "SMART_SELECT,TEXT,PEN,HAND");
                g_signal_emit_by_name(mainToolRailApplyButton, "clicked");
                require(importedConfig.settings.mainToolRail == std::vector<prototype::MainToolAction>({
                                                                        prototype::MainToolAction::SmartSelect,
                                                                        prototype::MainToolAction::Text,
                                                                        prototype::MainToolAction::Pen,
                                                                        prototype::MainToolAction::Hand,
                                                                }) &&
                                gtk_widget_get_first_child(mainToolGroup) == toolButtons[3] &&
                                gtk_widget_get_next_sibling(gtk_widget_get_first_child(mainToolGroup)) == textButton,
                        "Applying main tool rail preferences did not preserve the configured order");
                size_t moreToolsCount = 0;
                for (auto* child = gtk_widget_get_first_child(moreToolsGroup); child;
                     child = gtk_widget_get_next_sibling(child)) {
                    ++moreToolsCount;
                }
                require(moreToolsCount == prototype::mainToolRailNames.size() - 4 &&
                                gtk_widget_get_visible(moreToolsButton),
                        "Tools omitted from the main rail were not kept under More tools");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
                require(state.brush.tool == Tool::SmartSelect,
                        "The primary Smart Select rail button did not activate Smart Select");
                selectTool(Tool::Pen);
                size_t renderedRows = 0;
                for (auto* row = gtk_widget_get_first_child(floatingToolboxRowsContainer); row;
                     row = gtk_widget_get_next_sibling(row)) {
                    ++renderedRows;
                }
                require(renderedRows == 4 && floatingToolboxToolButtons.size() == 17,
                        "Configured rows were not rendered or rebuilding duplicated tool buttons");
                auto* firstConfiguredRow = gtk_widget_get_first_child(floatingToolboxRowsContainer);
                auto* configuredSmartSelect = gtk_widget_get_first_child(firstConfiguredRow);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(configuredSmartSelect), true);
                require(state.brush.tool == Tool::SmartSelect,
                        "Configured Smart Select action did not select the Smart Select tool");
                selectTool(Tool::Pen);
                auto* thirdConfiguredRow = gtk_widget_get_next_sibling(gtk_widget_get_next_sibling(firstConfiguredRow));
                auto* fineButton = gtk_widget_get_next_sibling(gtk_widget_get_first_child(thirdConfiguredRow));
                g_signal_emit_by_name(fineButton, "clicked");
                require(std::abs(state.brush.width - prototype::widthsForTool(Tool::Pen)[1]) < 1e-8,
                        "Configured FINE action did not select the Pen fine-width preset");
                auto* fourthConfiguredRow = gtk_widget_get_next_sibling(thirdConfiguredRow);
                const auto originalDrawingMode = state.drawingMode;
                g_signal_emit_by_name(gtk_widget_get_first_child(fourthConfiguredRow), "clicked");
                require(state.drawingMode == prototype::DrawingMode::Arrow,
                        "Configured DRAW_ARROW action did not select arrow drawing mode");
                const auto findConfiguredDrawingMode = [&](std::string_view token) -> GtkWidget* {
                    for (auto* row = gtk_widget_get_first_child(floatingToolboxRowsContainer); row;
                         row = gtk_widget_get_next_sibling(row)) {
                        for (auto* child = gtk_widget_get_first_child(row); child;
                             child = gtk_widget_get_next_sibling(child)) {
                            const auto* configuredToken = static_cast<const char*>(
                                    g_object_get_data(G_OBJECT(child), "floating-drawing-mode-token"));
                            if (configuredToken && token == configuredToken) {
                                return child;
                            }
                        }
                    }
                    return nullptr;
                };
                for (const auto& drawingAction: prototype::floatingDrawingModeActions) {
                    auto* drawingButton = findConfiguredDrawingMode(drawingAction.token);
                    require(drawingButton, "A configured master drawing-mode token did not render as an action");
                    g_signal_emit_by_name(drawingButton, "clicked");
                    require(state.drawingMode == drawingAction.mode,
                            "A configured master drawing-mode token selected the wrong drawing behavior");
                }
                const auto findConfiguredDrawingModeCombo = [&]() -> GtkDropDown* {
                    for (auto* row = gtk_widget_get_first_child(floatingToolboxRowsContainer); row;
                         row = gtk_widget_get_next_sibling(row)) {
                        for (auto* child = gtk_widget_get_first_child(row); child;
                             child = gtk_widget_get_next_sibling(child)) {
                            const auto* configuredToken = static_cast<const char*>(
                                    g_object_get_data(G_OBJECT(child), "floating-drawing-mode-token"));
                            if (configuredToken && std::string_view(configuredToken) == "DRAW") {
                                return GTK_DROP_DOWN(child);
                            }
                        }
                    }
                    return nullptr;
                };
                auto* drawingModeCombo = findConfiguredDrawingModeCombo();
                require(drawingModeCombo, "The configured DRAW combo token did not render as a mode selector");
                for (size_t index = 0; index < prototype::floatingDrawingModeChoices.size(); ++index) {
                    gtk_drop_down_set_selected(drawingModeCombo, static_cast<guint>(index));
                    const auto selectedMode = prototype::floatingDrawingModeComboMode(index);
                    require(selectedMode && state.drawingMode == *selectedMode,
                            "The DRAW combo selected the wrong drawing mode");
                }
                gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown), static_cast<guint>(originalDrawingMode));
                require(state.drawingMode == originalDrawingMode &&
                                gtk_drop_down_get_selected(drawingModeCombo) ==
                                        prototype::floatingDrawingModeComboIndex(originalDrawingMode),
                        "The floating drawing-mode smoke check did not restore the selected mode");
                require(floatingFillToggles.size() == 1,
                        "The configured TOOL_FILL token did not render as a toggle button");
                selectTool(Tool::Pen);
                gtk_toggle_button_set_active(floatingFillToggles.front(), true);
                require(state.brush.fillEnabled && penBrush.fillEnabled && importedConfig.settings.penFillEnabled,
                        "TOOL_FILL did not enable Pen fill");
                const auto fillDrawingMode = state.drawingMode;
                state.drawingMode = prototype::DrawingMode::Rectangle;
                const auto fillStart = state.pageScreenPoint(state.page, 30, 30);
                const auto fillEnd = state.pageScreenPoint(state.page, 100, 80);
                require(state.begin(prototype::Device::Mouse, fillStart.x, fillStart.y),
                        "Filled Pen rectangle did not begin");
                state.append(prototype::Device::Mouse, fillEnd.x, fillEnd.y);
                state.finish(prototype::Device::Mouse);
                require(!state.ink.empty() && state.ink.back().fill == importedConfig.settings.penFillOpacity,
                        "TOOL_FILL did not apply the configured opacity to a new Pen shape");
                state.undo();
                state.drawingMode = fillDrawingMode;
                selectTool(Tool::Highlighter);
                gtk_toggle_button_set_active(floatingFillToggles.front(), true);
                require(state.brush.fillEnabled && highlightBrush.fillEnabled &&
                                importedConfig.settings.highlighterFillEnabled,
                        "TOOL_FILL did not enable Highlighter fill");
                selectTool(Tool::Pen);
                const auto selectionDrawingMode = state.drawingMode;
                state.drawingMode = prototype::DrawingMode::Rectangle;
                const auto selectionFillStart = state.pageScreenPoint(state.page, 140, 30);
                const auto selectionFillEnd = state.pageScreenPoint(state.page, 210, 90);
                require(state.begin(prototype::Device::Mouse, selectionFillStart.x, selectionFillStart.y),
                        "Filled Pen rectangle for selection fill did not begin");
                state.append(prototype::Device::Mouse, selectionFillEnd.x, selectionFillEnd.y);
                state.finish(prototype::Device::Mouse);
                const auto filledPenIndex = state.ink.size() - 1;
                state.selected = {filledPenIndex};
                selectTool(Tool::SmartSelect);
                require(fillToggleAvailable() && !gtk_toggle_button_get_active(floatingFillToggles.front()),
                        "TOOL_FILL was not enabled for a selected Pen stroke");
                gtk_toggle_button_set_active(floatingFillToggles.front(), true);
                gtk_toggle_button_set_active(floatingFillToggles.front(), false);
                require(state.ink[filledPenIndex].fill == -1,
                        "TOOL_FILL did not remove fill from the selected Pen stroke");
                state.undo();
                update();
                require(state.ink[filledPenIndex].fill == importedConfig.settings.penFillOpacity,
                        "Undo did not restore the selected Pen stroke fill");
                state.redo();
                update();
                require(state.ink[filledPenIndex].fill == -1, "Redo did not remove the selected Pen stroke fill");
                gtk_toggle_button_set_active(floatingFillToggles.front(), true);
                require(state.ink[filledPenIndex].fill == importedConfig.settings.penFillOpacity,
                        "TOOL_FILL did not fill the selected Pen stroke");
                selectTool(Tool::Pen);
                state.drawingMode = selectionDrawingMode;
                require(gtk_toggle_button_get_active(floatingFillToggles.front()),
                        "TOOL_FILL did not synchronize when switching back to the filled Pen");
                require(floatingFillOpacityButtons.size() == 1 &&
                                gtk_widget_get_sensitive(GTK_WIDGET(floatingFillOpacityButtons.front())),
                        "The configured FILL_OPACITY token did not render as an enabled action");
                std::string opacityActionError;
                require(activatePluginAction("fill-opacity", opacityActionError) && fillOpacityDialog &&
                                fillOpacityTarget == Tool::Pen,
                        opacityActionError.empty() ? "The fill-opacity plugin action did not open the Pen dialog" :
                                                     opacityActionError.c_str());
                auto* fillOpacityAdjustment = gtk_range_get_adjustment(fillOpacityScale);
                require(gtk_adjustment_get_lower(fillOpacityAdjustment) == 5 &&
                                gtk_adjustment_get_upper(fillOpacityAdjustment) == 100,
                        "The fill-opacity chooser did not use the master 5–100 percent range");
                gtk_range_set_value(fillOpacityScale, 75);
                g_signal_emit_by_name(fillOpacityCancelButton, "clicked");
                require(!fillOpacityDialog && importedConfig.settings.penFillOpacity == 128 &&
                                penBrush.fillOpacity == 128,
                        "Cancelling the Pen fill-opacity dialog changed its stored value");
                g_signal_emit_by_name(floatingFillOpacityButtons.front(), "clicked");
                require(fillOpacityDialog && fillOpacityTarget == Tool::Pen,
                        "The floating FILL_OPACITY button did not open the Pen dialog");
                gtk_range_set_value(fillOpacityScale, 64);
                g_signal_emit_by_name(fillOpacityApplyButton, "clicked");
                require(!fillOpacityDialog && importedConfig.settings.penFillOpacity == 163 &&
                                penBrush.fillOpacity == 163 && state.brush.fillOpacity == 163,
                        "Applying Pen fill opacity did not update and persist the 64 percent value");
                selectTool(Tool::Highlighter);
                g_signal_emit_by_name(floatingFillOpacityButtons.front(), "clicked");
                require(fillOpacityDialog && fillOpacityTarget == Tool::Highlighter,
                        "The floating FILL_OPACITY button did not open the Highlighter dialog");
                gtk_range_set_value(fillOpacityScale, 40);
                g_signal_emit_by_name(fillOpacityApplyButton, "clicked");
                require(!fillOpacityDialog && importedConfig.settings.highlighterFillOpacity == 102 &&
                                highlightBrush.fillOpacity == 102 && state.brush.fillOpacity == 102,
                        "Applying Highlighter fill opacity did not update and persist the 40 percent value");
                selectTool(Tool::Pen);
                require(importedConfig.settings.themeVariant == prototype::ThemeVariant::ForceLight &&
                                importedConfig.settings.stylusButtonOne == prototype::ButtonToolAction::Pen &&
                                importedConfig.settings.stylusButtonTwo ==
                                        prototype::ButtonToolAction::FloatingToolbox &&
                                !importedConfig.settings.pressureSensitivity &&
                                importedConfig.settings.highlightPosition &&
                                importedConfig.settings.cursorHighlightColor == 0x7f123456U &&
                                std::abs(importedConfig.settings.cursorHighlightRadius - 17.0) < 1e-8 &&
                                importedConfig.settings.cursorHighlightBorderColor == 0xc0abcdefU &&
                                std::abs(importedConfig.settings.cursorHighlightBorderWidth - 2.0) < 1e-8 &&
                                !importedConfig.settings.snapRotation && !state.snapRotationEnabled &&
                                !importedConfig.settings.snapGrid && !state.snapGridEnabled &&
                                std::abs(state.snapGridSize - 24.0) < 1e-8 &&
                                std::abs(state.snapGridTolerance - 0.4) < 1e-8 &&
                                std::abs(state.edgePanSpeed - 31.0) < 1e-8 &&
                                std::abs(state.edgePanMaxMultiplier - 3.5) < 1e-8 &&
                                importedConfig.settings.autosaveTimeoutMinutes == 9 &&
                                importedConfig.settings.penFillEnabled &&
                                importedConfig.settings.penFillOpacity == 163 &&
                                importedConfig.settings.highlighterFillEnabled &&
                                importedConfig.settings.highlighterFillOpacity == 102 &&
                                std::abs(importedConfig.settings.zoomStep - 0.22) < 1e-8 &&
                                std::abs(importedConfig.settings.snapRotationTolerance - 0.45) < 1e-8 &&
                                importedConfig.settings.floatingToolboxRows == floatingModeTestRows &&
                                importedConfig.settings.latexDefaultText == "\\frac{\\alpha}{2}" &&
                                prototype::preferences_detail::pathToUtf8(importedConfig.settings.latexTemplatePath) ==
                                        "/tmp/template with spaces.tex" &&
                                importedConfig.settings.latexGeneratorCommand ==
                                        "pdflatex -halt-on-error -interaction=nonstopmode '{}'" &&
                                importedConfig.settings.latexExternalEditorCommand ==
                                        "'/tmp/fake latex editor' --wait" &&
                                !importedConfig.settings.latexAutoCheckDependencies &&
                                importedConfig.settings.latexUseCustomEditorFont &&
                                !importedConfig.settings.latexEditorWordWrap &&
                                !importedConfig.settings.latexSourceViewAutoIndent &&
                                !importedConfig.settings.latexSourceViewSyntaxHighlight &&
                                importedConfig.settings.latexSourceViewShowLineNumbers &&
                                importedConfig.settings.latexUseExternalEditor &&
                                importedConfig.settings.latexExternalEditorAutoConfirm &&
                                importedConfig.settings.latexTemporaryFileExtension == "tex" &&
                                importedConfig.settings.mainToolRail == std::vector<prototype::MainToolAction>({
                                                                                prototype::MainToolAction::SmartSelect,
                                                                                prototype::MainToolAction::Text,
                                                                                prototype::MainToolAction::Pen,
                                                                                prototype::MainToolAction::Hand,
                                                                        }) &&
                                std::filesystem::is_regular_file(preferencesConfigDir / "gtk4-canvas.ini"),
                        "Changing preferences did not immediately save and apply the edited values");
                {
                    const auto pageStart = state.pageScreenPoint(0, 230, 310);
                    const auto pageEnd = state.pageScreenPoint(0, 280, 310);
                    selectTool(Tool::Highlighter);
                    activeStylusAction = importedConfig.settings.stylusButtonOne;
                    begin(Device::Stylus, pageStart.x, pageStart.y, 0.5);
                    motion(Device::Stylus, pageEnd.x, pageEnd.y, 0.75);
                    finish(Device::Stylus, 0.75);
                    activeStylusAction = prototype::ButtonToolAction::None;
                    require(state.ink.back().brush.tool == Tool::Pen && state.brush.tool == Tool::Highlighter,
                            "A stylus button override did not apply temporarily or changed the toolbar tool");
                    state.undo();
                    selectTool(Tool::Pen);
                    const auto handStart = state.pageScreenPoint(0, 300, 330);
                    const auto handOffset = state.offsetX;
                    activeStylusAction = prototype::ButtonToolAction::Hand;
                    begin(Device::Stylus, handStart.x, handStart.y);
                    const bool handStarted = input.panning();
                    motion(Device::Stylus, handStart.x + 15, handStart.y + 10);
                    finish(Device::Stylus);
                    activeStylusAction = prototype::ButtonToolAction::None;
                    require(handStarted && std::abs(state.offsetX - handOffset - 15) < 1e-6 &&
                                    state.brush.tool == Tool::Pen,
                            "The stylus Hand mapping did not pan without changing the toolbar tool");
                }
                adw_dialog_close(ADW_DIALOG(preferencesUi.dialog));
                {
                    const auto anchor =
                            prototype::Point{gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0};
                    const auto inkCount = state.ink.size();
                    selectTool(Tool::FloatingToolbox);
                    begin(Device::Mouse, anchor.x, anchor.y);
                    require(gtk_widget_get_visible(floatingToolbox) && state.brush.tool == Tool::FloatingToolbox,
                            "Selecting Floating Toolbox did not show it at the canvas pointer");
                    showFloatingToolbox(0, 0);
                    require(gtk_widget_get_margin_start(floatingToolbox) == 10 &&
                                    gtk_widget_get_margin_top(floatingToolbox) == 10,
                            "Floating toolbox did not clamp to the canvas bounds");
                    showFloatingToolbox(anchor.x, anchor.y);
                    const auto penButton =
                            std::find_if(floatingToolboxToolButtons.begin(), floatingToolboxToolButtons.end(),
                                         [](const auto& entry) { return entry.first == Tool::Pen; });
                    require(penButton != floatingToolboxToolButtons.end(),
                            "Floating toolbox did not include the Pen action");
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(penButton->second), true);
                    require(!gtk_widget_get_visible(floatingToolbox) && state.brush.tool == Tool::Pen,
                            "Choosing a toolbox action did not select the tool and close the panel");
                    activeStylusAction = prototype::ButtonToolAction::FloatingToolbox;
                    begin(Device::Stylus, anchor.x, anchor.y);
                    activeStylusAction = prototype::ButtonToolAction::None;
                    require(gtk_widget_get_visible(floatingToolbox) && state.brush.tool == Tool::Pen &&
                                    state.ink.size() == inkCount,
                            "Stylus Floating Toolbox mapping changed the active tool or drew a stroke");
                    hideFloatingToolbox();
                }
                while (state.canUndo()) {
                    state.undo();
                }
                selectTool(Tool::Pen);
                require(state.ink.empty() && !modified(), "Preferences smoke checks left a modified test document");
                state.setPages({{595, 842}, {595, 842}, {595, 842}});
                pageCount = state.pageCount();
                state.selectActivePage(0);
                auto floatingLayers = state.pageLayers;
                floatingLayers[0].initialLayerCount = 3;
                floatingLayers[0].selectedLayer = 1;
                floatingLayers[0].backgroundName = "Paper";
                floatingLayers[0].layers = {{"Lower", true, {0}}, {"Upper", true, {1}}, {"Top", true, {2}}};
                state.setLayerDocument(floatingLayers);
                update();
                floatingToolboxSmokeReady = false;
                floatingToolboxSmokeSource = g_timeout_add(
                        400,
                        +[](gpointer data) -> gboolean {
                            auto* self = static_cast<Prototype*>(data);
                            self->showFloatingToolbox(gtk_widget_get_width(self->canvas) / 2.0,
                                                      gtk_widget_get_height(self->canvas) / 2.0);
                            const bool pageSpinnerReady =
                                    self->floatingPageSpinners.size() == 1 &&
                                    gtk_spin_button_get_value_as_int(self->floatingPageSpinners.front().first) == 1 &&
                                    gtk_adjustment_get_lower(gtk_spin_button_get_adjustment(
                                            self->floatingPageSpinners.front().first)) == 1 &&
                                    gtk_adjustment_get_upper(gtk_spin_button_get_adjustment(
                                            self->floatingPageSpinners.front().first)) == 3 &&
                                    std::string(gtk_label_get_text(self->floatingPageSpinners.front().second)) ==
                                            "of 3";
                            const bool layerMenuReady =
                                    self->floatingLayerMenus.size() == 1 &&
                                    std::string(gtk_label_get_text(
                                            self->floatingLayerMenus.front().activeLayerLabel)) == "Lower" &&
                                    self->findFloatingLayerControl(self->floatingLayerMenus.front(), 2, false) &&
                                    self->findFloatingLayerControl(self->floatingLayerMenus.front(), 2, true) &&
                                    self->findFloatingLayerControl(self->floatingLayerMenus.front(), 0, true) &&
                                    !gtk_widget_get_sensitive(
                                            GTK_WIDGET(self->floatingLayerMenus.front().addBelowButton));
                            self->floatingToolboxSmokeReady = gtk_widget_get_visible(self->floatingToolbox) &&
                                                              self->floatingToolboxToolButtons.size() >= 15 &&
                                                              pageSpinnerReady && layerMenuReady;
                            self->floatingToolboxSmokeSource = 0;
                            return G_SOURCE_REMOVE;
                        },
                        this);
                break;
            }
            case 4: {
                require(floatingToolboxSmokeReady, "Floating toolbox did not open with the expected tool actions");
                require(floatingPageSpinners.size() == 1 && state.page == 0 &&
                                gtk_spin_button_get_value_as_int(floatingPageSpinners.front().first) == 1 &&
                                std::string(gtk_label_get_text(floatingPageSpinners.front().second)) == "of 3",
                        "PAGE_SPIN did not expose the current page and page count");
                require(floatingLayerMenus.size() == 1 &&
                                std::string(gtk_label_get_text(floatingLayerMenus.front().activeLayerLabel)) == "Lower",
                        "LAYER did not display the current active layer");
                gtk_menu_button_popup(floatingLayerMenus.front().button);
                require(gtk_widget_get_visible(GTK_WIDGET(floatingLayerMenus.front().popover)),
                        "LAYER did not open its layer and visibility menu");
                auto* upperLayer = GTK_CHECK_BUTTON(findFloatingLayerControl(floatingLayerMenus.front(), 2, false));
                require(upperLayer, "LAYER did not list foreground layers for selection");
                gtk_check_button_set_active(upperLayer, true);
                require(state.activeLayerId(0) == 2 &&
                                std::string(gtk_label_get_text(floatingLayerMenus.front().activeLayerLabel)) ==
                                        "Upper" &&
                                !gtk_widget_get_visible(GTK_WIDGET(floatingLayerMenus.front().popover)),
                        "Selecting a layer in LAYER did not update the current layer and close the menu");
                gtk_menu_button_popup(floatingLayerMenus.front().button);
                auto* upperVisibility =
                        GTK_TOGGLE_BUTTON(findFloatingLayerControl(floatingLayerMenus.front(), 2, true));
                require(upperVisibility, "LAYER did not expose foreground visibility toggles");
                gtk_toggle_button_set_active(upperVisibility, false);
                require(!state.layerVisible(0, 2) &&
                                gtk_widget_get_visible(GTK_WIDGET(floatingLayerMenus.front().popover)),
                        "LAYER did not hide the selected layer while keeping its menu open");
                auto* backgroundVisibility =
                        GTK_TOGGLE_BUTTON(findFloatingLayerControl(floatingLayerMenus.front(), 0, true));
                require(backgroundVisibility, "LAYER did not expose background visibility");
                gtk_toggle_button_set_active(backgroundVisibility, false);
                require(!state.layerVisible(0, 0) &&
                                gtk_widget_get_visible(GTK_WIDGET(floatingLayerMenus.front().popover)),
                        "LAYER did not toggle background visibility while keeping its menu open");
                gtk_popover_popdown(floatingLayerMenus.front().popover);
                while (state.canUndo()) {
                    state.undo();
                }
                require(state.selectLayer(0, 1), "LAYER smoke checks could not restore the initial active layer");
                update();
                floatingToolboxSmokeReady = false;
                gtk_spin_button_set_value(floatingPageSpinners.front().first, 3);
                floatingToolboxSmokeSource = g_timeout_add(
                        180,
                        +[](gpointer data) -> gboolean {
                            auto* self = static_cast<Prototype*>(data);
                            self->floatingToolboxSmokeSource = 0;
                            if (self->state.page != 2 ||
                                gtk_spin_button_get_value_as_int(self->floatingPageSpinners.front().first) != 3 ||
                                std::string(gtk_label_get_text(GTK_LABEL(self->pageLabel))) != "3 / 3") {
                                std::cerr << "SMOKE FAILED: PAGE_SPIN did not navigate to the selected page: page="
                                          << self->state.page << " spin="
                                          << gtk_spin_button_get_value_as_int(self->floatingPageSpinners.front().first)
                                          << " label=" << gtk_label_get_text(GTK_LABEL(self->pageLabel)) << '\n';
                                self->failed = true;
                                g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                                return G_SOURCE_REMOVE;
                            }
                            gtk_spin_button_set_value(self->floatingPageSpinners.front().first, 1);
                            self->floatingToolboxSmokeSource = g_timeout_add(
                                    180,
                                    +[](gpointer verifyData) -> gboolean {
                                        auto* self = static_cast<Prototype*>(verifyData);
                                        self->floatingToolboxSmokeSource = 0;
                                        if (self->state.page != 0 ||
                                            gtk_spin_button_get_value_as_int(
                                                    self->floatingPageSpinners.front().first) != 1 ||
                                            std::string(gtk_label_get_text(GTK_LABEL(self->pageLabel))) != "1 / 3") {
                                            std::cerr << "SMOKE FAILED: PAGE_SPIN did not follow page navigation\n";
                                            self->failed = true;
                                            g_application_quit(G_APPLICATION(
                                                    gtk_window_get_application(GTK_WINDOW(self->window))));
                                            return G_SOURCE_REMOVE;
                                        }
                                        self->floatingToolboxSmokeReady = true;
                                        return G_SOURCE_REMOVE;
                                    },
                                    self);
                            return G_SOURCE_REMOVE;
                        },
                        this);
                break;
            }
            case 5:
                require(floatingToolboxSmokeReady, "PAGE_SPIN navigation and synchronization did not finish");
                capture("floating-toolbox");
                floatingToolboxDismissSmokeReady = false;
                scheduleFloatingToolboxHide();
                floatingToolboxDismissSmokeSource = g_timeout_add(
                        250,
                        +[](gpointer data) -> gboolean {
                            auto* self = static_cast<Prototype*>(data);
                            self->floatingToolboxDismissSmokeReady = !gtk_widget_get_visible(self->floatingToolbox);
                            self->floatingToolboxDismissSmokeSource = 0;
                            return G_SOURCE_REMOVE;
                        },
                        this);
                break;
            case 6:
                require(floatingToolboxDismissSmokeReady,
                        "Floating toolbox did not close after the pointer-leave delay");
                capture("preferences-edited");
                std::cout << "PASS: Adw Preferences editing, legacy stylus mapping import and persistent GTK 4 "
                             "settings\n";
                gtk_window_close(GTK_WINDOW(window));
                return G_SOURCE_REMOVE;
            default:
                return G_SOURCE_CONTINUE;
        }
    } else if (preferencesVerifyMode == "reload") {
        switch (preferencesVerifyStep++) {
            case 0:
                require(importedConfig.settings.themeVariant == prototype::ThemeVariant::ForceLight &&
                                importedConfig.settings.stylusButtonOne == prototype::ButtonToolAction::Pen &&
                                importedConfig.settings.stylusButtonTwo ==
                                        prototype::ButtonToolAction::FloatingToolbox &&
                                !importedConfig.settings.pressureSensitivity &&
                                importedConfig.settings.highlightPosition &&
                                importedConfig.settings.cursorHighlightColor == 0x7f123456U &&
                                std::abs(importedConfig.settings.cursorHighlightRadius - 17.0) < 1e-8 &&
                                importedConfig.settings.cursorHighlightBorderColor == 0xc0abcdefU &&
                                std::abs(importedConfig.settings.cursorHighlightBorderWidth - 2.0) < 1e-8 &&
                                !importedConfig.settings.snapRotation && !state.snapRotationEnabled &&
                                !importedConfig.settings.snapGrid && !state.snapGridEnabled &&
                                std::abs(state.snapGridSize - 24.0) < 1e-8 &&
                                std::abs(state.snapGridTolerance - 0.4) < 1e-8 &&
                                importedConfig.settings.autosaveTimeoutMinutes == 9 &&
                                importedConfig.settings.penFillEnabled &&
                                importedConfig.settings.penFillOpacity == 163 &&
                                importedConfig.settings.highlighterFillEnabled &&
                                importedConfig.settings.highlighterFillOpacity == 102 &&
                                std::abs(importedConfig.settings.zoomStep - 0.22) < 1e-8 &&
                                std::abs(importedConfig.settings.snapRotationTolerance - 0.45) < 1e-8 &&
                                importedConfig.settings.floatingToolboxRows == floatingModeTestRows,
                        "GTK 4 preference edits were not restored at next startup");
                openPreferences();
                break;
            default:
                require(adw_combo_row_get_selected(ADW_COMBO_ROW(control(PreferenceKey::Theme))) == 1 &&
                                adw_combo_row_get_selected(ADW_COMBO_ROW(control(PreferenceKey::StylusButtonOne))) ==
                                        static_cast<guint>(prototype::ButtonToolAction::Pen) &&
                                !adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::PressureSensitivity))) &&
                                adw_switch_row_get_active(ADW_SWITCH_ROW(control(PreferenceKey::PositionHighlight))),
                        "Preferences dialog did not display restored values");
                require(Prototype::packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(
                                control(PreferenceKey::CursorHighlightColor)))) == 0x7f123456U &&
                                std::abs(gtk_spin_button_get_value(
                                                 GTK_SPIN_BUTTON(control(PreferenceKey::CursorHighlightRadius))) -
                                         17.0) < 1e-8 &&
                                Prototype::packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(
                                        control(PreferenceKey::CursorHighlightBorderColor)))) == 0xc0abcdefU &&
                                std::abs(gtk_spin_button_get_value(
                                                 GTK_SPIN_BUTTON(control(PreferenceKey::CursorHighlightBorderWidth))) -
                                         2.0) < 1e-8,
                        "Preferences dialog did not restore pointer highlight appearance settings");
                require(!adw_switch_row_get_active(ADW_SWITCH_ROW(control(PreferenceKey::SnapRotation))),
                        "Preferences dialog did not restore the rotation snapping setting");
                require(!adw_switch_row_get_active(ADW_SWITCH_ROW(control(PreferenceKey::SnapGrid))) &&
                                std::abs(gtk_spin_button_get_value(
                                                 GTK_SPIN_BUTTON(control(PreferenceKey::SnapGridSize))) -
                                         24.0) < 1e-8 &&
                                std::abs(gtk_spin_button_get_value(
                                                 GTK_SPIN_BUTTON(control(PreferenceKey::SnapGridTolerance))) -
                                         0.4) < 1e-8,
                        "Preferences dialog did not restore page-grid snapping values");
                require(std::abs(gtk_spin_button_get_value(GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanSpeed))) -
                                 31.0) < 1e-8 &&
                                std::abs(gtk_spin_button_get_value(
                                                 GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanMaxMultiplier))) -
                                         3.5) < 1e-8,
                        "Preferences dialog did not restore selection edge-pan values");
                require(std::string(gtk_editable_get_text(GTK_EDITABLE(control(PreferenceKey::LatexDefaultText)))) ==
                                        "\\frac{\\alpha}{2}" &&
                                prototype::preferences_detail::pathToUtf8(importedConfig.settings.latexTemplatePath) ==
                                        "/tmp/template with spaces.tex" &&
                                std::string(gtk_editable_get_text(
                                        GTK_EDITABLE(control(PreferenceKey::LatexGeneratorCommand)))) ==
                                        "pdflatex -halt-on-error -interaction=nonstopmode '{}'" &&
                                std::string(gtk_editable_get_text(
                                        GTK_EDITABLE(control(PreferenceKey::LatexExternalEditorCommand)))) ==
                                        "'/tmp/fake latex editor' --wait" &&
                                !adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexAutoCheckDependencies))) &&
                                adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexUseCustomEditorFont))) &&
                                !adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexEditorWordWrap))) &&
                                !adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewAutoIndent))) &&
                                !adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewSyntaxHighlight))) &&
                                adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewShowLineNumbers))) &&
                                adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexUseExternalEditor))) &&
                                adw_switch_row_get_active(
                                        ADW_SWITCH_ROW(control(PreferenceKey::LatexExternalEditorAutoConfirm))) &&
                                std::string(gtk_editable_get_text(
                                        GTK_EDITABLE(control(PreferenceKey::LatexTemporaryFileExtension)))) == "tex",
                        "Preferences dialog did not restore LaTeX settings and editor options");
                capture("preferences-reloaded");
                std::cout << "PASS: GTK 4 preferences survive application restart\n";
                adw_dialog_close(ADW_DIALOG(preferencesUi.dialog));
                gtk_window_close(GTK_WINDOW(window));
                return G_SOURCE_REMOVE;
        }
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

gboolean Prototype::smokeSettingsVerify() {
    if (settingsVerifyMode == "fallback") {
        require(importedConfig.usedSettingsFallback, "Expected corrupt settings.xml to trigger fallback");
        require(importedConfig.palette.usedFallback && paletteSwatchButtons.size() == 11,
                "Expected corrupt or unreferenced palette to fall back to 11-color default");
        require(state.brush.tool == Tool::Pen && std::abs(state.brush.width - 1.41) < 1e-6 &&
                        state.eraserMode == prototype::EraserMode::Standard && std::abs(state.zoomStep - 0.10) < 1e-6 &&
                        autosaveSource != 0,
                "Corrupt settings.xml did not restore clean defaults");
        capture("settings-fallback");
        std::cout << "PASS: corrupt settings.xml and .gpl fallback\n";
        closeWindow();
        return G_SOURCE_REMOVE;
    }

    const bool expectDark = (settingsVerifyMode == "imported-dark");
    const auto savedOut = (std::filesystem::path(options.smokeDirectory) / "settings-saved.xopp").string();
    switch (settingsVerifyStep++) {
        case 0: {
            require(!importedConfig.usedSettingsFallback && !importedConfig.palette.usedFallback,
                    "Valid settings.xml or .gpl unexpectedly fell back to defaults");
            require(bool(adw_style_manager_get_dark(adw_style_manager_get_default())) == expectDark,
                    "Imported themeVariant did not set libadwaita light/dark scheme");
            require(std::abs(importedConfig.settings.snapRotationTolerance - 0.55) < 1e-8 &&
                            std::abs(state.snapRotationTolerance - 0.55) < 1e-8,
                    "Imported snapRotationTolerance did not reach the canvas state");
            require(autosaveSource == 0, "autosaveEnabled=false in settings.xml must disable autosave timer");
            require(state.brush.tool == Tool::Highlighter && std::abs(state.brush.width - 30.0) < 1e-6 &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 4u &&
                            std::abs(state.brush.color[0] - 1.0) < 1e-2 &&
                            std::abs(state.brush.color[1] - 0xa1 / 255.0) < 1e-2,
                    "Imported active Highlighter color or VERY_BIG preset mismatch");
            require(paletteSwatchButtons.size() == 11 && importedConfig.palette.paletteName == "Xournal++ Palette",
                    "Imported .gpl palette swatches or Name header mismatch");
            const auto h0 = state.pageScreenPoint(0, 72, 136);
            const auto h1 = state.pageScreenPoint(0, 290, 136);
            g_signal_emit_by_name(mouse, "drag-begin", h0.x, h0.y);
            g_signal_emit_by_name(mouse, "drag-update", h1.x - h0.x, 0.0);
            g_signal_emit_by_name(mouse, "drag-end", h1.x - h0.x, 0.0);
            require(state.ink.size() == 1 && state.ink.back().brush.tool == Tool::Highlighter &&
                            std::abs(state.ink.back().brush.width - 30.0) < 1e-6,
                    "Imported Highlighter did not commit 30pt stroke");
            capture("settings-imported");

            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            require(state.brush.tool == Tool::Pen && std::abs(state.brush.width - 2.26) < 1e-6 &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 3u &&
                            std::abs(state.brush.color[1] - 0x2e / 255.0) < 1e-2 &&
                            std::abs(state.brush.color[2] - 0x99 / 255.0) < 1e-2,
                    "Imported Pen BIG preset or #002e99 color mismatch");
            const auto p0 = state.pageScreenPoint(0, 72, 188);
            const auto p1 = state.pageScreenPoint(0, 290, 188);
            input.begin(Device::Stylus, {p0.x, p0.y, 0.05});
            input.motion(Device::Stylus, {p1.x, p1.y, 0.50});
            input.finish(Device::Stylus, 0.50);
            require(state.ink.size() == 2 && state.ink.back().pressureSensitive &&
                            std::abs(state.ink.back().points.front().pressure - 0.15) < 1e-6 &&
                            std::abs(state.ink.back().points.back().pressure - 0.60) < 1e-6,
                    "Imported minimumPressure=0.15 or pressureMultiplier=1.2 was not applied");
            gtk_menu_button_popup(GTK_MENU_BUTTON(paletteButton));
            break;
        }
        case 1:
            break;  // Let the .gpl palette popover finish opening before snapshot.
        case 2: {
            require(gtk_widget_get_visible(palettePopover), "Palette popover did not open");
            gtk_expander_set_expanded(GTK_EXPANDER(drawingOptionsExpander), true);
            break;
        }
        case 3: {
            capture("palette-popover");
            g_signal_emit_by_name(paletteSwatchButtons[2], "clicked");
            require(!gtk_widget_get_visible(palettePopover) && std::abs(state.brush.color[0] - 205 / 255.0) < 1e-2 &&
                            std::abs(state.brush.color[1] - 158 / 255.0) < 1e-2 &&
                            std::abs(state.brush.color[2] - 247 / 255.0) < 1e-2,
                    "Clicking .gpl Purple swatch did not update brush color or close popover");
            const auto p0 = state.pageScreenPoint(0, 72, 232);
            const auto p1 = state.pageScreenPoint(0, 290, 232);
            g_signal_emit_by_name(mouse, "drag-begin", p0.x, p0.y);
            g_signal_emit_by_name(mouse, "drag-update", p1.x - p0.x, 0.0);
            g_signal_emit_by_name(mouse, "drag-end", p1.x - p0.x, 0.0);
            require(state.ink.size() == 3 && std::abs(state.ink.back().brush.color[0] - 205 / 255.0) < 1e-2,
                    "Stroke did not use selected .gpl swatch color");

            const double zoomBefore = state.zoom;
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_plus, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && std::abs(state.zoom / zoomBefore - 1.25) < 1e-6,
                    "Ctrl++ did not use imported zoomStep=25%");

            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
            require(state.brush.tool == Tool::Eraser && state.eraserMode == prototype::EraserMode::DeleteStroke &&
                            std::abs(state.brush.width - 12.0) < 1e-6 &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 3u,
                    "Imported Eraser deleteStroke mode or BIG 12pt preset mismatch");
            break;
        }
        case 4:
            capture("palette-drawn");
            saveCopyTo(savedOut);
            break;
        default:
            require(!modified() && std::filesystem::is_regular_file(savedOut),
                    "Saving document after settings/palette import failed");
            std::cout << "PASS: read-only settings.xml and .gpl palette import (" << settingsVerifyMode << ")\n";
            closeWindow();
            return G_SOURCE_REMOVE;
    }
    update();
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

}  // namespace prototype::gtk4
