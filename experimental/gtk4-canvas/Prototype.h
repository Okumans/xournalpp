#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <adwaita.h>
#include <pango/pangocairo.h>
#include <poppler.h>
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
#include <gtksourceview/gtksource.h>
#endif

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include "ApplicationOptions.h"
#include "AudioPlayback.h"
#include "AudioTarget.h"
#include "BeamerPresentation.h"
#include "BehaviorDefaults.h"
#include "CanvasImageActions.h"
#include "CanvasInput.h"
#include "CanvasScrollMotion.h"
#include "CanvasState.h"
#include "ClipboardRenderer.h"
#include "DocumentAutosave.h"
#include "DocumentInk.h"
#include "DocumentJob.h"
#include "DocumentSession.h"
#include "Gtk4SearchController.h"
#include "ImageRenderer.h"
#include "InkRasterCache.h"
#include "InkRenderer.h"
#include "LatexEditor.h"
#include "LuaPluginRuntime.h"
#include "PageRasterRenderer.h"
#include "PaletteImport.h"
#include "PdfTextSelection.h"
#include "PreferencesDialog.h"
#include "PreferencesStore.h"
#include "SettingsImport.h"
#include "StrokeClipboard.h"
#include "StylusSamples.h"
#include "TextEditorOverlay.h"
#include "TextRenderer.h"


namespace prototype::gtk4 {
using prototype::CanvasState;
using prototype::Device;
using prototype::Ink;
using prototype::maxPageThumbnailHeight;
using prototype::maxPageThumbnailWidth;
using prototype::PageRaster;
using prototype::PageRasterLayerSource;
using prototype::PageRasterQuality;
using prototype::PageRasterRenderer;
using prototype::PageRasterRequest;
using prototype::Tool;


struct SnapshotPending {};

struct RecoveryInfo {
    std::string saveTarget;
    std::string saveVersion;
    std::string autosavePath;
};

inline std::vector<prototype::bridge::PageSizePacket> currentPageSizes(const CanvasState& state) {
    std::vector<prototype::bridge::PageSizePacket> sizes;
    sizes.reserve(static_cast<size_t>(state.pageCount()));
    for (int page = 0; page < state.pageCount(); ++page) {
        const auto size = state.pageSize(page);
        sizes.push_back({size.width, size.height});
    }
    return sizes;
}

constexpr int maxCanvasMediumLodDimension = 512;
constexpr int maxCanvasLowLodDimension = 192;

struct ImageGesture {
    enum class Mode { Move, Resize, Rotate };

    prototype::Device owner = prototype::Device::Mouse;
    Mode mode = Mode::Move;
    size_t index = 0;
    size_t corner = 0;
    prototype::Point start;
    prototype::Point center;
    double startAngle = 0;
    double rotationDelta = 0;
    prototype::CanvasImage original;
    prototype::CanvasImage preview;
    bool changed = false;
};

struct LatexTarget {
    std::optional<size_t> imageIndex;
    std::optional<size_t> textIndex;
};

struct PdfTextSelectionState {
    prototype::Device owner = prototype::Device::Mouse;
    int page = -1;
    Tool tool = Tool::SelectPdfTextLinear;
    prototype::PdfTextSelectionStyle style = prototype::PdfTextSelectionStyle::Linear;
    PopplerRectangle bounds{};
    prototype::Point toolbarAnchor;
    bool dragging = false;
    bool finalized = false;
    std::string text;
    std::vector<PopplerRectangle> textRects;
    prototype::CairoRegion region;
};

struct SmartSelectPdfGesture {
    prototype::Device owner = prototype::Device::Mouse;
    prototype::Point start;
    prototype::Point latest;
    bool altPressed = false;
};

enum class PdfTextAnnotation { Highlight, Underline, Strikethrough };

struct PdfHighlightPreset {
    const char* name;
    std::array<double, 3> color;
};

constexpr std::array<PdfHighlightPreset, 6> pdfHighlightPresets{{
        {"Yellow", {1.0, 224.0 / 255, 102.0 / 255}},
        {"Green", {140.0 / 255, 233.0 / 255, 154.0 / 255}},
        {"Blue", {116.0 / 255, 192.0 / 255, 252.0 / 255}},
        {"Pink", {250.0 / 255, 162.0 / 255, 193.0 / 255}},
        {"Orange", {1.0, 192.0 / 255, 120.0 / 255}},
        {"Purple", {208.0 / 255, 191.0 / 255, 1.0}},
}};

inline prototype::PdfTextSelectionStyle pdfSelectionStyleForTool(Tool tool) {
    return tool == Tool::SelectPdfTextRect ? prototype::PdfTextSelectionStyle::Rectangle :
                                             prototype::PdfTextSelectionStyle::Linear;
}

class Prototype: private SearchControllerHost {
public:
    struct FloatingLayerMenuWidgets {
        GtkMenuButton* button = nullptr;
        GtkLabel* activeLayerLabel = nullptr;
        GtkPopover* popover = nullptr;
        GtkBox* layerRows = nullptr;
        GtkButton* showAllButton = nullptr;
        GtkButton* hideAllButton = nullptr;
        GtkButton* addAboveButton = nullptr;
        GtkButton* addBelowButton = nullptr;
        int page = -1;
        std::optional<prototype::PageLayers> snapshot;
    };

    explicit Prototype(prototype::ApplicationOptions options);

    ~Prototype();

    int run();

private:
    struct PluginDialogContext {
        Prototype* prototype = nullptr;
        std::map<std::string, int> responses;
        std::function<void(int)> callback;
    };

    struct QuickScreenshotContext {
        Prototype* self = nullptr;
        std::string plugin;
        std::string directory;
        std::string imagePath;
        GSubprocess* process = nullptr;
        bool copyToClipboard = false;
    };

    static void fail(GError* error);

    static std::pair<PopplerDocument*, std::vector<prototype::PageSize>> openPreviewPdf(const std::string& path);

    static std::vector<PopplerDocument*> openLayerPreviewPdfs(const std::vector<std::filesystem::path>& paths,
                                                              int expectedPageCount);

    static std::vector<PopplerDocument*> openAudioLayerPreviewPdfs(const std::vector<std::filesystem::path>& paths,
                                                                   int expectedPageCount);

    static void releaseLayerPreviewPdfs(std::vector<PopplerDocument*>& documents);

    bool modified() const;

    static std::filesystem::path cacheRoot();

    static std::filesystem::path configRoot();

    std::string currentAutosaveTarget() const;

    void cleanupActiveAutosave();

    void scheduleAutosave(guint seconds);

    void waitForAutosave(std::function<void()> action);

    void startAutosave();

    void closeWindow();

    void updateTitle();

    void showAlert(const char* title, const std::string& message);

    void showError(const std::string& message);

    void activatePageLayoutAction(GSimpleAction* action, GVariant* parameter);

    const prototype::ViewModeSettings& activeViewModeSettings() const;

    void setPageLayoutActionsEnabled(bool enabled);

    void applyCurrentViewMode();

    void fitCurrentView(double width, double height);

    void setPresentationMode(bool enabled);

    void setFullscreenMode(bool enabled);

    void syncPageLayoutActions();

    void buildPageLayoutMenu(GtkWidget* header);

    static GdkRGBA unpackArgbColor(std::uint32_t color);

    static std::uint32_t packArgbColor(const GdkRGBA& color);

    bool applyFloatingToolboxRows();

    bool applyMainToolRail();

    void changePreference(PreferenceKey key, GtkWidget* control);

    void chooseLatexTemplate();

    bool applyLatexPreferences();

    void testLatexSetup();

    void initializePlugins();

    static void removeQuickScreenshotDirectory(const QuickScreenshotContext& context);

    void finishQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context);

    void failQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context, std::string message);

    void cancelQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context);

    bool launchQuickScreenshotProcess(const std::shared_ptr<QuickScreenshotContext>& context,
                                      const std::vector<std::string>& arguments, bool captureOutput,
                                      std::string& error);

    bool startQuickScreenshot(std::string_view plugin, std::string& error);

    void quickScreenshotReady(const std::shared_ptr<QuickScreenshotContext>& context);

    void copyQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context, GFile* destination);

    bool changePluginPageBackground(std::string_view format, std::string_view config, std::string& error);

    prototype::PluginDocumentStructure pluginDocumentStructure() const;

    bool transformSelectedImagesFromPlugin(std::int64_t action, std::string& error);

    void refreshNativeDocumentRevision(int changedPage);

    bool showPluginDialog(std::string_view pluginName, std::string_view message,
                          const std::vector<prototype::PluginDialogButton>& buttons, bool isError,
                          std::function<void(int)> callback, std::string& error);

    static std::optional<Tool> pluginToolFromName(std::string_view name);

    static std::string_view pluginToolName(Tool tool);

    static std::string_view pluginDrawingTypeName(prototype::DrawingMode mode);

    static std::string_view pluginToolSizeName(const std::array<double, 5>& widths, double width);

    static std::uint32_t pluginRgbColor(const std::array<double, 3>& color);

    bool changePluginToolColor(const std::optional<std::string>& toolName, std::uint32_t color, bool selection,
                               std::string& error);

    template <typename Object, typename Callback>
    bool forEachPluginObject(std::string_view type, const std::vector<Object>& objects,
                             const std::vector<size_t>& selectedIndices, Callback&& callback, std::string& error);

    bool getPluginStrokes(std::string_view type, std::vector<prototype::PluginStroke>& strokes, std::string& error);

    bool getPluginTexts(std::string_view type, std::vector<prototype::PluginText>& texts, std::string& error);

    bool getPluginLinks(std::string_view type, std::vector<prototype::PluginLink>& links, std::string& error);

    bool getPluginImages(std::string_view type, std::vector<prototype::PluginImage>& images, std::string& error);

    bool addPluginReferencesToSelection(const std::vector<std::uintptr_t>& references, std::string& error);

    bool addPluginStrokes(const std::vector<prototype::PluginStrokeInput>& inputs,
                          prototype::PluginStrokeUndoPolicy policy, std::vector<std::uintptr_t>& references,
                          std::string& error);

    bool addPluginTexts(const std::vector<prototype::PluginTextInput>& inputs, prototype::PluginStrokeUndoPolicy policy,
                        std::vector<std::uintptr_t>& references, std::string& error);

    bool addPluginLinks(const std::vector<prototype::PluginLinkInput>& inputs, prototype::PluginStrokeUndoPolicy policy,
                        std::vector<std::uintptr_t>& references, std::string& error);

    bool addPluginTextItems(std::vector<prototype::TextItem> additions, prototype::PluginStrokeUndoPolicy policy,
                            std::vector<std::uintptr_t>& references, std::string& error);

    bool addPluginImages(const std::vector<prototype::PluginImageInput>& inputs,
                         prototype::PluginStrokeUndoPolicy policy, std::vector<prototype::PluginImageResult>& results,
                         std::string& error);

    int pluginDisplayDpi() const;

    bool getPluginPageLabel(std::size_t pageNumber, std::string& label, std::string& error) const;

    std::vector<prototype::PluginPaletteColor> getPluginColorPalette() const;

    bool refreshPluginPage(std::string& error);

    bool setPluginFont(const prototype::PluginFont& font, std::string& error);

    prototype::PluginScrollPosition getPluginScrollPosition() const;

    bool setPluginZoom(double value, std::string& error);

    bool setPluginCurrentPage(std::size_t pageNumber, std::string& error);

    bool scrollPluginToPage(int pageNumber, bool relative, std::string& error);

    bool scrollPluginToPosition(double x, double y, bool relative, std::string& error);

    bool setPluginCurrentLayer(std::size_t layerNumber, bool updateVisibility, std::string& error);

    bool setPluginPageSize(double width, double height, bool relative, std::string& error);

    bool setPluginLayerVisibility(bool visible, std::string& error);

    bool setPluginCurrentLayerName(std::string_view name, std::string& error);

    bool setPluginBackgroundName(std::string_view name, std::string& error);

    std::int64_t pluginSelectedToolState() const;

    std::int64_t pluginToolSizeState(const std::array<double, 5>& widths, double width) const;

    std::int64_t pluginActiveToolSizeState() const;

    bool toolSupportsSelectionFill() const;

    bool fillToggleAvailable() const;

    bool fillToggleActive() const;

    bool setFillEnabled(bool enabled, std::string& error);

    bool fillOpacityAvailable() const;

    bool setFillOpacity(Tool tool, int opacity, std::string& error);

    void closeFillOpacityDialog(bool apply);

    bool openFillOpacityDialog(std::string& error);

    size_t selectedPenStrokeCount() const;

    std::optional<std::string> selectedPenLineStyleState() const;

    bool getPluginActionState(std::string_view action, prototype::PluginActionValue& value, std::string& error) const;

    bool getPluginToolInfo(std::string_view tool, prototype::PluginToolInfo& info, std::string& error) const;

    bool changePluginActionState(std::string_view action, const prototype::PluginActionValue& value,
                                 std::string& error);

    bool changePdfBackgroundPageFromPlugin(int pdfPage, std::string& error);

    bool stepBeamerPresentation(int delta, std::string& error);

    bool cleanBeamerAnimations(std::string& error);

    void showLayerActionsNotice(const char* message);

    void startLayerActionsClone(int currentPage, int pdfPage, bool allowAnnotated,
                                const std::shared_ptr<prototype::bridge::DocumentSession>& session,
                                const std::string& worker, const std::vector<prototype::bridge::StrokePacket>& packets,
                                const std::vector<prototype::TextItem>& texts,
                                const std::vector<prototype::bridge::ImagePacket>& images,
                                const prototype::bridge::LayerDocument& layers,
                                const std::vector<prototype::bridge::PageSizePacket>& pageSizes);

    bool cloneNextPageFromLayerActions(std::string& error);

    bool hideLayersFromLayerActions(std::string& error);

    bool addTopLayerToEveryPageFromLayerActions(std::string& error);

    bool activatePluginAction(std::string_view action, std::string& error);

    void openPreferences();

    void openPluginManager();

    void buildPluginManager();

    void rebuildPluginManagerRows();

    void applyPluginManagerChanges();

    void captureTopLevel(const std::string& name, GtkWidget* topLevel);

    void verifyQuickScreenshotPlugin();

    void verifyFloatingToolboxTextFormats();

    void verifyFloatingToolboxTextAlignment();

    void verifyPageLayoutIntegration();

    void verifyPresentationModeIntegration();

    void verifySidebarPerformanceIntegration();

    void verifySidebarStartupIntegration();

    void verifySearchIntegration();
    void verifySearchOverlayGeometry();

    void verifyScrollMotionIntegration();

    void verifyCursorFeedbackIntegration();

    void verifySmartSelectPerformanceIntegration();

    void verifyPenLineStyleIntegration();

    void verifyPluginIntegration();

    void verifyExportPlugin();

    void verifyFitToContentPlugin();

    void verifyBeamerIntegration();

    void verifyLayerActionsIntegration();

    void verifyLatexIntegration();

    void verifyImageActionsPlugin();

    void waitForPluginDocumentJob();

    void runDocumentJob(const char* status, std::function<void()> work, std::function<void()> success,
                        bool initial = false);

    void replaceDocument(const std::shared_ptr<prototype::bridge::DocumentSession>& session, const std::string& source,
                         const std::optional<RecoveryInfo>& recovery = std::nullopt);

    void openDocument(const std::string& path, bool initial = false);

    void recoverSidecarAutosave(const std::string& path, const std::string& sidecar, bool initial);

    void promptSidecarRecovery(const std::string& path, const std::string& sidecar, bool initial);

    void restoreStartupRecovery(const std::string& recoveryPath);

    void checkStartupRecovery();

    void requestOpenDocument(const std::string& path, bool initial = false);

    void saveCopyTo(const std::string& path, std::function<void()> after = {});

    void save(std::function<void()> after = {});

    void saveTo(const std::string& path, const std::string& expected = {}, std::function<void()> after = {});

    void chooseOpen();

    void chooseSave(std::function<void()> after = {});

    void finishFileChoice(GFile* file, GError* error, const std::function<void(const std::string&)>& chosen);

    void protectDocument(std::function<void()> after);

    void scheduleCanvasPagePreload();

    void navigateToPage(int number);

    void openGotoPageDialog();

    void openLayerRenameDialog(int pageIndex, size_t layerId);

    void exportPdfTo(const std::string& path);

    void startPluginExport(prototype::DocumentExportOptions exportOptions);

    bool exportDocumentFromPlugin(const prototype::DocumentExportOptions& exportOptions, std::string& error);

    void chooseExportPdf();

    void performHistoryStep(bool redo);

    void insertPageAfterCurrent();

    bool addEmptyPagesFromPlugin(bool force, std::string& error);

    void deleteCurrentPage();

    GtkWidget* button(const char* icon, const char* tooltip, GCallback callback);

    void openSearchBar();

    void closeSearchBar();

    void searchTextChanged();

    void searchNext();

    void searchPrevious();

    void ensureSearchMatchesFresh();

    bool canOpenSearchBar() const override;

    int searchPageCount() const override;

    prototype::CanvasState& searchCanvasState() override;

    PopplerPage* pdfPageForSearch(int page);

    PopplerPage* layerPdfPageForSearch(int page, size_t origin);

    std::optional<SearchPdfSource> searchBackgroundPdfSource(int page) const override;

    std::optional<SearchPdfSource> searchLayerPdfSource(int page, size_t origin) const override;

    std::shared_ptr<const void> searchDocumentLifetime() const override;

    const void* searchDocumentIdentity() const override;

    GtkWidget* searchCanvasWidget() const override;

    void finishTextEditingForSearch() override;

    void resetInputForSearch() override;

    void focusSearchMatch(const SearchMatch& match) override;

    void updateSearchCanvas() override;

    void syncSizeDropdown();

    void syncPenLineStyleDropdown();

    PopplerPage* pdfPageForPage(int page);

    bool smartSelectHasSelectedObjectAt(int page, prototype::Point point) const;

    bool smartSelectHasEditableObjectAt(int page, prototype::Point point) const;

    bool pdfTextAtCanvasPoint(int pageIndex, prototype::Point point);

    bool beginSmartSelectPdfGesture(Device device, double x, double y);

    void updatePdfActionsPosition(bool popup);

    void hidePdfActionsPreservingSelection();

    bool reopenPdfActionsForSelection(double x, double y);

    void clearPdfTextSelection(bool update = true);

    void beginPdfTextSelection(Device device, double x, double y);

    void updatePdfTextSelection(Device device, double x, double y);

    void finishPdfTextSelection(Device device, std::optional<prototype::Point> toolbarAnchor = std::nullopt);

    void switchPdfTextSelectionStyle();

    void selectPdfTextAtPoint(double x, double y, prototype::PdfTextSelectionStyle style);

    void copyPdfTextSelection(bool dismiss = true);

    static bool samePdfHighlight(const Ink& stroke, int page, size_t layerId, size_t originLayer,
                                 const PopplerRectangle& rect, int fill);

    void createPdfTextAnnotation(PdfTextAnnotation annotation);

    void setPdfHighlightColor(const std::array<double, 3>& color);

    void createPdfActionsPopover();

    void createLinkInfoPopover();

    void addFloatingToolboxToolAction(GtkWidget* row, Tool tool, const char* label);

    static std::string layerDisplayName(const prototype::PageLayers& pageLayers, size_t layerId);

    void appendFloatingLayerControl(FloatingLayerMenuWidgets& menu, int page, size_t layerId, const std::string& name,
                                    bool visible, bool selectable, bool active, GtkCheckButton*& activeGroup);

    static bool sameFloatingLayerStructure(const prototype::PageLayers& first, const prototype::PageLayers& second);

    void updateFloatingLayerMenuSensitivity(FloatingLayerMenuWidgets& menu, const prototype::PageLayers& pageLayers);

    void updateFloatingLayerMenuRows(FloatingLayerMenuWidgets& menu, int page, const prototype::PageLayers& pageLayers);

    void rebuildFloatingLayerRows(FloatingLayerMenuWidgets& menu, int page, const prototype::PageLayers& pageLayers);

    void rebuildFloatingLayerMenus();

    void scheduleFloatingLayerMenuRebuild();

    void updateFloatingLayerMenus();

    GtkWidget* findFloatingLayerControl(const FloatingLayerMenuWidgets& menu, size_t layerId, bool visibility) const;

    void addFloatingLayerMenu(GtkWidget* row);

    void addFloatingToolboxAction(GtkWidget* row, const std::string& token);

    void rebuildFloatingToolboxRows();

    void selectPaletteColor(size_t index);

    void createFloatingToolbox();

    void positionFloatingToolbox(double x, double y);

    void showFloatingToolbox(double x, double y);

    void hideFloatingToolbox();

    void scheduleFloatingToolboxHide();

    guint selectionModeIndex(Tool tool) const;

    void activateSelection(Tool tool, bool acrossLayers = false);

    void copyImagesToClipboard(const std::vector<prototype::CanvasImage>& images);

    void copyObjectsToClipboard(const std::vector<prototype::EditableObject>& objects,
                                const prototype::SelectionBox& bounds);

    void copySelection(bool cut = false);

    void duplicateSelection();

    void finishClipboardRead(const std::string& error);

    bool placePastedImages(std::vector<prototype::CanvasImage> images);

    bool placePastedImage(prototype::CanvasImage image);

    bool placePastedTexts(std::vector<prototype::TextItem> texts);

    bool placePastedObjects(std::vector<prototype::EditableObject> objects);

    void pasteExternalText();

    void pasteExternalImage();

    void pasteSelection();

    std::optional<size_t> textAt(int page, prototype::Point point) const;

    void closeLinkEditor();

    void openLinkEditor(int page, prototype::Point position, std::optional<size_t> index);

    void openLinkUrl(const std::string& url);

    void showLinkInfo(size_t index);

    void updateLinkHover(double x, double y);

    void handleLinkClick(double x, double y, int presses, bool controlDown);

    static uint32_t packTextColor(const std::array<double, 3>& color);

    void syncTextColorControl(uint32_t color);

    void updateTextToolbarVisibility();

    bool beginTextTransform(Device device, double x, double y);

    void applyTextColor(uint32_t color);

    void startTextEditing(double x, double y);

    void selectTextWordAt(double x, double y);

    void finishTextEditing();

    void updateTextEditorPosition();

    void applyTextFormat(prototype::TextEditorOverlay::Format format);

    void applyTextAlignment(int alignment);

    void applyTextJustify(bool justify);

    void selectTool(Tool tool);

    GtkWidget* createPaletteSwatch(size_t index);

    GtkWidget* mainToolWidget(prototype::MainToolAction action) const;

    void rebuildMainToolRail();

    GtkWidget* createMainToolRail();

    GtkWidget* createPalette();

    GtkWidget* createSidebar();

    void rebuildLayerStack();

    GtkWidget* findLayerControl(size_t layerId, bool visibility) const;

    void updateLayerActionSensitivity(const prototype::PageLayers& pageLayers);

    void refreshAfterLayerMutation();

    void prioritizeVisibleSidebarThumbnails();

    void requestSidebarThumbnail(int number);

    void drawThumbnail(cairo_t* cr, int number, int width, int height, bool selected);

    void activate(GtkApplication* app);

    void scrollPageSidebarToCurrent();

    void update();

    void updateFloatingPageSpinners();

    void ensureLaserFadeTimer();

    void zoom(double value, double x, double y, bool previewRaster = false);

    bool applyCanvasScroll(double dx, double dy, bool discrete);

    void startScrollMomentum(double vx, double vy);

    void cancelScrollMomentum();

    void finishZoomPreview();

    static bool mouseEvent(GtkEventController* controller);

    std::optional<prototype::Point> surfacePoint(double x, double y) const;

    prototype::Point scrollAnchor(GtkEventController* controller) const;

    prototype::CanvasImage displayedImage(size_t index) const;

    std::optional<size_t> imageHandleAt(const prototype::CanvasImage& image, const prototype::Point& point) const;

    bool beginImageGesture(Device device, double x, double y);

    void updateImageGesture(double x, double y);

    void finishImageGesture();

    LatexTarget latexTargetAt(int page, const prototype::Point& point, bool includeText = true) const;

    prototype::Point textTopLeft(const prototype::TextItem& item) const;

    void openLatexEditorAt(double x, double y, bool existingLatexOnly = false);

    void insertImageFromPath(const std::string& path);

    void chooseImage();

    void scheduleSelectionEdgePan(double x, double y);

    void stopSelectionEdgePan();

    void resetInput(bool interrupted = false);

    void playObjectAt(double screenX, double screenY);

    void syncAudioControls();

    void trackPointerPosition(double x, double y);

    void setCanvasCursor(const char* name);

    void updateCanvasCursor(double x, double y);

    void refreshCanvasCursor();

    void begin(Device device, double x, double y, std::optional<double> pressure = std::nullopt, bool eraserEnd = false,
               uint32_t timeMs = 0);

    void motion(Device device, double x, double y, std::optional<double> pressure = std::nullopt, uint32_t timeMs = 0);

    void finish(Device device, std::optional<double> releasePressure = std::nullopt,
                std::optional<prototype::Point> releasePoint = std::nullopt);

    static std::optional<double> pressure(GtkGestureStylus* stylus);

    void consumeBacklog(std::span<const GdkTimeCoord> samples);

    void stylusMotion(GtkGestureStylus* gesture, double x, double y);

    struct PanState {
        double x = 0;
        double y = 0;
        bool enabled = false;
    };

    void connectInput();

    void addGesture(GtkGesture* gesture, std::optional<Device> inkOwner = std::nullopt);

    PageRasterRequest canvasRenderRequest(int page, double scale) const;

    double canvasLodScale(int page, int deviceScale) const;

    double canvasMediumDetailScale(int page, int deviceScale) const;

    double canvasCacheScale(int page, int deviceScale, double zoomMultiplier, int maxDimension) const;

    PageRasterRequest canvasMediumDetailRequest(int page) const;

    PageRasterRequest canvasLodRequest(int page) const;

    PageRasterRequest canvasPreloadRequest(int page) const;

    void applyCanvasCacheLimits();

    bool canvasUsesMediumDetail() const;

    bool canvasOverviewMode(size_t visiblePageCount) const;

    bool canvasOverviewMode() const;

    int canvasDetailPage(const std::map<int, double>& visiblePages) const;

    double canvasRenderScale(int page, int deviceScale) const;

    double canvasDetailRenderScale(int page, int deviceScale, bool overview) const;

    bool waitForCanvasRaster(int page, double scale, int timeoutMilliseconds = 6000);

    size_t totalCacheBytes() const;

    void draw(cairo_t* cr, int width, int height);

    void capture(const std::string& name, GtkWidget* target = nullptr, bool captureTargetDirectly = false);

    void waitForNextPaint();

    void waitForNextWindowFrame();

    static void require(bool condition, const char* message);

    void checkInputParity(double x, double y);

    void smokeShiftStrokeConstraints(size_t expectedInkCount);

    void smokeEraserShiftConstraints(size_t expectedInkCount);

    void smokeDrawingTools();

    gboolean stepCrashHelper();

    gboolean stepRecoveryVerify();

    gboolean smokeLayers();

    gboolean smokeSelectionModes();

    void smokeDragPdfPage(Tool tool);

    gboolean smokePdfTextToolbar();

    gboolean smokePdfTextSelection();

    gboolean smokePreferencesVerify();

    gboolean smokeImages();

    gboolean smokeSettingsVerify();

    gboolean smokeLaserPointers();

    gboolean smokePlayObject();

    gboolean smokeLinks();

    void verifyTextClipboard();

    gboolean smoke();

    static GtkWidget* responseButton(GtkWidget* widget, const char* label);

    static GtkWidget* findWidgetByData(GtkWidget* widget, const char* key, const char* value);

    bool dialogHasResponseLabel(const char* response, const char* expectedLabel) const;

    void answerDialog(const char* response);

    bool cancelPrivateChooser();

    gboolean smokeDocuments();

    prototype::ApplicationOptions options;
    std::unique_ptr<prototype::LuaPluginRuntime> pluginRuntime;
    std::vector<std::shared_ptr<QuickScreenshotContext>> quickScreenshotOperations;
    bool quickScreenshotVerifyDone = false;
    std::string quickScreenshotVerifyError;
    std::filesystem::path preferencesConfigDir;
    prototype::SettingsImportResult importedConfig;
    int effectiveAutosaveSeconds = prototype::behavior::autosaveTimeoutSeconds;
    std::string settingsVerifyMode;
    int settingsVerifyStep = 0;
    std::string preferencesVerifyMode;
    int preferencesVerifyStep = 0;
    std::string layerVerifyMode;
    int layerVerifyStep = -1;
    size_t layerVerifyInitialInk = 0;
    size_t layerVerifyDuplicateInkStart = 0;
    std::string imageVerifyMode;
    std::string imageVerifySource;
    int imageVerifyStep = -1;
    std::string selectionVerifyMode;
    int selectionVerifyStep = -1;
    std::string pdfTextSelectionVerifyMode;
    std::string linkVerifyMode;
    int linkVerifyStep = -1;
    std::string openedLinkForTest;
    std::string linkOutputPath;
    bool linkSaveComplete = false;
    std::string laserVerifyMode;
    int laserVerifyStep = -1;
    size_t laserInitialInkCount = 0;
    size_t laserInitialRevision = 0;
    bool laserInitialUndo = false;
    std::string laserOutputPath;
    bool laserSaveComplete = false;
    std::string playObjectVerifyMode;
    int playObjectVerifyStep = -1;
    std::string playObjectSaveOutput;
    bool playObjectSaveComplete = false;
    int pdfTextSelectionVerifyStep = -1;
    std::string pdfClipboardExpected;
    std::string pdfClipboardReadText;
    std::string pdfClipboardReadError;
    std::string pdfTextSelectionOutput;
    bool pdfClipboardReadComplete = false;
    bool pdfTextSelectionSaveComplete = false;
    double selectionVerifyTextX = 0;
    prototype::CanvasImageTransform imageVerifyExpected;
    double imageVerifyMultiDeltaX = 0;
    double imageVerifyMultiDeltaY = 0;
    std::shared_ptr<prototype::bridge::DocumentSession> nativeSession;
    std::string sourcePath;
    std::string savedCopyPath;
    std::string exportedPdfPath;
    std::string saveTarget;
    std::string saveVersion;
    bool savedNormally = false;
    std::string busyText;
    std::optional<size_t> savedRevision = 0;
    std::optional<size_t> autosavedRevision = 0;
    bool recoveredChanges = false;
    bool checkedStartupRecovery = false;
    bool autosaving = false;
    int autosaveErrors = 0;
    std::string activeAutosavePath;
    std::function<void()> deferredAction;
    guint autosaveSource = 0;
    guint laserFadeSource = 0;
    guint selectionEdgePanSource = 0;
    guint zoomPreviewSource = 0;
    bool zoomPreview = false;
    int autosaveHeartbeats = 0;
    int autosaveWaits = 0;
    std::string smokeAutosaveBaseVersion;
    guint crashHelperSource = 0;
    int crashHelperStep = 0;
    int crashHelperTicks = 0;
    std::string crashMarkerPath;
    guint recoveryVerifySource = 0;
    int recoveryVerifyStep = 0;
    int recoveryVerifyTicks = 0;
    std::string recoveryActionSpec;
    bool busy = false;
    bool closed = false;
    bool choosingFile = false;
    int ioErrors = 0;
    AdwAlertDialog* messageDialog = nullptr;
    GtkWidget* gotoPageSpin = nullptr;
    GtkWidget* layerRenameEntry = nullptr;
    int layerRenamePage = -1;
    size_t layerRenameId = 0;
    std::function<void()> afterConfirmation;
    GtkWidget* heading = nullptr;
    GtkWidget* openButton = nullptr;
    GtkWidget* searchButton = nullptr;
    GtkWidget* saveButton = nullptr;
    GtkWidget* saveAsButton = nullptr;
    GtkWidget* exportPdfButton = nullptr;
    GtkWidget* prevPageButton = nullptr;
    GtkWidget* nextPageButton = nullptr;
    GtkWidget* pageButton = nullptr;
    GtkWidget* preferencesButton = nullptr;
    GtkWidget* pageLayoutMenuButton = nullptr;
    GtkWidget* pluginsMenuButton = nullptr;
    GtkWidget* insertPageButton = nullptr;
    GtkWidget* deletePageButton = nullptr;
    int documentSmokeStep = -1;
    int documentWaits = 0;
    gint64 lastDocumentCompletion = 0;
    int chooserWaits = 0;
    guint heartbeatSource = 0;
    int busyHeartbeats = 0;
    bool smokeContinued = false;
    std::string smokeSaveChoice;
    std::string smokeExportChoice;
    std::string smokeExternalVersion;
    CanvasState state;
    prototype::CanvasInput input{state};
    prototype::ScrollAxisLock scrollAxisLock;
    prototype::ScrollMomentum scrollMomentum;
    guint scrollMomentumTick = 0;
    bool scrollGestureAllowsMomentum = false;
    Gtk4SearchController searchController;
    std::optional<size_t> selectedImageIndex;
    std::optional<ImageGesture> imageGesture;
    std::optional<Device> textSelectionOwner;
    std::optional<prototype::Point> selectionEdgePanPointer;
    std::optional<prototype::Point> pointerPosition;
    std::string canvasCursorName;
    std::optional<prototype::Point> pendingStylusTextTap;
    std::optional<SmartSelectPdfGesture> smartSelectPdfGesture;
    bool textDragHandled = false;
    std::optional<PdfTextSelectionState> pdfTextSelection;
    std::optional<double> pdfActionsObservedZoom;
    PopplerDocument* document = nullptr;
    PopplerDocument* pdfTextDocument = nullptr;
    std::string pdfTextDocumentPath;
    std::vector<PopplerDocument*> layerDocuments;
    std::vector<PopplerDocument*> audioLayerDocuments;
    std::vector<prototype::AudioTarget> audioTargets;
    int pageCount = 1;
    int lastPreloadPage = -1;
    int canvasPreloadDirection = 1;
    guint canvasPreloadSource = 0;
    std::map<int, double> canvasPreloadedPages;
    bool syncingPage = false;
    bool syncingFloatingLayers = false;
    GtkWidget* window = nullptr;
    GdkPaintable* windowPaintable = nullptr;
    GdkFrameClock* paintClock = nullptr;
    int paints = 0;
    int lastSmokePaint = 0;
    int paintWaits = 0;
    double selectionVerifyEdgePanOffsetX = 0;
    double selectionVerifyEdgePanScreenX = 0;
    GtkWidget* contentView = nullptr;
    GtkWidget* headerBarWidget = nullptr;
    GtkWidget* toolbarWidget = nullptr;
    GtkWidget* fitPageButton = nullptr;
    GtkWidget* canvas = nullptr;
    GtkWidget* canvasOverlay = nullptr;
    std::unique_ptr<prototype::TextEditorOverlay> textEditor;
    GtkWidget* textButton = nullptr;
    GtkWidget* linkButton = nullptr;
    GtkWidget* playObjectButton = nullptr;
    GtkWidget* floatingToolboxToolButton = nullptr;
    GtkWidget* floatingToolbox = nullptr;
    GtkWidget* floatingToolboxRowsContainer = nullptr;
    size_t floatingToolboxStaticToolButtonCount = 0;
    std::vector<std::pair<Tool, GtkWidget*>> floatingToolboxToolButtons;
    std::vector<GtkWidget*> floatingZoomButtons;
    prototype::Point floatingToolboxAnchor{};
    guint floatingToolboxHideSource = 0;
    guint floatingToolboxSmokeSource = 0;
    bool floatingToolboxSmokeReady = true;
    guint floatingToolboxDismissSmokeSource = 0;
    bool floatingToolboxDismissSmokeReady = true;
    bool presentationMode = false;
    bool fullscreenModeActive = false;
    bool windowFullscreen = false;
    bool applyingViewMode = false;
    int presentationFitPage = -1;
    std::optional<bool> presentationFullscreenOverride;
    guint floatingPageSpinTimeout = 0;
    GtkSpinButton* pendingFloatingPageSpin = nullptr;
    std::vector<std::pair<GtkSpinButton*, GtkLabel*>> floatingPageSpinners;
    guint floatingLayerRefreshSource = 0;
    std::vector<FloatingLayerMenuWidgets> floatingLayerMenus;
    std::array<GtkWidget*, 2> laserButtons{};
    GtkWidget* audioControls = nullptr;
    GtkWidget* audioBackButton = nullptr;
    GtkWidget* audioPauseButton = nullptr;
    GtkWidget* audioForwardButton = nullptr;
    GtkWidget* audioStopButton = nullptr;
    std::unique_ptr<prototype::AudioPlayback> audioPlayback;
    GtkWidget* imageButton = nullptr;
    GtkWidget* latexButton = nullptr;
    std::shared_ptr<prototype::LatexEditor> latexEditor;
    std::filesystem::path latexTemplatePath;
    GtkWidget* linkInfoPopover = nullptr;
    GtkWidget* linkInfoLabel = nullptr;
    std::optional<size_t> linkInfoIndex;
    GtkTextView* linkTextInput = nullptr;
    GtkEntry* linkUrlInput = nullptr;
    GtkDropDown* linkPrefixDropdown = nullptr;
    GtkEntry* linkFontEntry = nullptr;
    GtkSpinButton* linkFontSize = nullptr;
    GtkDropDown* linkAlignmentDropdown = nullptr;
    std::optional<size_t> editingLinkIndex;
    int linkEditorPage = -1;
    prototype::Point linkEditorPosition{};
    GtkWidget* textFontEntry = nullptr;
    GtkWidget* textFontSize = nullptr;
    GtkWidget* textFormatBox = nullptr;
    std::array<GtkWidget*, 6> textFormatButtons{};
    GtkGestureClick* textClick = nullptr;
    GtkGestureClick* stylusButtonClick = nullptr;
    GtkWidget* pageLabel = nullptr;
    GtkWidget* zoomLabel = nullptr;
    GtkWidget* undoButton = nullptr;
    GtkWidget* redoButton = nullptr;
    GtkWidget* sidebarButton = nullptr;
    GtkWidget* colorBox = nullptr;
    GtkWidget* colorButton = nullptr;
    GtkWidget* paletteButton = nullptr;
    GtkWidget* palettePopover = nullptr;
    GtkWidget* paletteContent = nullptr;
    GtkWidget* paletteRailBox = nullptr;
    GtkWidget* mainToolRailWidget = nullptr;
    GtkWidget* mainToolGroup = nullptr;
    GtkWidget* moreToolsButton = nullptr;
    GtkWidget* moreToolsGroup = nullptr;
    prototype::ButtonToolAction activeStylusAction = prototype::ButtonToolAction::None;
    guint activeStylusButton = 0;
    std::vector<GtkWidget*> paletteSwatchButtons;
    GtkWidget* eraserModeDropdown = nullptr;
    GtkWidget* sizeDropdown = nullptr;
    GtkWidget* lineStyleDropdown = nullptr;
    GtkWidget* widthButton = nullptr;
    GtkWidget* drawingOptionsExpander = nullptr;
    GtkWidget* drawingModeDropdown = nullptr;
    std::vector<GtkDropDown*> floatingDrawingModeSelectors;
    std::vector<GtkToggleButton*> floatingFillToggles;
    std::vector<GtkButton*> floatingFillOpacityButtons;
    bool selectionFillEnabled = false;
    AdwDialog* fillOpacityDialog = nullptr;
    GtkRange* fillOpacityScale = nullptr;
    GtkDrawingArea* fillOpacityPreview = nullptr;
    GtkButton* fillOpacityCancelButton = nullptr;
    GtkButton* fillOpacityApplyButton = nullptr;
    Tool fillOpacityTarget = Tool::Pen;
    GtkWidget* averagingDropdown = nullptr;
    GtkWidget* preprocessorDropdown = nullptr;
    std::array<GtkWidget*, 5> toolButtons{};
    GtkWidget* selectionLabel = nullptr;
    GtkWidget* selectionModeDropdown = nullptr;
    GtkWidget* selectionActions = nullptr;
    GtkPopover* pdfActionsPopover = nullptr;
    GtkWidget* pdfCopyButton = nullptr;
    GtkWidget* pdfHighlightButton = nullptr;
    GtkWidget* pdfUnderlineButton = nullptr;
    GtkWidget* pdfStrikethroughButton = nullptr;
    GtkWidget* pdfSelectionStyleButton = nullptr;
    GtkWidget* pdfHighlightColorButton = nullptr;
    GtkWidget* pdfHighlightPaletteButton = nullptr;
    std::array<GtkWidget*, 6> pdfHighlightPresetButtons{};
    GtkWidget* pdfMarkerOpacityButton = nullptr;
    GtkWidget* pdfMarkerOpacityScale = nullptr;
    GtkWidget* pdfTextColorButton = nullptr;
    std::array<double, 3> pdfTextColor{0, 0, 0};
    int pdfTextMarkerOpacity = 60;
    Tool selectionTool = Tool::SelectRectangle;
    GCancellable* clipboardCancellation = nullptr;
    guint clipboardTimer = 0;
    AdwOverlaySplitView* splitView = nullptr;
    PreferencesDialog preferencesDialog;
    GtkWindow* pluginManagerWindow = nullptr;
    GtkWidget* pluginManagerList = nullptr;
    GtkWidget* pluginManagerApplyButton = nullptr;
    std::map<std::string, GtkWidget*> pluginManagerSwitches;
    GtkListView* pageList = nullptr;
    std::vector<GtkWidget*> thumbnailWidgets;
    PageRasterRenderer thumbnailRenderer;
    PageRasterRenderer canvasRenderer;
    InkRasterCache inkRasterCache;
    size_t sidebarStartupReadyDraws = 0;
    bool sidebarCanvasRenderSuppressed = false;
    GtkWidget* layerRows = nullptr;
    GtkWidget* layerAddButton = nullptr;
    GtkWidget* layerDeleteButton = nullptr;
    GtkWidget* layerMergeButton = nullptr;
    GtkWidget* layerMenuButton = nullptr;
    GtkWidget* layerPopover = nullptr;
    GtkWidget* layerRenameButton = nullptr;
    GtkWidget* layerRenameBackgroundButton = nullptr;
    GtkWidget* layerDuplicateButton = nullptr;
    GtkWidget* layerMoveUpButton = nullptr;
    GtkWidget* layerMoveDownButton = nullptr;
    GtkWidget* layerShowAllButton = nullptr;
    GtkWidget* layerHideAllButton = nullptr;
    int layerPanelPage = -1;
    std::optional<prototype::PageLayers> layerPanelSnapshot;
    prototype::Brush penBrush;
    prototype::Brush latexBrush{Tool::Latex, {0, 0, 0}, 1};
    prototype::Brush highlightBrush{
            Tool::Highlighter, prototype::behavior::defaultHighlighterColor,
            prototype::behavior::highlighterWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    prototype::Brush eraserBrush{
            Tool::Eraser, prototype::behavior::whiteoutColor,
            prototype::behavior::eraserWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    prototype::Brush laserPenBrush{
            Tool::LaserPointerPen, prototype::behavior::laserPointerColor,
            prototype::behavior::laserPenWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    prototype::Brush laserHighlighterBrush{
            Tool::LaserPointerHighlighter, prototype::behavior::laserPointerColor,
            prototype::behavior::laserHighlighterWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    bool syncingBrush = false;
    bool syncingTools = false;
    GtkGesture* mouse = nullptr;
    GtkGesture* stylus = nullptr;
    GtkEventController* pointerMotion = nullptr;
    GtkEventControllerScroll* scrollController = nullptr;
    std::vector<GtkGesture*> panGestures;
    GtkGesture* pinch = nullptr;
    GtkEventController* keys = nullptr;
    double dragX = 0;
    double dragY = 0;
    double pinchX = 0;
    double pinchY = 0;
    double pinchZoom = 1;
    bool fitted = false;
    bool fitMode = true;
    bool failed = false;
    int lastWidth = 0;
    int lastHeight = 0;
    int frames = 0;
    size_t baselineFullRasters = 0;
    int smokeStep = 0;
    size_t smokeCacheBytes = 0;
    double smokeCursorAspectRatioBeforeTextEditor = 0;
    int smokeTextEditorInitialWidth = 0;
    int snapshotWaits = 0;
    double smokeTransformWidth = 0;
    size_t smokeClipboardCount = 0;
    size_t smokeSelectionIndex = 0;
    int smokeWaits = 0;
};

}  // namespace prototype::gtk4
