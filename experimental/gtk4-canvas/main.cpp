#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

#include "AudioPlayback.h"
#include "AudioTarget.h"
#include "BeamerPresentation.h"
#include "BehaviorDefaults.h"
#include "CanvasImageActions.h"
#include "CanvasInput.h"
#include "CanvasState.h"
#include "DocumentAutosave.h"
#include "DocumentInk.h"
#include "DocumentJob.h"
#include "DocumentSession.h"
#include "ImageRenderer.h"
#include "InkRenderer.h"
#include "LatexEditor.h"
#include "LuaPluginRuntime.h"
#include "PaletteImport.h"
#include "PdfTextSelection.h"
#include "PreferencesStore.h"
#include "SettingsImport.h"
#include "StrokeClipboard.h"
#include "StylusSamples.h"
#include "TextEditorOverlay.h"
#include "TextRenderer.h"

using prototype::CanvasState;
using prototype::Device;
using prototype::Ink;
using prototype::Tool;

namespace {

struct SnapshotPending {};

struct Options {
    std::string pdf;
    std::string nativeDocument;
    std::string worker;
    std::string configDirectory;
    std::string pluginDirectory;
    std::string smokeDirectory;
    int autosaveSeconds = prototype::behavior::autosaveTimeoutSeconds;
    std::optional<int> autosaveSecondsOverride;
    bool dark = false;
    bool explicitDark = false;
    bool disableAudio = false;
    bool verifyPlugins = false;
    bool verifyBeamer = false;
    bool verifyLayerActions = false;
    bool verifyLatex = false;
    int width = 1100;
    int height = 820;
};

struct RecoveryInfo {
    std::string saveTarget;
    std::string saveVersion;
    std::string autosavePath;
};

std::vector<prototype::bridge::PageSizePacket> currentPageSizes(const CanvasState& state) {
    std::vector<prototype::bridge::PageSizePacket> sizes;
    sizes.reserve(static_cast<size_t>(state.pageCount()));
    for (int page = 0; page < state.pageCount(); ++page) {
        const auto size = state.pageSize(page);
        sizes.push_back({size.width, size.height});
    }
    return sizes;
}

struct SurfaceDeleter {
    void operator()(cairo_surface_t* surface) const { cairo_surface_destroy(surface); }
};

struct PageCacheEntry {
    int pageIndex = -1;
    size_t originLayer = prototype::newLayerOrigin;
    double scale = 0;
    bool audioMode = false;
    uint64_t stamp = 0;
    std::unique_ptr<cairo_surface_t, SurfaceDeleter> surface;
};

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

prototype::PdfTextSelectionStyle pdfSelectionStyleForTool(Tool tool) {
    return tool == Tool::SelectPdfTextRect ? prototype::PdfTextSelectionStyle::Rectangle :
                                             prototype::PdfTextSelectionStyle::Linear;
}

std::string encodeImagesForClipboard(const std::vector<prototype::CanvasImage>& images) {
    if (images.empty()) {
        throw std::runtime_error("No images are selected");
    }
    for (const auto& image: images) {
        if (!prototype::validCanvasImage(image) ||
            (image.kind == prototype::CanvasImageKind::Latex && !image.pdfDocument && !image.surface) ||
            (image.kind == prototype::CanvasImageKind::Raster && !image.surface)) {
            throw std::runtime_error("The selected image cannot be copied");
        }
    }

    auto bounds = prototype::canvasImageBounds(images.front());
    double pixelsPerPoint = 1.0;
    for (const auto& image: images) {
        for (const auto& point: prototype::canvasImageCorners(image)) {
            bounds.x0 = std::min(bounds.x0, point.x);
            bounds.y0 = std::min(bounds.y0, point.y);
            bounds.x1 = std::max(bounds.x1, point.x);
            bounds.y1 = std::max(bounds.y1, point.y);
        }
        const auto& transform = image.transform;
        const double scaleX = std::hypot(transform.xx, transform.yx);
        const double scaleY = std::hypot(transform.xy, transform.yy);
        const double smallestScale = std::min(scaleX, scaleY);
        if (smallestScale > 1e-10) {
            pixelsPerPoint = std::max(pixelsPerPoint, 1.0 / smallestScale);
        }
    }

    const double boundsWidth = bounds.x1 - bounds.x0;
    const double boundsHeight = bounds.y1 - bounds.y0;
    if (!std::isfinite(boundsWidth) || !std::isfinite(boundsHeight)) {
        throw std::runtime_error("The selected images have invalid clipboard dimensions");
    }
    double width = std::ceil(boundsWidth * pixelsPerPoint);
    double height = std::ceil(boundsHeight * pixelsPerPoint);
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1) {
        throw std::runtime_error("The selected images have invalid clipboard dimensions");
    }
    if (!std::isfinite(width * height) || width * height > static_cast<double>(prototype::maxCanvasImagePixels)) {
        const double scaleFactor = std::sqrt(static_cast<double>(prototype::maxCanvasImagePixels)) /
                                   std::sqrt(width) / std::sqrt(height);
        if (!std::isfinite(scaleFactor) || scaleFactor <= 0) {
            throw std::runtime_error("The selected images have invalid clipboard dimensions");
        }
        pixelsPerPoint *= scaleFactor;
        width = std::max(1.0, std::floor(boundsWidth * pixelsPerPoint));
        height = std::max(1.0, std::floor(boundsHeight * pixelsPerPoint));
        while (width * height > static_cast<double>(prototype::maxCanvasImagePixels)) {
            if (width >= height) {
                --width;
            } else {
                --height;
            }
        }
    }
    if (width > std::numeric_limits<int>::max() || height > std::numeric_limits<int>::max()) {
        throw std::runtime_error("The selected images have invalid clipboard dimensions");
    }
    const auto pixelWidth = static_cast<int>(width);
    const auto pixelHeight = static_cast<int>(height);
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixelWidth, pixelHeight);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        const std::string message = cairo_status_to_string(cairo_surface_status(surface));
        cairo_surface_destroy(surface);
        throw std::runtime_error("Cannot render the images for the clipboard: " + message);
    }

    auto* context = cairo_create(surface);
    cairo_set_operator(context, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(context, 0, 0, 0, 0);
    cairo_paint(context);
    cairo_set_operator(context, CAIRO_OPERATOR_OVER);
    cairo_translate(context, -bounds.x0, -bounds.y0);
    cairo_scale(context, pixelsPerPoint, pixelsPerPoint);
    for (const auto& image: images) {
        prototype::drawCanvasImage(context, image);
    }
    cairo_destroy(context);

    std::string png;
    const auto write = [](void* data, const unsigned char* bytes, unsigned int length) {
        auto& output = *static_cast<std::string*>(data);
        if (length > prototype::maxCanvasImageBytes - output.size()) {
            return CAIRO_STATUS_WRITE_ERROR;
        }
        output.append(reinterpret_cast<const char*>(bytes), length);
        return CAIRO_STATUS_SUCCESS;
    };
    const auto status = cairo_surface_write_to_png_stream(surface, write, &png);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS || png.empty()) {
        throw std::runtime_error("Cannot encode the selected images as PNG");
    }
    return png;
}

std::string encodeImageForClipboard(const prototype::CanvasImage& image) {
    return encodeImagesForClipboard({image});
}

class Prototype {
public:
    explicit Prototype(Options options): options(std::move(options)) {
        preferencesConfigDir = !this->options.configDirectory.empty() ?
                                       std::filesystem::path(this->options.configDirectory) :
                                       (configRoot() / "xournalpp");
        importedConfig = prototype::importSettingsFromConfigDir(preferencesConfigDir);
        if (!importedConfig.warning.empty()) {
            std::cerr << "xournalpp-gtk4-canvas: " << importedConfig.warning << '\n';
        }
        const auto preferenceLoad = prototype::loadGtk4Preferences(preferencesConfigDir, importedConfig.settings);
        if (!preferenceLoad.warning.empty()) {
            std::cerr << "xournalpp-gtk4-canvas: " << preferenceLoad.warning << '\n';
        }
        prototype::applyImportedSettingsToCanvas(state, importedConfig.settings);
        state.backgroundChangedCallback = [this](int page) { refreshNativeDocumentRevision(page); };
        input.setStabilizerConfig(importedConfig.settings.stabilizer);
        penBrush = {Tool::Pen, importedConfig.settings.penColor, importedConfig.settings.penWidth};
        latexBrush = {Tool::Latex, importedConfig.settings.latexColor, importedConfig.settings.penWidth};
        latexTemplatePath = importedConfig.settings.latexTemplatePath;
        if (latexTemplatePath.empty()) {
#ifdef GTK4_CANVAS_RESOURCE_DIR
            latexTemplatePath = std::filesystem::path(GTK4_CANVAS_RESOURCE_DIR) / "default_template.tex";
#endif
        }
        highlightBrush = {Tool::Highlighter, importedConfig.settings.highlighterColor,
                          importedConfig.settings.highlighterWidth};
        laserPenBrush = {Tool::LaserPointerPen, importedConfig.settings.laserPenColor,
                         importedConfig.settings.laserPenWidth};
        laserHighlighterBrush = {Tool::LaserPointerHighlighter, importedConfig.settings.laserHighlighterColor,
                                 importedConfig.settings.laserHighlighterWidth};
        eraserBrush = {Tool::Eraser, prototype::behavior::whiteoutColor, importedConfig.settings.eraserWidth};
        state.brush = importedConfig.settings.activeTool == Tool::Highlighter ?
                              highlightBrush :
                      importedConfig.settings.activeTool == Tool::Eraser ? eraserBrush :
                      importedConfig.settings.activeTool == Tool::LaserPointerPen ? laserPenBrush :
                      importedConfig.settings.activeTool == Tool::LaserPointerHighlighter ? laserHighlighterBrush :
                      importedConfig.settings.activeTool == Tool::Latex ? latexBrush :
                                                                                           penBrush;
        effectiveAutosaveSeconds =
                this->options.autosaveSecondsOverride.value_or(importedConfig.settings.effectiveAutosaveSeconds());

        sourcePath = this->options.nativeDocument.empty() ? this->options.pdf : this->options.nativeDocument;
        if (!this->options.pdf.empty() && this->options.worker.empty()) {
            auto [loadedDoc, sizes] = openPreviewPdf(this->options.pdf);
            document = loadedDoc;
            pageCount = static_cast<int>(sizes.size());
            state.setPages(std::move(sizes));
            state.selectActivePage(0);
        } else {
            state.setPages({{595, 842}});
            state.selectActivePage(0);
        }
        initializePlugins();
    }

    ~Prototype() {
        for (const auto& operation: quickScreenshotOperations) {
            operation->self = nullptr;
            if (operation->process) {
                g_subprocess_force_exit(operation->process);
                g_clear_object(&operation->process);
            }
            removeQuickScreenshotDirectory(*operation);
        }
        quickScreenshotOperations.clear();
        for (guint* source: {&heartbeatSource, &autosaveSource, &crashHelperSource, &recoveryVerifySource,
                            &selectionEdgePanSource}) {
            if (*source) {
                g_source_remove(*source);
                *source = 0;
            }
        }
        if (laserFadeSource) {
            g_source_remove(laserFadeSource);
            laserFadeSource = 0;
        }
        if (floatingToolboxHideSource) {
            g_source_remove(floatingToolboxHideSource);
            floatingToolboxHideSource = 0;
        }
        if (floatingToolboxSmokeSource) {
            g_source_remove(floatingToolboxSmokeSource);
            floatingToolboxSmokeSource = 0;
        }
        if (floatingToolboxDismissSmokeSource) {
            g_source_remove(floatingToolboxDismissSmokeSource);
            floatingToolboxDismissSmokeSource = 0;
        }
        if (closed) {
            cleanupActiveAutosave();
        }
        if (pluginManagerWindow) {
            gtk_window_destroy(pluginManagerWindow);
            g_clear_object(&pluginManagerWindow);
        }
        g_clear_object(&preferencesDialog);
        if (latexEditor) {
            auto editor = std::move(latexEditor);
            editor->cancel();
        }
        audioPlayback.reset();
        if (paintClock) {
            g_signal_handlers_disconnect_by_data(paintClock, this);
            g_clear_object(&paintClock);
        }
        g_clear_object(&windowPaintable);
        if (document) {
            g_object_unref(document);
        }
        g_clear_object(&pdfTextDocument);
        releaseLayerPreviewPdfs(layerDocuments);
        releaseLayerPreviewPdfs(audioLayerDocuments);
    }

    int run() {
        auto* app = adw_application_new("com.github.xournalpp.Gtk4CanvasPrototype", G_APPLICATION_NON_UNIQUE);
        g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer data) {
                             static_cast<Prototype*>(data)->activate(app);
                         }),
                         this);
        const auto result = g_application_run(G_APPLICATION(app), 0, nullptr);
        g_object_unref(app);
        return failed ? 1 : result;
    }

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

    static void fail(GError* error) {
        const std::string message = error ? error->message : "Unknown PDF error";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }

    static std::pair<PopplerDocument*, std::vector<prototype::PageSize>> openPreviewPdf(const std::string& path) {
        GError* error = nullptr;
        auto* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
        if (!uri) {
            fail(error);
        }
        auto* candidate = poppler_document_new_from_file(uri, nullptr, &error);
        g_free(uri);
        if (!candidate) {
            fail(error);
        }
        const int count = poppler_document_get_n_pages(candidate);
        if (count <= 0) {
            g_object_unref(candidate);
            throw std::runtime_error("Document has no pages");
        }
        std::vector<prototype::PageSize> sizes;
        sizes.reserve(static_cast<size_t>(count));
        try {
            for (int i = 0; i < count; ++i) {
                auto* pdfPage = poppler_document_get_page(candidate, i);
                if (!pdfPage) {
                    throw std::runtime_error("Cannot open rendered PDF page");
                }
                double w = 595;
                double h = 842;
                poppler_page_get_size(pdfPage, &w, &h);
                g_object_unref(pdfPage);
                sizes.push_back({w, h});
            }
        } catch (...) {
            g_object_unref(candidate);
            throw;
        }
        return {candidate, std::move(sizes)};
    }

    static std::vector<PopplerDocument*> openLayerPreviewPdfs(
            const std::vector<std::filesystem::path>& paths, int expectedPageCount) {
        std::vector<PopplerDocument*> documents;
        documents.reserve(paths.size());
        try {
            for (const auto& path: paths) {
                auto [document, sizes] = openPreviewPdf(path.string());
                if (static_cast<int>(sizes.size()) != expectedPageCount) {
                    g_object_unref(document);
                    throw std::runtime_error("A native layer preview has the wrong page count");
                }
                documents.push_back(document);
            }
        } catch (...) {
            for (auto* document: documents) {
                g_object_unref(document);
            }
            throw;
        }
        return documents;
    }

    static std::vector<PopplerDocument*> openAudioLayerPreviewPdfs(
            const std::vector<std::filesystem::path>& paths, int expectedPageCount) {
        return openLayerPreviewPdfs(paths, expectedPageCount);
    }

    static void releaseLayerPreviewPdfs(std::vector<PopplerDocument*>& documents) {
        for (auto* document: documents) {
            g_object_unref(document);
        }
        documents.clear();
    }

    bool modified() const { return savedRevision != state.revision(); }

    static std::filesystem::path cacheRoot() {
        const char* dir = g_get_user_cache_dir();
        return dir ? std::filesystem::path(dir) : std::filesystem::path{};
    }

    static std::filesystem::path configRoot() {
        const char* dir = g_get_user_config_dir();
        return dir ? std::filesystem::path(dir) : std::filesystem::path{};
    }

    std::string currentAutosaveTarget() const {
        return prototype::bridge::autosavePathForTarget(saveTarget, cacheRoot(), static_cast<long long>(::getpid()))
                .string();
    }

    void cleanupActiveAutosave() {
        if (!activeAutosavePath.empty()) {
            prototype::bridge::removeAutosaveFiles(activeAutosavePath);
            activeAutosavePath.clear();
        }
    }

    void scheduleAutosave(guint seconds) {
        if (autosaveSource) {
            g_source_remove(autosaveSource);
            autosaveSource = 0;
        }
        if (!options.worker.empty() && seconds > 0) {
            autosaveSource = g_timeout_add_seconds(
                    seconds,
                    +[](gpointer data) -> gboolean {
                        static_cast<Prototype*>(data)->startAutosave();
                        return G_SOURCE_CONTINUE;
                    },
                    this);
        }
    }

    void waitForAutosave(std::function<void()> action) {
        resetInput(true);
        gtk_widget_set_sensitive(contentView, false);
        deferredAction = std::move(action);
        updateTitle();
    }

    void startAutosave() {
        if (!nativeSession || options.worker.empty() || closed || busy || autosaving || choosingFile || messageDialog) {
            return;
        }
        finishTextEditing();
        if (!modified()) {
            if (!activeAutosavePath.empty()) {
                cleanupActiveAutosave();
                updateTitle();
            }
            autosavedRevision = state.revision();
            return;
        }
        const auto target = currentAutosaveTarget();
        std::error_code statusError;
        if (autosavedRevision == state.revision() && activeAutosavePath == target &&
            std::filesystem::is_regular_file(std::filesystem::symlink_status(target, statusError)) && !statusError) {
            return;
        }
        const auto revision = state.revision();
        const auto packets = prototype::bridge::transferInk(state.committedInk());
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto previous = activeAutosavePath;
        autosaving = true;
        updateTitle();
        auto* application = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
        g_application_hold(application);
        prototype::bridge::documentJob(
                [session, worker, target, packets, texts, images, layers, pageSizes] {
                    session->autosave(worker, target, packets, texts, images, layers, pageSizes);
                },
                [this, application, session, revision, target, previous](const std::string& error) {
                    autosaving = false;
                    if (error.empty() && nativeSession == session && !closed) {
                        if (!previous.empty() && previous != target) {
                            prototype::bridge::removeAutosaveFiles(previous);
                        }
                        if (modified()) {
                            activeAutosavePath = target;
                            autosavedRevision = revision;
                        } else {
                            prototype::bridge::removeAutosaveFiles(target);
                            activeAutosavePath.clear();
                            autosavedRevision = state.revision();
                        }
                    } else if (!error.empty()) {
                        ++autosaveErrors;
                    }
                    if (deferredAction) {
                        gtk_widget_set_sensitive(contentView, true);
                        auto next = std::move(deferredAction);
                        deferredAction = nullptr;
                        if (!closed) {
                            next();
                        }
                    }
                    if (!closed) {
                        update();
                    }
                    g_application_release(application);
                });
    }

    void closeWindow() {
        cleanupActiveAutosave();
        closed = true;
        gtk_window_destroy(GTK_WINDOW(window));
    }

    void updateTitle() {
        if (!heading) {
            return;
        }
        const auto name = sourcePath.empty() ? std::string("Blank notebook") :
                                               std::filesystem::path(sourcePath).filename().string();
        adw_window_title_set_title(ADW_WINDOW_TITLE(heading), (modified() ? "• " + name : name).c_str());
        std::string status;
        if (busy) {
            status = busyText;
        } else if (autosaving) {
            status = "Autosaving…";
        } else if (modified()) {
            if (recoveredChanges && state.revision() == 0) {
                status = "Recovered unsaved changes";
            } else if (autosavedRevision == state.revision() && !activeAutosavePath.empty()) {
                status = "Unsaved changes · Autosaved";
            } else {
                status = "Unsaved changes";
            }
        } else if (!savedCopyPath.empty()) {
            status = "Copy saved: " + std::filesystem::path(savedCopyPath).filename().string();
        } else if (savedNormally) {
            status = "Saved";
        } else if (nativeSession) {
            status = "Document loaded";
        } else {
            status = "Temporary notebook";
        }
        adw_window_title_set_subtitle(ADW_WINDOW_TITLE(heading), status.c_str());
    }

    void showAlert(const char* title, const std::string& message) {
        ++ioErrors;
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, message.c_str()));
        adw_alert_dialog_add_response(messageDialog, "close", "Close");
        adw_alert_dialog_set_close_response(messageDialog, "close");
        g_signal_connect(messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
            static_cast<Prototype*>(data)->messageDialog = nullptr;
        }), this);
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
    }

    void showError(const std::string& message) { showAlert("Document operation failed", message); }

    static GdkRGBA unpackArgbColor(std::uint32_t color) {
        return {
                static_cast<float>((color >> 16U) & 0xffU) / 255.0F,
                static_cast<float>((color >> 8U) & 0xffU) / 255.0F,
                static_cast<float>(color & 0xffU) / 255.0F,
                static_cast<float>((color >> 24U) & 0xffU) / 255.0F,
        };
    }

    static std::uint32_t packArgbColor(const GdkRGBA& color) {
        const auto channel = [](float value) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(static_cast<double>(value), 0.0, 1.0) * 255.0));
        };
        return (channel(color.alpha) << 24U) | (channel(color.red) << 16U) | (channel(color.green) << 8U) |
               channel(color.blue);
    }

    enum class PreferenceKey : guint {
        Theme = 1,
        Sidebar,
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

    GtkWidget* preferenceCombo(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                               const char* const* options, guint selected, PreferenceKey key) {
        auto* row = ADW_COMBO_ROW(adw_combo_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        auto* model = gtk_string_list_new(options);
        adw_combo_row_set_model(row, G_LIST_MODEL(model));
        g_object_unref(model);
        adw_combo_row_set_selected(row, selected);
        g_object_set_data(G_OBJECT(row), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
        g_signal_connect(row, "notify::selected",
                         G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto key = static_cast<PreferenceKey>(
                                     GPOINTER_TO_UINT(g_object_get_data(object, "gtk4-preference-key")));
                             self->changePreference(key, GTK_WIDGET(object));
                         }),
                         this);
        preferenceControls[static_cast<size_t>(key)] = GTK_WIDGET(row);
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return GTK_WIDGET(row);
    }

    GtkWidget* preferenceSwitch(AdwPreferencesGroup* group, const char* title, const char* subtitle, bool active,
                                PreferenceKey key) {
        auto* row = ADW_SWITCH_ROW(adw_switch_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        adw_switch_row_set_active(row, active);
        g_object_set_data(G_OBJECT(row), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
        g_signal_connect(row, "notify::active",
                         G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto key = static_cast<PreferenceKey>(
                                     GPOINTER_TO_UINT(g_object_get_data(object, "gtk4-preference-key")));
                             self->changePreference(key, GTK_WIDGET(object));
                         }),
                         this);
        preferenceControls[static_cast<size_t>(key)] = GTK_WIDGET(row);
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return GTK_WIDGET(row);
    }

    GtkWidget* preferenceSpin(AdwPreferencesGroup* group, const char* title, const char* subtitle, double value,
                              double minimum, double maximum, double step, guint digits, const char* suffix,
                              PreferenceKey key) {
        auto* row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(row, subtitle);
        auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        auto* spin = gtk_spin_button_new_with_range(minimum, maximum, step);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), true);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
        gtk_widget_set_size_request(spin, 96, -1);
        gtk_box_append(GTK_BOX(box), spin);
        if (suffix && *suffix) {
            gtk_box_append(GTK_BOX(box), gtk_label_new(suffix));
        }
        adw_action_row_add_suffix(row, box);
        adw_action_row_set_activatable_widget(row, spin);
        g_object_set_data(G_OBJECT(spin), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
        g_signal_connect(spin, "value-changed",
                         G_CALLBACK(+[](GtkSpinButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto key = static_cast<PreferenceKey>(g_object_get_data(
                                     G_OBJECT(button), "gtk4-preference-key") ?
                                     GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "gtk4-preference-key")) : 0);
                             self->changePreference(key, GTK_WIDGET(button));
                         }),
                         this);
        preferenceControls[static_cast<size_t>(key)] = GTK_WIDGET(spin);
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return GTK_WIDGET(spin);
    }

    GtkWidget* preferenceColor(AdwPreferencesGroup* group, const char* title, const char* subtitle, std::uint32_t color,
                               PreferenceKey key) {
        auto* row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(row, subtitle);
        auto* dialog = gtk_color_dialog_new();
        gtk_color_dialog_set_with_alpha(dialog, true);
        auto* button = gtk_color_dialog_button_new(dialog);
        gtk_widget_set_size_request(button, 48, 36);
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
        const auto rgba = unpackArgbColor(color);
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
        adw_action_row_add_suffix(row, button);
        adw_action_row_set_activatable_widget(row, button);
        g_object_set_data(G_OBJECT(button), "gtk4-preference-key", GUINT_TO_POINTER(static_cast<guint>(key)));
        g_signal_connect(button, "notify::rgba", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto key = static_cast<PreferenceKey>(
                                     GPOINTER_TO_UINT(g_object_get_data(object, "gtk4-preference-key")));
                             self->changePreference(key, GTK_WIDGET(object));
                         }),
                         this);
        preferenceControls[static_cast<size_t>(key)] = button;
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return GTK_WIDGET(button);
    }

    GtkWidget* preferenceTextEntry(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                                   const std::string& value, PreferenceKey key) {
        auto* row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(row, subtitle);
        auto* entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), value.c_str());
        gtk_editable_set_width_chars(GTK_EDITABLE(entry), 30);
        gtk_widget_set_hexpand(entry, true);
        gtk_widget_set_size_request(entry, 240, -1);
        adw_action_row_add_suffix(row, entry);
        adw_action_row_set_activatable_widget(row, entry);
        preferenceControls[static_cast<size_t>(key)] = entry;
        if (key == PreferenceKey::LatexDefaultText || key == PreferenceKey::LatexGeneratorCommand ||
            key == PreferenceKey::LatexExternalEditorCommand ||
            key == PreferenceKey::LatexTemporaryFileExtension) {
            g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
                                 static_cast<Prototype*>(data)->markLatexTextPreferencesPending();
                             }), this);
        }
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return entry;
    }

    GtkWidget* preferenceTemplatePath(AdwPreferencesGroup* group, const std::string& value) {
        auto* row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "TeX template");
        adw_action_row_set_subtitle(row, "Leave empty to use the bundled template");
        auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        auto* entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), value.c_str());
        gtk_editable_set_width_chars(GTK_EDITABLE(entry), 28);
        gtk_widget_set_size_request(entry, 240, -1);
        gtk_widget_set_hexpand(entry, true);
        latexTemplatePathEntry = entry;
        g_signal_connect(entry, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
                             static_cast<Prototype*>(data)->markLatexTextPreferencesPending();
                         }), this);
        gtk_box_append(GTK_BOX(box), entry);
        auto* browse = gtk_button_new_with_label("Browse…");
        gtk_widget_set_valign(browse, GTK_ALIGN_CENTER);
        gtk_accessible_update_property(GTK_ACCESSIBLE(browse), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Choose a LaTeX template file", -1);
        g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->chooseLatexTemplate();
                         }), this);
        gtk_box_append(GTK_BOX(box), browse);
        adw_action_row_add_suffix(row, box);
        adw_action_row_set_activatable_widget(row, entry);
        preferenceControls[static_cast<size_t>(PreferenceKey::LatexTemplatePath)] = entry;
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return entry;
    }

    GtkWidget* preferenceFont(AdwPreferencesGroup* group, const char* title, const char* subtitle,
                              const std::string& value) {
        auto* row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(row, subtitle);
        auto* dialog = gtk_font_dialog_new();
        gtk_font_dialog_set_font_map(dialog, pango_cairo_font_map_get_default());
        auto* button = GTK_FONT_DIALOG_BUTTON(gtk_font_dialog_button_new(dialog));
        gtk_font_dialog_button_set_level(button, GTK_FONT_LEVEL_FONT);
        PangoFontDescription* description = pango_font_description_from_string(value.c_str());
        gtk_font_dialog_button_set_font_desc(button, description);
        pango_font_description_free(description);
        adw_action_row_add_suffix(row, GTK_WIDGET(button));
        adw_action_row_set_activatable_widget(row, GTK_WIDGET(button));
        g_signal_connect(button, "notify::font-desc", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                             static_cast<Prototype*>(data)->changePreference(PreferenceKey::LatexEditorFont,
                                                                             GTK_WIDGET(object));
                         }), this);
        preferenceControls[static_cast<size_t>(PreferenceKey::LatexEditorFont)] = GTK_WIDGET(button);
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return GTK_WIDGET(button);
    }

    GtkWidget* preferenceLatexTheme(AdwPreferencesGroup* group, const std::string& selectedTheme) {
        auto* row = ADW_COMBO_ROW(adw_combo_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Syntax color scheme");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "Choose colors for LaTeX source highlighting");
        auto* model = gtk_string_list_new(nullptr);
        gtk_string_list_append(model, "System default");
        latexSourceViewThemeIds.clear();
        latexSourceViewThemeIds.emplace_back();
        guint selectedIndex = 0;
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
        auto* manager = gtk_source_style_scheme_manager_get_default();
        const char* const* ids = gtk_source_style_scheme_manager_get_scheme_ids(manager);
        if (ids) {
            for (const char* const* id = ids; *id; ++id) {
                auto* scheme = gtk_source_style_scheme_manager_get_scheme(manager, *id);
                if (!scheme) {
                    continue;
                }
                const auto index = static_cast<guint>(latexSourceViewThemeIds.size());
                latexSourceViewThemeIds.emplace_back(*id);
                gtk_string_list_append(model, gtk_source_style_scheme_get_name(scheme));
                if (selectedTheme == *id) {
                    selectedIndex = index;
                }
            }
        }
#else
        (void)selectedTheme;
        gtk_widget_set_sensitive(GTK_WIDGET(row), false);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), "GtkSourceView was unavailable when this build was made");
#endif
        adw_combo_row_set_model(row, G_LIST_MODEL(model));
        g_object_unref(model);
        adw_combo_row_set_selected(row, selectedIndex);
        g_object_set_data(G_OBJECT(row), "gtk4-preference-key",
                          GUINT_TO_POINTER(static_cast<guint>(PreferenceKey::LatexSourceViewTheme)));
        g_signal_connect(row, "notify::selected",
                         G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->changePreference(PreferenceKey::LatexSourceViewTheme, GTK_WIDGET(object));
                         }), this);
        preferenceControls[static_cast<size_t>(PreferenceKey::LatexSourceViewTheme)] = GTK_WIDGET(row);
        adw_preferences_group_add(group, GTK_WIDGET(row));
        return GTK_WIDGET(row);
    }

    bool applyFloatingToolboxRows() {
        static constexpr std::array<PreferenceKey, 4> keys = {
                PreferenceKey::FloatingToolboxRow1,
                PreferenceKey::FloatingToolboxRow2,
                PreferenceKey::FloatingToolboxRow3,
                PreferenceKey::FloatingToolboxRow4,
        };
        const auto previousRows = importedConfig.settings.floatingToolboxRows;
        prototype::FloatingToolboxRows rows;
        for (std::size_t index = 0; index < keys.size(); ++index) {
            auto* entry = preferenceControls[static_cast<size_t>(keys[index])];
            const char* raw = gtk_editable_get_text(GTK_EDITABLE(entry));
            auto parsed = prototype::parseFloatingToolboxRow(raw ? raw : "");
            if (!parsed) {
                gtk_widget_add_css_class(entry, "error");
                showAlert("Invalid floating toolbox row",
                          "Use comma-separated toolbar action names. Empty rows are allowed.");
                return false;
            }
            gtk_widget_remove_css_class(entry, "error");
            rows[index] = std::move(*parsed);
        }

        importedConfig.settings.floatingToolboxRows = std::move(rows);
        std::string error;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
            importedConfig.settings.floatingToolboxRows = previousRows;
            showAlert("Could not save toolbar preferences", error);
            return false;
        }
        rebuildFloatingToolboxRows();
        return true;
    }

    bool applyMainToolRail() {
        auto* entry = preferenceControls[static_cast<size_t>(PreferenceKey::MainToolRail)];
        const char* raw = gtk_editable_get_text(GTK_EDITABLE(entry));
        const auto parsed = prototype::parseMainToolRail(raw ? raw : "");
        if (!parsed) {
            gtk_widget_add_css_class(entry, "error");
            showAlert("Invalid main tool rail layout",
                      "Use comma-separated action names and keep SMART_SELECT in the rail.");
            return false;
        }
        gtk_widget_remove_css_class(entry, "error");

        const auto previous = importedConfig.settings.mainToolRail;
        importedConfig.settings.mainToolRail = *parsed;
        std::string error;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
            importedConfig.settings.mainToolRail = previous;
            showAlert("Could not save toolbar preferences", error);
            return false;
        }
        rebuildMainToolRail();
        return true;
    }

    void changePreference(PreferenceKey key, GtkWidget* control) {
        if (syncingPreferenceControls) {
            return;
        }
        auto& settings = importedConfig.settings;
        switch (key) {
            case PreferenceKey::Theme:
                switch (adw_combo_row_get_selected(ADW_COMBO_ROW(control))) {
                    case 1:
                        settings.themeVariant = prototype::ThemeVariant::ForceLight;
                        break;
                    case 2:
                        settings.themeVariant = prototype::ThemeVariant::ForceDark;
                        break;
                    default:
                        settings.themeVariant = prototype::ThemeVariant::UseSystem;
                        break;
                }
                break;
            case PreferenceKey::Sidebar:
                settings.showSidebar = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::PressureSensitivity:
                settings.pressureSensitivity = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::PositionHighlight:
                settings.highlightPosition = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::CursorHighlightColor:
                settings.cursorHighlightColor =
                        packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(control)));
                break;
            case PreferenceKey::CursorHighlightRadius:
                settings.cursorHighlightRadius = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::CursorHighlightBorderColor:
                settings.cursorHighlightBorderColor =
                        packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(control)));
                break;
            case PreferenceKey::CursorHighlightBorderWidth:
                settings.cursorHighlightBorderWidth = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::LatexAutoCheckDependencies:
                settings.latexAutoCheckDependencies = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::LatexSourceViewTheme: {
                const auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(control));
                if (selected >= latexSourceViewThemeIds.size()) {
                    return;
                }
                settings.latexSourceViewThemeId = latexSourceViewThemeIds[selected];
                break;
            }
            case PreferenceKey::LatexSourceViewAutoIndent:
                settings.latexSourceViewAutoIndent = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::LatexSourceViewSyntaxHighlight:
                settings.latexSourceViewSyntaxHighlight = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::LatexSourceViewShowLineNumbers:
                settings.latexSourceViewShowLineNumbers = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::LatexEditorFont: {
                PangoFontDescription* description =
                        gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(control));
                if (description) {
                    gchar* value = pango_font_description_to_string(description);
                    settings.latexEditorFont = value ? value : "Monospace 12";
                    g_free(value);
                    pango_font_description_free(description);
                }
                break;
            }
            case PreferenceKey::LatexUseCustomEditorFont:
                settings.latexUseCustomEditorFont = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                updateLatexPreferenceSensitivity();
                break;
            case PreferenceKey::LatexEditorWordWrap:
                settings.latexEditorWordWrap = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::LatexUseExternalEditor:
                settings.latexUseExternalEditor = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                updateLatexPreferenceSensitivity();
                break;
            case PreferenceKey::LatexExternalEditorAutoConfirm:
                settings.latexExternalEditorAutoConfirm = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::MinimumPressure:
                settings.minimumPressure = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::PressureMultiplier:
                settings.pressureMultiplier = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::StylusButtonOne:
                settings.stylusButtonOne = static_cast<prototype::ButtonToolAction>(
                        adw_combo_row_get_selected(ADW_COMBO_ROW(control)));
                break;
            case PreferenceKey::StylusButtonTwo:
                settings.stylusButtonTwo = static_cast<prototype::ButtonToolAction>(
                        adw_combo_row_get_selected(ADW_COMBO_ROW(control)));
                break;
            case PreferenceKey::AutosaveEnabled:
                settings.autosaveEnabled = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::AutosaveTimeout:
                settings.autosaveTimeoutMinutes = static_cast<int>(
                        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control)));
                break;
            case PreferenceKey::ZoomStep:
                settings.zoomStep = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control)) / 100.0;
                break;
            case PreferenceKey::ScrollZoomStep:
                settings.scrollZoomStep = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control)) / 100.0;
                break;
            case PreferenceKey::EdgePanSpeed:
                settings.edgePanSpeed = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::EdgePanMaxMultiplier:
                settings.edgePanMaxMultiplier = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::RotationSnapTolerance:
                settings.snapRotationTolerance = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::SnapRotation:
                settings.snapRotation = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::SnapGrid:
                settings.snapGrid = adw_switch_row_get_active(ADW_SWITCH_ROW(control));
                break;
            case PreferenceKey::SnapGridSize:
                settings.snapGridSize = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::SnapGridTolerance:
                settings.snapGridTolerance = gtk_spin_button_get_value(GTK_SPIN_BUTTON(control));
                break;
            case PreferenceKey::FloatingToolboxRow1:
            case PreferenceKey::FloatingToolboxRow2:
            case PreferenceKey::FloatingToolboxRow3:
            case PreferenceKey::FloatingToolboxRow4:
            case PreferenceKey::MainToolRail:
            case PreferenceKey::LatexDefaultText:
            case PreferenceKey::LatexTemplatePath:
            case PreferenceKey::LatexGeneratorCommand:
            case PreferenceKey::LatexExternalEditorCommand:
            case PreferenceKey::LatexTemporaryFileExtension:
            case PreferenceKey::Count:
                return;
        }

        prototype::applyImportedSettingsToCanvas(state, settings);
        input.setStabilizerConfig(settings.stabilizer);
        effectiveAutosaveSeconds = settings.effectiveAutosaveSeconds();
        scheduleAutosave(static_cast<guint>(std::max(0, effectiveAutosaveSeconds)));
        if (splitView) {
            adw_overlay_split_view_set_show_sidebar(splitView, settings.showSidebar);
        }
        if (!options.explicitDark) {
            const auto scheme = settings.themeVariant == prototype::ThemeVariant::ForceLight ? ADW_COLOR_SCHEME_FORCE_LIGHT :
                                settings.themeVariant == prototype::ThemeVariant::ForceDark ? ADW_COLOR_SCHEME_FORCE_DARK :
                                                                                              ADW_COLOR_SCHEME_DEFAULT;
            adw_style_manager_set_color_scheme(adw_style_manager_get_default(), scheme);
        }
        std::string error;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, settings, error)) {
            showAlert("Could not save preferences", error);
        }
        update();
    }

    void chooseLatexTemplate() {
        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Choose a TeX template");
        const char* currentPath = gtk_editable_get_text(GTK_EDITABLE(latexTemplatePathEntry));
        if (currentPath && *currentPath) {
            GFile* currentFile = g_file_new_for_path(currentPath);
            gtk_file_dialog_set_initial_file(dialog, currentFile);
            g_object_unref(currentFile);
        }

        using DialogContext = std::pair<Prototype*, GtkFileDialog*>;
        auto* context = new DialogContext(this, dialog);
        gtk_file_dialog_open(dialog, GTK_WINDOW(window), nullptr,
                             +[](GObject*, GAsyncResult* result, gpointer data) {
                                 std::unique_ptr<DialogContext> context(static_cast<DialogContext*>(data));
                                 GError* error = nullptr;
                                 GFile* file = gtk_file_dialog_open_finish(context->second, result, &error);
                                 if (file) {
                                     gchar* path = g_file_get_path(file);
                                     if (path) {
                                         gtk_editable_set_text(GTK_EDITABLE(context->first->latexTemplatePathEntry),
                                                               path);
                                         g_free(path);
                                     } else {
                                         context->first->showAlert(
                                                 "Template must be a local file",
                                                 "Choose a template stored on this device.");
                                     }
                                     g_object_unref(file);
                                 }
                                 g_clear_error(&error);
                                 g_object_unref(context->second);
                             },
                             context);
    }

    bool applyLatexPreferences() {
        auto entryText = [this](PreferenceKey key) {
            auto* entry = preferenceControls[static_cast<size_t>(key)];
            const char* text = entry ? gtk_editable_get_text(GTK_EDITABLE(entry)) : "";
            return std::string(text ? text : "");
        };
        const auto defaultText = entryText(PreferenceKey::LatexDefaultText);
        const auto templatePathText = entryText(PreferenceKey::LatexTemplatePath);
        const auto generatorCommand = entryText(PreferenceKey::LatexGeneratorCommand);
        const auto externalEditorCommand = entryText(PreferenceKey::LatexExternalEditorCommand);
        const auto temporaryFileExtension = entryText(PreferenceKey::LatexTemporaryFileExtension);
        if (defaultText.size() > prototype::maxCanvasLatexSourceBytes || templatePathText.size() > 4096 ||
            generatorCommand.size() > 16384 || externalEditorCommand.size() > 16384 ||
            temporaryFileExtension.size() > 64 || temporaryFileExtension.find('/') != std::string::npos ||
            temporaryFileExtension.find('\\') != std::string::npos ||
            std::any_of(temporaryFileExtension.begin(), temporaryFileExtension.end(),
                        [](unsigned char value) { return value < 0x20; })) {
            showAlert("LaTeX settings could not be applied",
                      "Check the formula size, command lengths, template path, and temporary file suffix.");
            return false;
        }

        auto& settings = importedConfig.settings;
        const auto oldDefaultText = settings.latexDefaultText;
        const auto oldTemplatePath = settings.latexTemplatePath;
        const auto oldGeneratorCommand = settings.latexGeneratorCommand;
        const auto oldExternalEditorCommand = settings.latexExternalEditorCommand;
        const auto oldTemporaryFileExtension = settings.latexTemporaryFileExtension;
        settings.latexDefaultText = defaultText;
        settings.latexTemplatePath = templatePathText.empty() ? std::filesystem::path{} :
                                                               prototype::preferences_detail::pathFromUtf8(
                                                                       templatePathText);
        settings.latexGeneratorCommand = generatorCommand;
        settings.latexExternalEditorCommand = externalEditorCommand;
        settings.latexTemporaryFileExtension = temporaryFileExtension;

        std::string error;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, settings, error)) {
            settings.latexDefaultText = oldDefaultText;
            settings.latexTemplatePath = oldTemplatePath;
            settings.latexGeneratorCommand = oldGeneratorCommand;
            settings.latexExternalEditorCommand = oldExternalEditorCommand;
            settings.latexTemporaryFileExtension = oldTemporaryFileExtension;
            showAlert("Could not save LaTeX preferences", error);
            return false;
        }

        latexTemplatePath = settings.latexTemplatePath;
        if (latexTemplatePath.empty()) {
#ifdef GTK4_CANVAS_RESOURCE_DIR
            latexTemplatePath = std::filesystem::path(GTK4_CANVAS_RESOURCE_DIR) / "default_template.tex";
#endif
        }
        updateLatexPreferenceSensitivity();
        if (latexSettingsApplyStatus) {
            gtk_label_set_text(GTK_LABEL(latexSettingsApplyStatus), "Saved");
            gtk_widget_add_css_class(latexSettingsApplyStatus, "success");
            gtk_widget_remove_css_class(latexSettingsApplyStatus, "error");
            gtk_widget_remove_css_class(latexSettingsApplyStatus, "dim-label");
        }
        return true;
    }

    void markLatexTextPreferencesPending() {
        if (!latexSettingsApplyStatus) {
            return;
        }
        gtk_label_set_text(GTK_LABEL(latexSettingsApplyStatus), "Unsaved changes");
        gtk_widget_add_css_class(latexSettingsApplyStatus, "dim-label");
        gtk_widget_remove_css_class(latexSettingsApplyStatus, "success");
        gtk_widget_remove_css_class(latexSettingsApplyStatus, "error");
    }

    void updateLatexPreferenceSensitivity() {
        const auto& settings = importedConfig.settings;
        const bool sourceEditorEnabled = !settings.latexUseExternalEditor;
        const auto setSensitive = [this](PreferenceKey key, bool sensitive) {
            if (auto* control = preferenceControls[static_cast<size_t>(key)]) {
                gtk_widget_set_sensitive(control, sensitive);
            }
        };
        setSensitive(PreferenceKey::LatexEditorFont,
                     settings.latexUseCustomEditorFont && sourceEditorEnabled);
        setSensitive(PreferenceKey::LatexExternalEditorAutoConfirm, settings.latexUseExternalEditor);
        setSensitive(PreferenceKey::LatexExternalEditorCommand, settings.latexUseExternalEditor);
        setSensitive(PreferenceKey::LatexTemporaryFileExtension, settings.latexUseExternalEditor);
        setSensitive(PreferenceKey::LatexSourceViewTheme, sourceEditorEnabled);
        setSensitive(PreferenceKey::LatexSourceViewAutoIndent, sourceEditorEnabled);
        setSensitive(PreferenceKey::LatexSourceViewSyntaxHighlight, sourceEditorEnabled);
        setSensitive(PreferenceKey::LatexSourceViewShowLineNumbers, sourceEditorEnabled);
        setSensitive(PreferenceKey::LatexEditorWordWrap, sourceEditorEnabled);
#ifndef GTK4_CANVAS_HAVE_SOURCEVIEW
        setSensitive(PreferenceKey::LatexSourceViewTheme, false);
        setSensitive(PreferenceKey::LatexSourceViewAutoIndent, false);
        setSensitive(PreferenceKey::LatexSourceViewSyntaxHighlight, false);
        setSensitive(PreferenceKey::LatexSourceViewShowLineNumbers, false);
#endif
    }

    void testLatexSetup() {
        if (!applyLatexPreferences()) {
            return;
        }
        if (latexTemplatePath.empty()) {
            showAlert("Choose a TeX template", "Select a template file before checking the LaTeX setup.");
            return;
        }
        gtk_widget_set_sensitive(latexSetupTestButton, false);
        gtk_label_set_text(GTK_LABEL(latexSetupTestStatus), "Compiling the sample formula…");
        gtk_widget_remove_css_class(latexSetupTestStatus, "success");
        gtk_widget_remove_css_class(latexSetupTestStatus, "error");
        const auto& settings = importedConfig.settings;
        prototype::LatexEditor::testConfiguration(
                latexTemplatePath, settings.latexGeneratorCommand, settings.latexDefaultText, latexBrush.color,
                [this](bool success, std::string message) {
                    gtk_widget_set_sensitive(latexSetupTestButton, true);
                    gtk_label_set_text(GTK_LABEL(latexSetupTestStatus), success ?
                                                                                     "LaTeX setup is ready" :
                                                                                     "LaTeX setup needs attention");
                    gtk_widget_add_css_class(latexSetupTestStatus, success ? "success" : "error");
                    showAlert(success ? "LaTeX setup works" : "LaTeX setup failed", message);
                });
    }

    void initializePlugins() {
        std::vector<std::filesystem::path> searchPaths;
        if (!options.pluginDirectory.empty()) {
            searchPaths.emplace_back(options.pluginDirectory);
        }
#ifdef GTK4_CANVAS_BUNDLED_PLUGIN_DIR
        searchPaths.emplace_back(GTK4_CANVAS_BUNDLED_PLUGIN_DIR);
#endif
        searchPaths.push_back(preferencesConfigDir / "plugins");

        pluginRuntime = std::make_unique<prototype::LuaPluginRuntime>();
        pluginRuntime->discover(searchPaths, importedConfig.settings.pluginsEnabled,
                                importedConfig.settings.pluginsDisabled);

        prototype::PluginRuntimeHost host;
        host.changeToolColor = [this](const auto& tool, std::uint32_t color, bool selection, std::string& error) {
            return changePluginToolColor(tool, color, selection, error);
        };
        host.changeActionState = [this](std::string_view action, const prototype::PluginActionValue& value,
                                        std::string& error) { return changePluginActionState(action, value, error); };
        host.activateAction = [this](std::string_view action, std::string& error) {
            return activatePluginAction(action, error);
        };
        host.changeCurrentPageBackground = [this](std::string_view format, std::string_view config,
                                                  std::string& error) {
            return changePluginPageBackground(format, config, error);
        };
        host.addEmptyPages = [this](bool force, std::string& error) { return addEmptyPagesFromPlugin(force, error); };
        host.transformSelectedImages = [this](std::int64_t action, std::string& error) {
            return transformSelectedImagesFromPlugin(action, error);
        };
        host.getStrokes = [this](std::string_view type, std::vector<prototype::PluginStroke>& strokes,
                                 std::string& error) { return getPluginStrokes(type, strokes, error); };
        host.addStrokes = [this](const std::vector<prototype::PluginStrokeInput>& strokes,
                                 prototype::PluginStrokeUndoPolicy policy,
                                 std::vector<std::uintptr_t>& references, std::string& error) {
            return addPluginStrokes(strokes, policy, references, error);
        };
        host.setPageSize = [this](double width, double height, bool relative, std::string& error) {
            return setPluginPageSize(width, height, relative, error);
        };
        host.setLayerVisibility = [this](bool visible, std::string& error) {
            return setPluginLayerVisibility(visible, error);
        };
        host.setCurrentLayerName = [this](std::string_view name, std::string& error) {
            return setPluginCurrentLayerName(name, error);
        };
        host.exportDocument = [this](const prototype::DocumentExportOptions& exportOptions, std::string& error) {
            return exportDocumentFromPlugin(exportOptions, error);
        };
        host.quickScreenshot = [this](std::string_view plugin, std::string& error) {
            return startQuickScreenshot(plugin, error);
        };
        host.documentFilename = [this] { return saveTarget; };
        host.openDialog = [this](std::string_view plugin, std::string_view message,
                                 const std::vector<prototype::PluginDialogButton>& buttons, bool isError,
                                 std::function<void(int)> callback, std::string& error) {
            return showPluginDialog(plugin, message, buttons, isError, std::move(callback), error);
        };
        host.reportError = [this](std::string_view plugin, std::string_view message) {
            if (window) {
                showAlert("Plugin error", std::string(plugin) + ": " + std::string(message));
            } else {
                std::cerr << "xournalpp-gtk4-canvas: plugin " << plugin << ": " << message << '\n';
            }
        };
        pluginRuntime->loadEnabled(std::move(host));
    }

    static void removeQuickScreenshotDirectory(const QuickScreenshotContext& context) {
        if (!context.directory.empty()) {
            std::error_code error;
            std::filesystem::remove_all(context.directory, error);
        }
    }

    void finishQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context) {
        if (context->process) {
            g_clear_object(&context->process);
        }
        if (context->self) {
            context->self->choosingFile = false;
            auto& operations = context->self->quickScreenshotOperations;
            operations.erase(std::remove(operations.begin(), operations.end(), context), operations.end());
            context->self = nullptr;
        }
        removeQuickScreenshotDirectory(*context);
    }

    void failQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context, std::string message) {
        if (context->self) {
            auto* self = context->self;
            if (self->options.verifyPlugins) {
                self->quickScreenshotVerifyError = std::move(message);
                self->quickScreenshotVerifyDone = true;
            } else if (self->window) {
                self->showAlert("Quick Screenshot failed", message);
            }
        }
        finishQuickScreenshot(context);
    }

    void cancelQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context) {
        if (context->self && context->self->options.verifyPlugins) {
            context->self->quickScreenshotVerifyError = "Region capture was cancelled";
            context->self->quickScreenshotVerifyDone = true;
        }
        finishQuickScreenshot(context);
    }

    bool launchQuickScreenshotProcess(const std::shared_ptr<QuickScreenshotContext>& context,
                                     const std::vector<std::string>& arguments, bool captureOutput,
                                     std::string& error) {
        std::vector<const gchar*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument: arguments) {
            argv.push_back(argument.c_str());
        }
        argv.push_back(nullptr);

        GError* spawnError = nullptr;
        const auto flags = captureOutput ?
                                   static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE) :
                                   static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                                                 G_SUBPROCESS_FLAGS_STDERR_SILENCE);
        context->process = g_subprocess_newv(argv.data(), flags, &spawnError);
        if (!context->process) {
            error = spawnError ? spawnError->message : "Could not start the screenshot utility";
            g_clear_error(&spawnError);
            return false;
        }

        auto* callbackContext = new std::shared_ptr<QuickScreenshotContext>(context);
        if (captureOutput) {
            g_subprocess_communicate_utf8_async(
                    context->process, nullptr, nullptr,
                    +[](GObject* source, GAsyncResult* result, gpointer data) {
                        std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                                static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                        const auto operation = *boxed;
                        gchar* output = nullptr;
                        gchar* standardError = nullptr;
                        GError* error = nullptr;
                        const bool completed = g_subprocess_communicate_utf8_finish(
                                G_SUBPROCESS(source), result, &output, &standardError, &error);
                        std::string geometry = output ? output : "";
                        g_strstrip(geometry.data());
                        g_free(output);
                        g_free(standardError);
                        g_clear_error(&error);
                        g_clear_object(&operation->process);

                        if (!operation->self) {
                            removeQuickScreenshotDirectory(*operation);
                            return;
                        }
                        if (!completed || geometry.empty()) {
                            operation->self->cancelQuickScreenshot(operation);
                            return;
                        }

                        gchar* grimPath = g_find_program_in_path("grim");
                        std::string launchError;
                        const bool launched = grimPath && operation->self->launchQuickScreenshotProcess(
                                                                 operation,
                                                                 {grimPath, "-g", geometry, operation->imagePath},
                                                                 false, launchError);
                        g_free(grimPath);
                        if (!launched) {
                            operation->self->failQuickScreenshot(
                                    operation, launchError.empty() ? "The grim screenshot utility is unavailable" :
                                                                         launchError);
                        }
                    },
                    callbackContext);
        } else {
            g_subprocess_wait_check_async(
                    context->process, nullptr,
                    +[](GObject* source, GAsyncResult* result, gpointer data) {
                        std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                                static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                        const auto operation = *boxed;
                        GError* error = nullptr;
                        const bool completed = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error);
                        std::error_code fileError;
                        const auto imageStatus = std::filesystem::symlink_status(operation->imagePath, fileError);
                        const bool imageExists = !fileError && std::filesystem::is_regular_file(imageStatus);
                        auto imageSize = imageExists ?
                                                 std::filesystem::file_size(operation->imagePath, fileError) :
                                                 0;
                        if (fileError) {
                            imageSize = 0;
                        }
                        g_clear_error(&error);
                        g_clear_object(&operation->process);

                        if (!operation->self) {
                            removeQuickScreenshotDirectory(*operation);
                            return;
                        }
                        if (!completed && !imageExists) {
                            operation->self->cancelQuickScreenshot(operation);
                            return;
                        }
                        if (!completed || imageSize == 0) {
                            operation->self->failQuickScreenshot(operation,
                                                                  "The screenshot utility did not create a PNG image");
                            return;
                        }
                        operation->self->quickScreenshotReady(operation);
                    },
                    callbackContext);
        }
        return true;
    }

    bool startQuickScreenshot(std::string_view plugin, std::string& error) {
        if (!window || closed || busy || choosingFile || messageDialog) {
            error = "Wait for the current dialog or document operation to finish";
            return false;
        }

        gchar* scrot = nullptr;
        gchar* maim = nullptr;
        gchar* xclip = nullptr;
        gchar* grim = nullptr;
        gchar* slurp = nullptr;
        bool wayland = g_getenv("WAYLAND_DISPLAY") != nullptr;
#ifdef GDK_WINDOWING_WAYLAND
        if (auto* display = gdk_display_get_default()) {
            wayland = GDK_IS_WAYLAND_DISPLAY(display);
        }
#endif
        if (options.verifyPlugins && g_getenv("XOURNALPP_TEST_QUICK_SCREENSHOT_X11")) {
            wayland = false;
        }
        if (wayland) {
            grim = g_find_program_in_path("grim");
            slurp = g_find_program_in_path("slurp");
        }
        if (!wayland || !grim || !slurp) {
            maim = g_find_program_in_path("maim");
            if (!wayland) {
                xclip = g_find_program_in_path("xclip");
            }
        }
        const bool clipboardCapture = !wayland && maim && xclip;
        if (!clipboardCapture && (!grim || !slurp)) {
            g_clear_pointer(&grim, g_free);
            g_clear_pointer(&slurp, g_free);
            scrot = g_find_program_in_path("scrot");
            if (!scrot && !maim) {
                maim = g_find_program_in_path("maim");
            }
        }
        if (!clipboardCapture && !grim && !scrot && !maim) {
            grim = g_find_program_in_path("grim");
            slurp = g_find_program_in_path("slurp");
        }
        if (!clipboardCapture && ((!grim || !slurp) && !scrot && !maim)) {
            g_free(grim);
            g_free(slurp);
            g_free(xclip);
            g_free(maim);
            error = "Quick Screenshot needs scrot or maim on X11, or grim and slurp on Wayland";
            return false;
        }

        GError* directoryError = nullptr;
        gchar* directory = g_dir_make_tmp("xournalpp-screenshot-XXXXXX", &directoryError);
        if (!directory) {
            error = directoryError ? directoryError->message : "Could not create temporary screenshot storage";
            g_clear_error(&directoryError);
            g_free(grim);
            g_free(slurp);
            g_free(scrot);
            g_free(maim);
            g_free(xclip);
            return false;
        }

        auto operation = std::make_shared<QuickScreenshotContext>();
        operation->self = this;
        operation->plugin = plugin;
        operation->directory = directory;
        operation->imagePath = (std::filesystem::path(directory) / "region.png").string();
        operation->copyToClipboard = clipboardCapture;
        g_free(directory);
        quickScreenshotOperations.push_back(operation);
        choosingFile = true;
        resetInput(true);

        bool launched = false;
        if (grim && slurp) {
            launched = launchQuickScreenshotProcess(operation, {slurp}, true, error);
        } else if (scrot) {
            launched = launchQuickScreenshotProcess(operation,
                                                    {scrot, "--overwrite", "--select", "--silent",
                                                     operation->imagePath},
                                                    false, error);
        } else if (maim) {
            launched = launchQuickScreenshotProcess(operation, {maim, "-s", operation->imagePath}, false, error);
        }
        g_free(grim);
        g_free(slurp);
        g_free(scrot);
        g_free(maim);
        g_free(xclip);
        if (!launched) {
            finishQuickScreenshot(operation);
            return false;
        }
        return true;
    }

    void quickScreenshotReady(const std::shared_ptr<QuickScreenshotContext>& context) {
        if (!context->self) {
            removeQuickScreenshotDirectory(*context);
            return;
        }
        auto* self = context->self;
        auto* capturedImage = cairo_image_surface_create_from_png(context->imagePath.c_str());
        const auto imageStatus = cairo_surface_status(capturedImage);
        cairo_surface_destroy(capturedImage);
        if (imageStatus != CAIRO_STATUS_SUCCESS) {
            self->failQuickScreenshot(context, "The screenshot utility did not produce a readable PNG image");
            return;
        }
        if (context->copyToClipboard) {
            gchar* contents = nullptr;
            gsize length = 0;
            GError* error = nullptr;
            if (!g_file_get_contents(context->imagePath.c_str(), &contents, &length, &error)) {
                const std::string message = error ? error->message : "Could not read the captured PNG image";
                g_clear_error(&error);
                self->failQuickScreenshot(context, message);
                return;
            }
            auto* bytes = g_bytes_new_take(contents, length);
            auto* provider = gdk_content_provider_new_for_bytes("image/png", bytes);
            g_bytes_unref(bytes);
            const bool copied = gdk_clipboard_set_content(gtk_widget_get_clipboard(self->canvas), provider);
            g_object_unref(provider);
            if (!copied) {
                self->failQuickScreenshot(context, "Could not place the screenshot on the clipboard");
                return;
            }
            if (self->options.verifyPlugins) {
                self->quickScreenshotVerifyError.clear();
                self->quickScreenshotVerifyDone = true;
            }
            self->finishQuickScreenshot(context);
            return;
        }
        if (self->options.verifyPlugins) {
            const auto destination = std::filesystem::path(self->options.smokeDirectory) / "quick-screenshot.png";
            auto* file = g_file_new_for_path(destination.c_str());
            self->copyQuickScreenshot(context, file);
            g_object_unref(file);
            return;
        }

        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Save Quick Screenshot");
        gtk_file_dialog_set_initial_name(dialog, "Untitled.png");
        auto* callbackContext = new std::shared_ptr<QuickScreenshotContext>(context);
        gtk_file_dialog_save(dialog, GTK_WINDOW(self->window), nullptr,
                             +[](GObject* source, GAsyncResult* result, gpointer data) {
                                 std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                                         static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                                 const auto operation = *boxed;
                                 if (!operation->self) {
                                     removeQuickScreenshotDirectory(*operation);
                                     return;
                                 }
                                 GError* error = nullptr;
                                 auto* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
                                 if (!file) {
                                     const bool cancelled = error &&
                                             (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
                                              g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
                                              g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED));
                                     const std::string message = error ? error->message : "Could not choose a file";
                                     g_clear_error(&error);
                                     if (cancelled) {
                                         operation->self->cancelQuickScreenshot(operation);
                                     } else {
                                         operation->self->failQuickScreenshot(operation, message);
                                     }
                                     return;
                                 }
                                 operation->self->copyQuickScreenshot(operation, file);
                                 g_object_unref(file);
                             },
                             callbackContext);
        g_object_unref(dialog);
    }

    void copyQuickScreenshot(const std::shared_ptr<QuickScreenshotContext>& context, GFile* destination) {
        auto* source = g_file_new_for_path(context->imagePath.c_str());
        auto* callbackContext = new std::shared_ptr<QuickScreenshotContext>(context);
        g_file_copy_async(source, destination, G_FILE_COPY_OVERWRITE, G_PRIORITY_DEFAULT, nullptr, nullptr, nullptr,
                          +[](GObject* sourceFile, GAsyncResult* result, gpointer data) {
                              std::unique_ptr<std::shared_ptr<QuickScreenshotContext>> boxed(
                                      static_cast<std::shared_ptr<QuickScreenshotContext>*>(data));
                              const auto operation = *boxed;
                              GError* error = nullptr;
                              const bool copied = g_file_copy_finish(G_FILE(sourceFile), result, &error);
                              const std::string message = error ? error->message : "Could not save the screenshot";
                              g_clear_error(&error);
                              if (!operation->self) {
                                  removeQuickScreenshotDirectory(*operation);
                                  return;
                              }
                              auto* self = operation->self;
                              if (self->options.verifyPlugins) {
                                  self->quickScreenshotVerifyError = copied ? "" : message;
                                  self->quickScreenshotVerifyDone = true;
                              } else if (!copied && self->window) {
                                  self->showAlert("Could not save Quick Screenshot", message);
                              }
                              self->finishQuickScreenshot(operation);
                          },
                          callbackContext);
        g_object_unref(source);
    }

    bool changePluginPageBackground(std::string_view format, std::string_view config, std::string& error) {
        if (format != "plain" && format != "graph") {
            error = "This GTK 4 host currently supports the plain and graph page backgrounds";
            return false;
        }
        if (!nativeSession || options.worker.empty()) {
            error = "Changing a page background requires the document worker";
            return false;
        }
        if (busy || choosingFile || closed) {
            error = "A document operation is already in progress";
            return false;
        }
        if (autosaving) {
            const std::string formatCopy(format);
            const std::string configCopy(config);
            waitForAutosave([this, formatCopy, configCopy] {
                std::string deferredError;
                if (!changePluginPageBackground(formatCopy, configCopy, deferredError)) {
                    showError(deferredError);
                }
            });
            return true;
        }
        finishTextEditing();
        resetInput(true);
        if (!state.changePageBackground(state.page, std::string(format), std::string(config))) {
            return true;
        }
        return true;
    }

    bool transformSelectedImagesFromPlugin(std::int64_t action, std::string& error) {
        if (action < static_cast<int>(prototype::CanvasImageAction::Invert) ||
            action > static_cast<int>(prototype::CanvasImageAction::SplitHorizontal)) {
            error = "Unknown ImageActions mode";
            return false;
        }
        if (!prototype::canvasImageActionsAvailable()) {
            error = "ImageActions requires libvips support in this GTK 4 build";
            return false;
        }
        if (busy || choosingFile || messageDialog || closed || autosaving) {
            error = "Wait for the current document operation to finish before transforming images";
            return false;
        }
        if (state.selectedImages.empty()) {
            return true;
        }

        std::vector<std::pair<size_t, std::vector<prototype::CanvasImage>>> replacements;
        replacements.reserve(state.selectedImages.size());
        const auto imageAction = static_cast<prototype::CanvasImageAction>(action);
        for (const auto index: state.selectedImages) {
            if (index >= state.images.size()) {
                error = "The selected image no longer exists";
                return false;
            }
            const auto& image = state.images[index];
            if (image.page < 0 || image.page >= state.pageCount()) {
                error = "The selected image belongs to an invalid page";
                return false;
            }
            const auto page = state.pageSize(image.page);
            std::vector<prototype::CanvasImage> outputs;
            if (!prototype::transformCanvasImage(image, imageAction, page.width, page.height, outputs, error)) {
                return false;
            }
            replacements.emplace_back(index, std::move(outputs));
        }
        if (!state.replaceImageGroup(std::move(replacements))) {
            error = "The selected images could not be replaced as one undoable action";
            return false;
        }
        update();
        return true;
    }

    void refreshNativeDocumentRevision(int changedPage) {
        if (!nativeSession || options.worker.empty() || closed || changedPage < 0 || changedPage >= state.pageCount()) {
            return;
        }
        if (autosaving) {
            waitForAutosave([this, changedPage] { refreshNativeDocumentRevision(changedPage); });
            return;
        }
        if (busy) {
            showError("Cannot update the page background while another document operation is running");
            return;
        }

        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto packets = prototype::bridge::transferInk(state.committedInk());
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Updating page background…",
                [session, worker, packets, texts, images, layers, pageSizes, staged] {
                    *staged = session->stageRevision(worker, packets, texts, images, layers, pageSizes);
                },
                [this, session, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<PopplerDocument*> candidateLayers;
                    std::vector<PopplerDocument*> candidateAudioLayers;
                    std::vector<prototype::PageSize> sizes;
                    prototype::bridge::LayerDocument layerState;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                        candidateLayers =
                                openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                        candidateAudioLayers = openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(),
                                                                         static_cast<int>(sizes.size()));
                        layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                    } catch (...) {
                        if (candidate) {
                            g_object_unref(candidate);
                        }
                        releaseLayerPreviewPdfs(candidateLayers);
                        releaseLayerPreviewPdfs(candidateAudioLayers);
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }

                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    releaseLayerPreviewPdfs(layerDocuments);
                    layerDocuments = std::move(candidateLayers);
                    releaseLayerPreviewPdfs(audioLayerDocuments);
                    audioLayerDocuments = std::move(candidateAudioLayers);
                    audioTargets = session->audioTargets;
                    state.setLayerDocument(layerState);
                    g_clear_object(&pdfTextDocument);
                    pdfTextDocumentPath.clear();
                    pageCache.clear();
                    layerPageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    updateTitle();
                });
    }

    bool showPluginDialog(std::string_view pluginName, std::string_view message,
                          const std::vector<prototype::PluginDialogButton>& buttons, bool isError,
                          std::function<void(int)> callback, std::string& error) {
        if (!window) {
            error = "The main window is not available";
            return false;
        }
        if (messageDialog) {
            error = "Another dialog is already open";
            return false;
        }

        const std::string title = (isError ? "Error in Xournal++ Plugin «" : "Xournal++ Plugin «") +
                                  std::string(pluginName) + "»";
        const std::string body(message);
        auto* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title.c_str(), body.c_str()));
        auto* context = new PluginDialogContext{this, {}, std::move(callback)};
        for (const auto& button: buttons) {
            const auto response = std::to_string(button.response);
            const auto responseId = "plugin-response-" + response;
            context->responses.emplace(responseId, button.response);
            adw_alert_dialog_add_response(dialog, responseId.c_str(), button.label.c_str());
        }
        constexpr const char* dismissResponse = "plugin-dismiss";
        adw_alert_dialog_set_close_response(dialog, dismissResponse);
        if (!buttons.empty()) {
            const auto firstResponse = "plugin-response-" + std::to_string(buttons.front().response);
            adw_alert_dialog_set_default_response(dialog, firstResponse.c_str());
        }
        g_signal_connect_data(
                dialog, "response",
                G_CALLBACK(+[](AdwAlertDialog* dialog, const char* response, gpointer data) {
                    auto* context = static_cast<PluginDialogContext*>(data);
                    if (context->prototype->messageDialog == dialog) {
                        context->prototype->messageDialog = nullptr;
                    }
                    const auto selected = context->responses.find(response ? response : "");
                    if (selected != context->responses.end() && context->callback) {
                        context->callback(selected->second);
                    }
                }),
                context,
                +[](gpointer data, GClosure*) { delete static_cast<PluginDialogContext*>(data); },
                static_cast<GConnectFlags>(0));
        messageDialog = dialog;
        adw_dialog_present(ADW_DIALOG(dialog), window);
        return true;
    }

    static std::optional<Tool> pluginToolFromName(std::string_view name) {
        std::string normalized;
        normalized.reserve(name.size());
        for (const unsigned char character: name) {
            if (character != '_' && character != '-') {
                normalized += static_cast<char>(std::tolower(character));
            }
        }
        if (normalized == "pen")
            return Tool::Pen;
        if (normalized == "highlighter")
            return Tool::Highlighter;
        if (normalized == "eraser")
            return Tool::Eraser;
        if (normalized == "text")
            return Tool::Text;
        if (normalized == "selectrect")
            return Tool::SelectRectangle;
        if (normalized == "selectregion")
            return Tool::SelectLasso;
        if (normalized == "selectobject")
            return Tool::SelectObject;
        if (normalized == "selectmultilayerrect")
            return Tool::SelectRectangle;
        if (normalized == "selectmultilayerregion")
            return Tool::SelectLasso;
        if (normalized == "selectpdftextlinear")
            return Tool::SelectPdfTextLinear;
        if (normalized == "selectpdftextrect")
            return Tool::SelectPdfTextRect;
        if (normalized == "smartselect")
            return Tool::SmartSelect;
        if (normalized == "hand")
            return Tool::Hand;
        if (normalized == "laserpointerpen")
            return Tool::LaserPointerPen;
        if (normalized == "laserpointerhighlighter")
            return Tool::LaserPointerHighlighter;
        if (normalized == "link")
            return Tool::Link;
        if (normalized == "playobject")
            return Tool::PlayObject;
        if (normalized == "latex")
            return Tool::Latex;
        if (normalized == "verticalspace")
            return Tool::VerticalSpace;
        if (normalized == "showfloatingtoolbox")
            return Tool::FloatingToolbox;
        return std::nullopt;
    }

    bool changePluginToolColor(const std::optional<std::string>& toolName, std::uint32_t color, bool selection,
                               std::string& error) {
        auto tool = toolName ? pluginToolFromName(*toolName) : std::optional<Tool>{state.brush.tool};
        if (!tool) {
            error = "Unknown tool name";
            return false;
        }
        if (*tool == Tool::Eraser || *tool == Tool::Hand || *tool == Tool::Link || *tool == Tool::PlayObject ||
            *tool == Tool::VerticalSpace || *tool == Tool::FloatingToolbox) {
            error = "The selected tool does not support color changes";
            return false;
        }

        const std::array<double, 3> rgb{static_cast<double>((color >> 16U) & 0xffU) / 255.0,
                                        static_cast<double>((color >> 8U) & 0xffU) / 255.0,
                                        static_cast<double>(color & 0xffU) / 255.0};
        Tool colorTool = *tool;
        if (prototype::isSelectionTool(colorTool)) {
            colorTool = prototype::isPdfTextSelectionTool(colorTool) ? Tool::Highlighter : Tool::Pen;
        }

        if (colorTool == Tool::Pen) {
            penBrush.color = rgb;
        } else if (colorTool == Tool::Highlighter) {
            highlightBrush.color = rgb;
        } else if (colorTool == Tool::LaserPointerPen) {
            laserPenBrush.color = rgb;
        } else if (colorTool == Tool::LaserPointerHighlighter) {
            laserHighlighterBrush.color = rgb;
        } else if (colorTool == Tool::Text) {
            state.textColor = rgb;
        } else if (colorTool == Tool::Latex) {
            latexBrush.color = rgb;
        } else {
            error = "The selected tool does not support color changes";
            return false;
        }

        if (state.brush.tool == *tool || state.brush.tool == colorTool ||
            (prototype::isSelectionTool(state.brush.tool) &&
             (state.brush.tool == *tool || colorTool == Tool::Pen || colorTool == Tool::Highlighter))) {
            state.brush.color = rgb;
        }
        if (selection && state.hasSelection()) {
            state.recolorSelection(rgb, packTextColor(rgb));
            thumbnails.clear();
        }
        input.cancel();
        if (colorButton && state.brush.tool != Tool::Eraser) {
            syncingBrush = true;
            const GdkRGBA rgba{static_cast<float>(state.brush.color[0]), static_cast<float>(state.brush.color[1]),
                               static_cast<float>(state.brush.color[2]), 1};
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
            syncingBrush = false;
        }
        update();
        return true;
    }

    bool getPluginStrokes(std::string_view type, std::vector<prototype::PluginStroke>& strokes, std::string& error) {
        if (type != "selection" && type != "layer" && type != "page" && type != "all") {
            error = "Unknown argument (" + std::string(type) + ") for getting selection";
            return false;
        }
        if (type == "selection" && !state.hasSelection()) {
            error = "There is no selection";
            return false;
        }

        if (type != "selection" && state.hasSelection()) {
            state.clearSelection();
            update();
        }

        const auto appendStroke = [this, &strokes](size_t index, bool includePage, bool includeLayer) {
            if (index >= state.ink.size()) {
                return;
            }
            const auto& ink = state.ink[index];
            prototype::PluginStroke stroke;
            stroke.x.reserve(ink.points.size());
            stroke.y.reserve(ink.points.size());
            if (ink.pressureSensitive) {
                stroke.pressure.reserve(ink.points.size());
            }
            for (const auto& point: ink.points) {
                stroke.x.push_back(point.x);
                stroke.y.push_back(point.y);
                if (ink.pressureSensitive) {
                    stroke.pressure.push_back(point.pressure);
                }
            }
            switch (ink.brush.tool) {
                case Tool::Highlighter:
                case Tool::LaserPointerHighlighter:
                    stroke.tool = "highlighter";
                    break;
                case Tool::Eraser:
                    stroke.tool = "eraser";
                    break;
                default:
                    stroke.tool = "pen";
                    break;
            }
            stroke.width = ink.brush.width;
            const auto channel = [](double value) {
                return static_cast<std::uint32_t>(std::clamp(std::lround(value * 255.0), 0L, 255L));
            };
            stroke.color = (channel(ink.brush.color[0]) << 16U) | (channel(ink.brush.color[1]) << 8U) |
                           channel(ink.brush.color[2]);
            stroke.fill = ink.fill;
            stroke.lineStyle = "plain";
            stroke.reference = reinterpret_cast<std::uintptr_t>(&ink);
            if (includePage) {
                stroke.page = static_cast<std::size_t>(ink.page + 1);
            }
            if (includeLayer) {
                stroke.layer = ink.layerId;
            }
            strokes.push_back(std::move(stroke));
        };

        if (type == "selection") {
            for (const auto index: state.selected) {
                appendStroke(index, false, false);
            }
            return true;
        }

        const auto appendLayer = [this, &appendStroke](int page, size_t layer, bool includePage,
                                                       bool includeLayer) {
            std::vector<size_t> indices;
            for (size_t index = 0; index < state.ink.size(); ++index) {
                const auto& ink = state.ink[index];
                if (ink.page == page && ink.layerId == layer) {
                    indices.push_back(index);
                }
            }
            std::stable_sort(indices.begin(), indices.end(), [this](size_t a, size_t b) {
                return state.ink[a].layerOrder < state.ink[b].layerOrder;
            });
            for (const auto index: indices) {
                appendStroke(index, includePage, includeLayer);
            }
        };

        if (type == "layer") {
            appendLayer(state.page, state.activeLayerId(state.page), false, false);
            return true;
        }
        const int firstPage = type == "all" ? 0 : state.page;
        const int lastPage = type == "all" ? state.pageCount() : state.page + 1;
        for (int page = firstPage; page < lastPage; ++page) {
            const auto& layers = state.layersForPage(page).layers;
            for (size_t layer = 1; layer <= layers.size(); ++layer) {
                appendLayer(page, layer, type == "all", true);
            }
        }
        return true;
    }

    bool addPluginStrokes(const std::vector<prototype::PluginStrokeInput>& inputs,
                          prototype::PluginStrokeUndoPolicy policy,
                          std::vector<std::uintptr_t>& references, std::string& error) {
        if (busy || choosingFile || messageDialog || closed || autosaving) {
            error = "Wait for the current document operation to finish before adding strokes";
            return false;
        }
        if (inputs.empty()) {
            return true;
        }

        std::vector<prototype::Ink> additions;
        additions.reserve(inputs.size());
        for (const auto& inputStroke: inputs) {
            const bool highlighter = inputStroke.tool && *inputStroke.tool == "highlighter";
            const auto& defaults = highlighter ? highlightBrush : penBrush;
            prototype::Ink stroke{};
            stroke.page = state.page;
            stroke.brush.tool = highlighter ? Tool::Highlighter : Tool::Pen;
            stroke.brush.color = defaults.color;
            stroke.brush.width = defaults.width;
            if (inputStroke.width) {
                stroke.brush.width = *inputStroke.width;
            }
            if (inputStroke.color) {
                const auto color = *inputStroke.color;
                stroke.brush.color = {static_cast<double>((color >> 16U) & 0xffU) / 255.0,
                                      static_cast<double>((color >> 8U) & 0xffU) / 255.0,
                                      static_cast<double>(color & 0xffU) / 255.0};
            }
            stroke.fill = inputStroke.fill.value_or(-1);
            if (inputStroke.lineStyle && *inputStroke.lineStyle != "plain" && *inputStroke.lineStyle != "solid") {
                error = "The GTK 4 stroke model does not yet support the requested line style";
                return false;
            }
            stroke.pressureSensitive = !inputStroke.pressure.empty() && !highlighter;
            stroke.points.reserve(inputStroke.x.size());
            for (size_t index = 0; index < inputStroke.x.size(); ++index) {
                const double pressure = inputStroke.pressure.empty() ? 1.0 : inputStroke.pressure[index];
                stroke.points.push_back({inputStroke.x[index], inputStroke.y[index], pressure});
            }
            additions.push_back(std::move(stroke));
        }

        int undoPolicyValue = 0;
        switch (policy) {
            case prototype::PluginStrokeUndoPolicy::Grouped:
                undoPolicyValue = 0;
                break;
            case prototype::PluginStrokeUndoPolicy::Individual:
                undoPolicyValue = 1;
                break;
            case prototype::PluginStrokeUndoPolicy::None:
                undoPolicyValue = 2;
                break;
        }
        const auto first = state.ink.size();
        if (!state.addPluginStrokes(std::move(additions), undoPolicyValue)) {
            error = "The stroke batch could not be added to the current layer";
            return false;
        }
        references.clear();
        references.reserve(inputs.size());
        for (size_t index = first; index < state.ink.size(); ++index) {
            references.push_back(reinterpret_cast<std::uintptr_t>(&state.ink[index]));
        }
        update();
        return true;
    }

    bool setPluginPageSize(double width, double height, bool relative, std::string& error) {
        if (busy || choosingFile || messageDialog || closed || autosaving) {
            error = "Wait for the current document operation to finish before changing page size";
            return false;
        }
        const auto current = state.pageSize(state.page);
        if (relative) {
            width += current.width;
            height += current.height;
        }
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
            error = "Page dimensions must be finite and greater than zero";
            return false;
        }
        if (!state.resizePage(state.page, width, height) &&
            (state.pageSize(state.page).width != width || state.pageSize(state.page).height != height)) {
            error = "The current page size could not be changed";
            return false;
        }
        update();
        return true;
    }

    bool setPluginLayerVisibility(bool visible, std::string& error) {
        if (busy || choosingFile || messageDialog || closed || autosaving) {
            error = "Wait for the current document operation to finish before changing layer visibility";
            return false;
        }
        if (state.page < 0 || static_cast<size_t>(state.page) >= state.pageLayers.size()) {
            error = "There is no current layer";
            return false;
        }
        state.setLayerVisible(state.page, state.activeLayerId(state.page), visible);
        update();
        return true;
    }

    bool setPluginCurrentLayerName(std::string_view name, std::string& error) {
        if (busy || choosingFile || messageDialog || closed || autosaving) {
            error = "Wait for the current document operation to finish before renaming the layer";
            return false;
        }
        if (!state.renameLayer(state.page, state.activeLayerId(state.page), std::string(name))) {
            const auto& layers = state.layersForPage(state.page).layers;
            const auto layer = state.activeLayerId(state.page);
            if (layer == 0 || layer > layers.size()) {
                error = "There is no current layer to rename";
                return false;
            }
        }
        update();
        return true;
    }

    bool changePluginActionState(std::string_view action, const prototype::PluginActionValue& value,
                                 std::string& error) {
        if (action == "position-highlighting") {
            if (!std::holds_alternative<bool>(value)) {
                error = "position-highlighting expects a boolean state";
                return false;
            }
            const bool previous = importedConfig.settings.highlightPosition;
            importedConfig.settings.highlightPosition = std::get<bool>(value);
            std::string saveError;
            if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, saveError)) {
                importedConfig.settings.highlightPosition = previous;
                error = saveError;
                return false;
            }
            auto* control = preferenceControls[static_cast<size_t>(PreferenceKey::PositionHighlight)];
            if (control) {
                syncingPreferenceControls = true;
                adw_switch_row_set_active(ADW_SWITCH_ROW(control), importedConfig.settings.highlightPosition);
                syncingPreferenceControls = false;
            }
            update();
            return true;
        }
        if (action == "grid-snapping") {
            if (!std::holds_alternative<bool>(value)) {
                error = "grid-snapping expects a boolean state";
                return false;
            }
            state.snapGridEnabled = std::get<bool>(value);
            importedConfig.settings.snapGrid = state.snapGridEnabled;
            std::string saveError;
            if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, saveError)) {
                error = saveError;
                return false;
            }
            update();
            return true;
        }
        if (action != "select-tool" || !std::holds_alternative<std::int64_t>(value)) {
            error = "Only select-tool (integer), grid-snapping (boolean), and position-highlighting (boolean) "
                    "action state changes are supported";
            return false;
        }

        const auto selectionTool = [&](Tool selected, bool acrossLayers = false) {
            activateSelection(selected, acrossLayers);
        };
        switch (std::get<std::int64_t>(value)) {
            case 1:
                selectTool(Tool::Pen);
                break;
            case 2:
                selectTool(Tool::Eraser);
                break;
            case 3:
                selectTool(Tool::Highlighter);
                break;
            case 4:
                selectTool(Tool::Text);
                break;
            case 5:
                if (imageButton)
                    g_signal_emit_by_name(imageButton, "clicked");
                break;
            case 6:
                selectionTool(Tool::SelectRectangle);
                break;
            case 7:
                selectionTool(Tool::SelectLasso);
                break;
            case 8:
                selectionTool(Tool::SelectRectangle, true);
                break;
            case 9:
                selectionTool(Tool::SelectLasso, true);
                break;
            case 10:
                selectionTool(Tool::SelectObject);
                break;
            case 11:
                selectTool(Tool::PlayObject);
                break;
            case 12:
                selectTool(Tool::VerticalSpace);
                break;
            case 13:
                selectTool(Tool::Hand);
                break;
            case 14:
                state.drawingMode = prototype::DrawingMode::Rectangle;
                selectTool(Tool::Pen);
                break;
            case 15:
                state.drawingMode = prototype::DrawingMode::Ellipse;
                selectTool(Tool::Pen);
                break;
            case 16:
                state.drawingMode = prototype::DrawingMode::Arrow;
                selectTool(Tool::Pen);
                break;
            case 17:
                state.drawingMode = prototype::DrawingMode::DoubleArrow;
                selectTool(Tool::Pen);
                break;
            case 18:
                state.drawingMode = prototype::DrawingMode::CoordinateSystem;
                selectTool(Tool::Pen);
                break;
            case 19:
                selectTool(Tool::FloatingToolbox);
                break;
            case 20:
                state.drawingMode = prototype::DrawingMode::Spline;
                selectTool(Tool::Pen);
                break;
            case 21:
                selectionTool(Tool::SelectPdfTextLinear);
                break;
            case 22:
                selectionTool(Tool::SelectPdfTextRect);
                break;
            case 23:
                selectTool(Tool::LaserPointerPen);
                break;
            case 24:
                selectTool(Tool::LaserPointerHighlighter);
                break;
            case 25:
                selectTool(Tool::Link);
                break;
            case 27:
                selectionTool(Tool::SmartSelect);
                break;
            default:
                error = "This master tool is not available in the GTK 4 prototype yet";
                return false;
        }
        if (drawingModeDropdown) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown), static_cast<guint>(state.drawingMode));
        }
        update();
        return true;
    }

    bool changePdfBackgroundPageFromPlugin(int pdfPage, std::string& error) {
        if (!nativeSession || options.worker.empty()) {
            error = "Beamer animation navigation requires a native document and document worker";
            return false;
        }
        if (busy || choosingFile || messageDialog || closed) {
            error = "A document operation is already in progress";
            return false;
        }
        if (autosaving) {
            waitForAutosave([this, pdfPage] {
                std::string deferredError;
                if (!changePdfBackgroundPageFromPlugin(pdfPage, deferredError)) {
                    showError(deferredError);
                }
            });
            return true;
        }
        if (pdfPage < 0 || static_cast<size_t>(pdfPage) >= nativeSession->pdfPageLabels.size()) {
            error = "The requested Beamer PDF page is unavailable";
            return false;
        }

        finishTextEditing();
        resetInput(true);
        const int documentPage = state.page;
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto packets = prototype::bridge::transferInk(state.committedInk());
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Advancing Beamer animation…",
                [session, worker, documentPage, pdfPage, packets, texts, images, layers, pageSizes, staged] {
                    *staged = session->stageSetPdfPageBackground(worker, documentPage, pdfPage, packets, texts, images,
                                                                 layers, pageSizes);
                },
                [this, session, documentPage, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<PopplerDocument*> candidateLayers;
                    std::vector<PopplerDocument*> candidateAudioLayers;
                    std::vector<prototype::PageSize> sizes;
                    prototype::bridge::LayerDocument layerState;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                        candidateLayers =
                                openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                        candidateAudioLayers = openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(),
                                                                         static_cast<int>(sizes.size()));
                        layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                    } catch (...) {
                        if (candidate) {
                            g_object_unref(candidate);
                        }
                        releaseLayerPreviewPdfs(candidateLayers);
                        releaseLayerPreviewPdfs(candidateAudioLayers);
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }

                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    releaseLayerPreviewPdfs(layerDocuments);
                    layerDocuments = std::move(candidateLayers);
                    releaseLayerPreviewPdfs(audioLayerDocuments);
                    audioLayerDocuments = std::move(candidateAudioLayers);
                    audioTargets = session->audioTargets;
                    state.setPages(std::move(sizes));
                    state.selectActivePage(documentPage);
                    state.setLayerDocument(layerState);
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    g_clear_object(&pdfTextDocument);
                    pdfTextDocumentPath.clear();
                    pageCache.clear();
                    layerPageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    navigateToPage(documentPage);
                    updateTitle();
                });
        return true;
    }

    bool stepBeamerPresentation(int delta, std::string& error) {
        if (!nativeSession) {
            error = "Beamer navigation requires an open document";
            return false;
        }
        const auto action =
                prototype::beamer::step(state.page, delta, nativeSession->pdfPageMap, nativeSession->pdfPageLabels);
        if (!action) {
            return true;
        }
        if (action->kind == prototype::beamer::Step::Kind::ChangePdfBackground) {
            return changePdfBackgroundPageFromPlugin(action->target, error);
        }
        navigateToPage(action->target);
        return true;
    }

    bool cleanBeamerAnimations(std::string& error) {
        if (!nativeSession || options.worker.empty()) {
            error = "Cleaning Beamer animation pages requires a native document and document worker";
            return false;
        }
        if (busy || choosingFile || messageDialog || closed) {
            error = "A document operation is already in progress";
            return false;
        }
        if (autosaving) {
            waitForAutosave([this] {
                std::string deferredError;
                if (!cleanBeamerAnimations(deferredError)) {
                    showError(deferredError);
                }
            });
            return true;
        }

        const auto pageIndices =
                prototype::beamer::animationPagesToRemove(nativeSession->pdfPageMap, nativeSession->pdfPageLabels);
        if (pageIndices.empty()) {
            return true;
        }
        if (pageIndices.size() >= static_cast<size_t>(pageCount)) {
            error = "Beamer cleanup cannot remove every page in a document";
            return false;
        }

        finishTextEditing();
        resetInput(true);
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto pageSizes = currentPageSizes(state);
        const auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Removing Beamer animation pages…",
                [session, worker, pageIndices, pageSizes, staged] {
                    *staged = session->stageDeletePagesAt(worker, pageIndices, pageSizes);
                },
                [this, session, pageIndices, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<PopplerDocument*> candidateLayers;
                    std::vector<PopplerDocument*> candidateAudioLayers;
                    std::vector<prototype::PageSize> sizes;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                        candidateLayers =
                                openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                        candidateAudioLayers = openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(),
                                                                         static_cast<int>(sizes.size()));
                    } catch (...) {
                        if (candidate) {
                            g_object_unref(candidate);
                        }
                        releaseLayerPreviewPdfs(candidateLayers);
                        releaseLayerPreviewPdfs(candidateAudioLayers);
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }

                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    releaseLayerPreviewPdfs(layerDocuments);
                    layerDocuments = std::move(candidateLayers);
                    releaseLayerPreviewPdfs(audioLayerDocuments);
                    audioLayerDocuments = std::move(candidateAudioLayers);
                    audioTargets = session->audioTargets;
                    for (const int pageIndex: pageIndices) {
                        state.deletePageAt(pageIndex);
                    }
                    pageCount = static_cast<int>(sizes.size());
                    state.setPages(std::move(sizes));
                    state.selectActivePage(std::min(pageIndices.back(), pageCount - 1));
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    g_clear_object(&pdfTextDocument);
                    pdfTextDocumentPath.clear();
                    pageCache.clear();
                    layerPageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    navigateToPage(state.page);
                    updateTitle();
                });
        return true;
    }

    void showLayerActionsNotice(const char* message) {
        std::string error;
        if (!showPluginDialog("LayerActions", message, {{1, "OK"}}, true, {}, error)) {
            showError(error);
        }
    }

    void startLayerActionsClone(int currentPage, int pdfPage, bool allowAnnotated,
                                const std::shared_ptr<prototype::bridge::DocumentSession>& session,
                                const std::string& worker,
                                const std::vector<prototype::bridge::StrokePacket>& packets,
                                const std::vector<prototype::TextItem>& texts,
                                const std::vector<prototype::bridge::ImagePacket>& images,
                                const prototype::bridge::LayerDocument& layers,
                                const std::vector<prototype::bridge::PageSizePacket>& pageSizes) {
        if (nativeSession != session || currentPage < 0 || currentPage + 1 >= pageCount) {
            showError("The LayerActions source page changed before cloning could start");
            return;
        }
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Cloning layers to the next PDF page…",
                [session, worker, currentPage, pdfPage, allowAnnotated, packets, texts, images, layers, pageSizes,
                 staged] {
                    *staged = session->stageLayerActionsCloneNext(worker, currentPage, pdfPage, allowAnnotated,
                                                                  packets, texts, images, layers, pageSizes);
                },
                [this, session, currentPage, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<PopplerDocument*> candidateLayers;
                    std::vector<PopplerDocument*> candidateAudioLayers;
                    std::vector<prototype::PageSize> sizes;
                    prototype::bridge::LayerDocument layerState;
                    std::vector<Ink> hydratedInk;
                    std::vector<prototype::TextItem> hydratedTexts;
                    std::vector<prototype::CanvasImage> hydratedImages;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                        candidateLayers =
                                openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                        candidateAudioLayers = openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(),
                                                                         static_cast<int>(sizes.size()));
                        layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                        hydratedInk = prototype::bridge::sessionInk(
                                prototype::bridge::readTransfer(staged->editableInkPath()));
                        hydratedTexts = prototype::bridge::readTextTransfer(staged->editableTextPath());
                        const auto imagePackets = prototype::bridge::readImageTransfer(staged->editableImagesPath());
                        hydratedImages.reserve(imagePackets.size());
                        for (const auto& packet: imagePackets) {
                            hydratedImages.push_back(prototype::hydrateCanvasImage(
                                    prototype::bridge::canvasImage(packet)));
                        }
                        if (sizes.size() != static_cast<size_t>(pageCount) || layerState.size() != sizes.size() ||
                            currentPage + 1 >= static_cast<int>(sizes.size())) {
                            throw std::runtime_error("LayerActions clone output does not match the current document");
                        }
                    } catch (...) {
                        if (candidate) {
                            g_object_unref(candidate);
                        }
                        releaseLayerPreviewPdfs(candidateLayers);
                        releaseLayerPreviewPdfs(candidateAudioLayers);
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }

                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    releaseLayerPreviewPdfs(layerDocuments);
                    layerDocuments = std::move(candidateLayers);
                    releaseLayerPreviewPdfs(audioLayerDocuments);
                    audioLayerDocuments = std::move(candidateAudioLayers);
                    audioTargets = session->audioTargets;
                    if (!state.replaceNextPageWithClone(currentPage, sizes.at(static_cast<size_t>(currentPage + 1)))) {
                        throw std::runtime_error("Cannot replace the next page with the cloned page");
                    }
                    pageCount = static_cast<int>(sizes.size());
                    state.setPages(std::move(sizes));
                    state.setLayerDocument(layerState);
                    state.ink = std::move(hydratedInk);
                    state.texts = std::move(hydratedTexts);
                    state.images = std::move(hydratedImages);
                    state.selectActivePage(currentPage + 1);
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    selectedImageIndex.reset();
                    g_clear_object(&pdfTextDocument);
                    pdfTextDocumentPath.clear();
                    pageCache.clear();
                    layerPageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    navigateToPage(currentPage + 1);
                    updateTitle();
                });
    }

    bool cloneNextPageFromLayerActions(std::string& error) {
        if (!nativeSession || options.worker.empty()) {
            error = "Cloning layers requires an open document and document worker";
            return false;
        }
        if (busy || choosingFile || messageDialog || closed) {
            error = "A document operation is already in progress";
            return false;
        }
        if (autosaving) {
            waitForAutosave([this] {
                std::string deferredError;
                if (!cloneNextPageFromLayerActions(deferredError)) {
                    showError(deferredError);
                }
            });
            return true;
        }

        finishTextEditing();
        resetInput(true);
        const int currentPage = state.page;
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto packets = prototype::bridge::transferInk(state.committedInk());
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        auto target = std::make_shared<prototype::bridge::LayerActionTargetInfo>();
        runDocumentJob(
                "Checking the next page…",
                [session, worker, currentPage, packets, texts, images, layers, target] {
                    *target = session->inspectLayerActionTarget(worker, currentPage, packets, texts, images, layers);
                },
                [this, currentPage, session, worker, packets, texts, images, layers, pageSizes, target] {
                    if (!target->hasNextPage) {
                        showLayerActionsNotice("No next page. ");
                        return;
                    }
                    if (target->nextPdfPage < 0) {
                        showLayerActionsNotice("Next page has no pdf background. ");
                        return;
                    }
                    if (!target->nextPageAnnotated) {
                        startLayerActionsClone(currentPage, target->nextPdfPage, false, session, worker, packets,
                                               texts, images, layers, pageSizes);
                        return;
                    }

                    std::string dialogError;
                    const bool shown = showPluginDialog(
                            "LayerActions", "Next page contains annotations that will be lost, when proceeding. ",
                            {{1, "Cancel"}, {2, "Proceed"}}, false,
                            [this, currentPage, pdfPage = target->nextPdfPage, session, worker, packets, texts, images,
                             layers, pageSizes](int response) {
                                if (response == 2) {
                                    startLayerActionsClone(currentPage, pdfPage, true, session, worker, packets, texts,
                                                           images, layers, pageSizes);
                                }
                            },
                            dialogError);
                    if (!shown) {
                        showError(dialogError);
                    }
                });
        return true;
    }

    bool hideLayersFromLayerActions(std::string& error) {
        if (busy || choosingFile || messageDialog || closed) {
            error = "A document operation is already in progress";
            return false;
        }
        if (autosaving) {
            waitForAutosave([this] {
                std::string deferredError;
                if (!hideLayersFromLayerActions(deferredError)) {
                    showError(deferredError);
                }
            });
            return true;
        }

        finishTextEditing();
        resetInput(true);
        const int currentPage = state.page;
        for (int page = 0; page < state.pageCount(); ++page) {
            const auto count = state.layersForPage(page).layers.size();
            if (!state.selectLayer(page, 1)) {
                error = "LayerActions could not select the first layer on every page";
                return false;
            }
            for (size_t layer = 1; layer <= count; ++layer) {
                state.setLayerVisible(page, layer, layer == 1);
            }
        }
        state.selectActivePage(currentPage);
        navigateToPage(currentPage);
        refreshAfterLayerMutation();
        return true;
    }

    bool addTopLayerToEveryPageFromLayerActions(std::string& error) {
        if (busy || choosingFile || messageDialog || closed) {
            error = "A document operation is already in progress";
            return false;
        }
        if (autosaving) {
            waitForAutosave([this] {
                std::string deferredError;
                if (!addTopLayerToEveryPageFromLayerActions(deferredError)) {
                    showError(deferredError);
                }
            });
            return true;
        }
        for (int page = 0; page < state.pageCount(); ++page) {
            if (state.layersForPage(page).layers.size() >= prototype::bridge::maxLayersPerPage) {
                error = "A page already has the maximum supported number of layers";
                return false;
            }
        }

        finishTextEditing();
        resetInput(true);
        const int currentPage = state.page;
        for (int page = 0; page < state.pageCount(); ++page) {
            const auto topLayer = state.layersForPage(page).layers.size();
            state.selectActivePage(page);
            if (!state.selectLayer(page, topLayer) || !state.addLayerAboveCurrent()) {
                error = "LayerActions could not add a top layer to every page";
                return false;
            }
        }
        state.selectActivePage(currentPage);
        navigateToPage(currentPage);
        refreshAfterLayerMutation();
        return true;
    }

    bool activatePluginAction(std::string_view action, std::string& error) {
        if (action == "goto-next") {
            navigateToPage(state.page + 1);
        } else if (action == "goto-previous") {
            navigateToPage(state.page - 1);
        } else if (action == "goto-page") {
            openGotoPageDialog();
        } else if (action == "undo") {
            selectedImageIndex.reset();
            input.cancel();
            state.undo();
            update();
        } else if (action == "redo") {
            selectedImageIndex.reset();
            input.cancel();
            state.redo();
            update();
        } else if (action == "select-default-tool") {
            selectTool(Tool::Pen);
        } else if (action == "beamer-next") {
            return stepBeamerPresentation(1, error);
        } else if (action == "beamer-previous") {
            return stepBeamerPresentation(-1, error);
        } else if (action == "beamer-clean") {
            return cleanBeamerAnimations(error);
        } else if (action == "layer-actions-clone-next") {
            return cloneNextPageFromLayerActions(error);
        } else if (action == "layer-actions-hide") {
            return hideLayersFromLayerActions(error);
        } else if (action == "layer-actions-add-top") {
            return addTopLayerToEveryPageFromLayerActions(error);
        } else if (action == "layer-new-above-current") {
            if (busy || choosingFile || messageDialog || closed || autosaving) {
                error = "Wait for the current document operation to finish before adding a layer";
                return false;
            }
            if (!state.addLayerAboveCurrent()) {
                error = "A layer could not be added above the current layer";
                return false;
            }
            refreshAfterLayerMutation();
        } else {
            error = "This master action is not available in the GTK 4 prototype yet: " + std::string(action);
            return false;
        }
        return true;
    }

    void openPreferences() {
        if (preferencesDialog) {
            adw_dialog_present(ADW_DIALOG(preferencesDialog), window);
            return;
        }

        preferencesDialog = ADW_PREFERENCES_DIALOG(g_object_ref_sink(adw_preferences_dialog_new()));
        adw_preferences_dialog_set_search_enabled(preferencesDialog, true);

        auto* appearance = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_name(appearance, "appearance");
        adw_preferences_page_set_title(appearance, "Appearance");
        adw_preferences_page_set_icon_name(appearance, "applications-graphics-symbolic");
        auto* appearanceGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(appearanceGroup, "Window");
        static const char* const themes[] = {"Follow system", "Light", "Dark", nullptr};
        preferenceCombo(appearanceGroup, "Color scheme", "Choose how the canvas follows the desktop appearance",
                        themes, static_cast<guint>(settingsThemeIndex(importedConfig.settings.themeVariant)),
                        PreferenceKey::Theme);
        preferenceSwitch(appearanceGroup, "Show pages sidebar", "Keep page thumbnails beside the canvas",
                         importedConfig.settings.showSidebar, PreferenceKey::Sidebar);
        adw_preferences_page_add(appearance, appearanceGroup);
        adw_preferences_dialog_add(preferencesDialog, appearance);

        auto* latexPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_name(latexPage, "latex");
        adw_preferences_page_set_title(latexPage, "LaTeX");
        adw_preferences_page_set_icon_name(latexPage, "accessories-text-editor-symbolic");
        auto* latexGenerationGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(latexGenerationGroup, "Formula generation");
        adw_preferences_group_set_description(
                latexGenerationGroup,
                "Set the source, template and TeX command used when you insert a formula.");
        preferenceSwitch(latexGenerationGroup, "Check LaTeX dependencies automatically",
                         "Preserve the legacy setting; compiler errors are reported by the configured generator",
                         importedConfig.settings.latexAutoCheckDependencies,
                         PreferenceKey::LatexAutoCheckDependencies);
        preferenceTextEntry(latexGenerationGroup, "Default formula",
                            "Selected when you click an empty page with the LaTeX tool",
                            importedConfig.settings.latexDefaultText, PreferenceKey::LatexDefaultText);
        preferenceTemplatePath(latexGenerationGroup,
                               prototype::preferences_detail::pathToUtf8(importedConfig.settings.latexTemplatePath));
        preferenceTextEntry(latexGenerationGroup, "Generator command",
                            "Use {} where the temporary .tex path belongs",
                            importedConfig.settings.latexGeneratorCommand, PreferenceKey::LatexGeneratorCommand);
        auto* latexTestRow = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(latexTestRow), "Test the template and compiler");
        adw_action_row_set_subtitle(latexTestRow, "Compile the default formula without changing the document");
        latexSetupTestStatus = gtk_label_new("");
        gtk_widget_add_css_class(latexSetupTestStatus, "dim-label");
        gtk_widget_set_margin_end(latexSetupTestStatus, 8);
        adw_action_row_add_suffix(latexTestRow, latexSetupTestStatus);
        latexSetupTestButton = gtk_button_new_with_label("Test setup");
        g_signal_connect(latexSetupTestButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->testLatexSetup();
                         }), this);
        adw_action_row_add_suffix(latexTestRow, latexSetupTestButton);
        adw_preferences_group_add(latexGenerationGroup, GTK_WIDGET(latexTestRow));
        adw_preferences_page_add(latexPage, latexGenerationGroup);

        auto* sourceEditorGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(sourceEditorGroup, "In-app source editor");
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
        adw_preferences_group_set_description(sourceEditorGroup,
                                              "Tune the editor used when external editing is turned off.");
#else
        adw_preferences_group_set_description(
                sourceEditorGroup,
                "GtkSourceView was unavailable when this build was made. Source highlighting and indentation "
                "options are disabled.");
#endif
        preferenceSwitch(sourceEditorGroup, "Use a custom editor font", "Choose a font for LaTeX source",
                         importedConfig.settings.latexUseCustomEditorFont,
                         PreferenceKey::LatexUseCustomEditorFont);
        preferenceFont(sourceEditorGroup, "Editor font", "Set the family, style and size for the source editor",
                       importedConfig.settings.latexEditorFont);
        preferenceSwitch(sourceEditorGroup, "Wrap long lines", "Keep long formulas inside the editor width",
                         importedConfig.settings.latexEditorWordWrap, PreferenceKey::LatexEditorWordWrap);
        preferenceSwitch(sourceEditorGroup, "Indent automatically", "Indent new lines to match the current formula",
                         importedConfig.settings.latexSourceViewAutoIndent,
                         PreferenceKey::LatexSourceViewAutoIndent);
        preferenceSwitch(sourceEditorGroup, "Highlight LaTeX syntax", "Color LaTeX commands and comments",
                         importedConfig.settings.latexSourceViewSyntaxHighlight,
                         PreferenceKey::LatexSourceViewSyntaxHighlight);
        preferenceSwitch(sourceEditorGroup, "Show line numbers", "Number each line in the source editor",
                         importedConfig.settings.latexSourceViewShowLineNumbers,
                         PreferenceKey::LatexSourceViewShowLineNumbers);
        preferenceLatexTheme(sourceEditorGroup, importedConfig.settings.latexSourceViewThemeId);
        adw_preferences_page_add(latexPage, sourceEditorGroup);

        auto* externalEditorGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(externalEditorGroup, "External editor");
        adw_preferences_group_set_description(
                externalEditorGroup,
                "Open the temporary source in another app. The preview updates when that app closes.");
        preferenceSwitch(externalEditorGroup, "Use an external editor", "Open LaTeX source outside Xournal++",
                         importedConfig.settings.latexUseExternalEditor, PreferenceKey::LatexUseExternalEditor);
        preferenceTextEntry(externalEditorGroup, "Editor command",
                            "Leave empty to use the editor from $EDITOR",
                            importedConfig.settings.latexExternalEditorCommand,
                            PreferenceKey::LatexExternalEditorCommand);
        preferenceTextEntry(externalEditorGroup, "Temporary file extension",
                            "The source file extension passed to the external editor",
                            importedConfig.settings.latexTemporaryFileExtension,
                            PreferenceKey::LatexTemporaryFileExtension);
        preferenceSwitch(externalEditorGroup, "Insert automatically when valid",
                         "Insert the result after the external editor closes and compilation succeeds",
                         importedConfig.settings.latexExternalEditorAutoConfirm,
                         PreferenceKey::LatexExternalEditorAutoConfirm);
        adw_preferences_page_add(latexPage, externalEditorGroup);

        auto* latexSaveGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(latexSaveGroup, "Pending text settings");
        adw_preferences_group_set_description(
                latexSaveGroup,
                "Apply the formula, template and editor command fields above.");
        auto* latexSaveRow = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(latexSaveRow), "Save LaTeX settings");
        adw_action_row_set_subtitle(latexSaveRow, "Switches and selectors save as you change them");
        latexSettingsApplyStatus = gtk_label_new("");
        gtk_widget_add_css_class(latexSettingsApplyStatus, "dim-label");
        gtk_widget_set_margin_end(latexSettingsApplyStatus, 8);
        adw_action_row_add_suffix(latexSaveRow, latexSettingsApplyStatus);
        latexSettingsApplyButton = gtk_button_new_with_label("Apply");
        g_signal_connect(latexSettingsApplyButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->applyLatexPreferences()) {
                                 gtk_label_set_text(GTK_LABEL(self->latexSettingsApplyStatus), "Saved");
                                 gtk_widget_add_css_class(self->latexSettingsApplyStatus, "success");
                                 gtk_widget_remove_css_class(self->latexSettingsApplyStatus, "error");
                             }
                         }), this);
        adw_action_row_add_suffix(latexSaveRow, latexSettingsApplyButton);
        adw_preferences_group_add(latexSaveGroup, GTK_WIDGET(latexSaveRow));
        adw_preferences_page_add(latexPage, latexSaveGroup);

        adw_preferences_dialog_add(preferencesDialog, latexPage);
        updateLatexPreferenceSensitivity();

        auto* inputPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_name(inputPage, "input");
        adw_preferences_page_set_title(inputPage, "Input");
        adw_preferences_page_set_icon_name(inputPage, "input-tablet-symbolic");
        auto* pressureGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(pressureGroup, "Stylus pressure");
        preferenceSwitch(pressureGroup, "Use pressure sensitivity", "Apply pen pressure to stroke width",
                         importedConfig.settings.pressureSensitivity, PreferenceKey::PressureSensitivity);
        preferenceSpin(pressureGroup, "Minimum pressure", "Pressure floor for a light stroke",
                       importedConfig.settings.minimumPressure, 0.01, 1.0, 0.01, 2, "", PreferenceKey::MinimumPressure);
        preferenceSpin(pressureGroup, "Pressure multiplier", "Scale reported pressure before drawing",
                       importedConfig.settings.pressureMultiplier, 0.05, 10.0, 0.05, 2, "×",
                       PreferenceKey::PressureMultiplier);
        adw_preferences_page_add(inputPage, pressureGroup);
        auto* buttonsGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(buttonsGroup, "Stylus buttons");
        adw_preferences_group_set_description(
                buttonsGroup,
                "Tool mappings apply while a side button is held; Floating Toolbox opens at the pointer.");
        static const char* const buttonActions[] = {"No action",        "Pen",  "Highlighter", "Eraser", "Hand tool",
                                                    "Floating toolbox", nullptr};
        preferenceCombo(buttonsGroup, "Button 1", "First side button", buttonActions,
                        static_cast<guint>(importedConfig.settings.stylusButtonOne), PreferenceKey::StylusButtonOne);
        preferenceCombo(buttonsGroup, "Button 2", "Second side button", buttonActions,
                        static_cast<guint>(importedConfig.settings.stylusButtonTwo), PreferenceKey::StylusButtonTwo);
        adw_preferences_page_add(inputPage, buttonsGroup);
        auto* highlightGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(highlightGroup, "Pointer position highlight");
        preferenceSwitch(highlightGroup, "Highlight pointer position",
                         "Show a translucent circle around the pointer; the HighlightPosition plugin can toggle it",
                         importedConfig.settings.highlightPosition, PreferenceKey::PositionHighlight);
        preferenceColor(highlightGroup, "Circle color", "Choose the fill color and opacity",
                        importedConfig.settings.cursorHighlightColor, PreferenceKey::CursorHighlightColor);
        preferenceSpin(highlightGroup, "Circle radius", "Set the pointer highlight radius",
                       importedConfig.settings.cursorHighlightRadius, 0.0, 30.0, 1.0, 0, "px",
                       PreferenceKey::CursorHighlightRadius);
        preferenceColor(highlightGroup, "Border color", "Choose the outline color and opacity",
                        importedConfig.settings.cursorHighlightBorderColor, PreferenceKey::CursorHighlightBorderColor);
        preferenceSpin(highlightGroup, "Border width", "Set the pointer highlight outline width",
                       importedConfig.settings.cursorHighlightBorderWidth, 0.0, 30.0, 1.0, 0, "px",
                       PreferenceKey::CursorHighlightBorderWidth);
        adw_preferences_page_add(inputPage, highlightGroup);
        adw_preferences_dialog_add(preferencesDialog, inputPage);

        auto* behavior = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_name(behavior, "behavior");
        adw_preferences_page_set_title(behavior, "Behavior");
        adw_preferences_page_set_icon_name(behavior, "preferences-system-symbolic");
        auto* autosaveGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(autosaveGroup, "Autosave");
        preferenceSwitch(autosaveGroup, "Autosave changes", "Save recovery data while editing a document",
                         importedConfig.settings.autosaveEnabled, PreferenceKey::AutosaveEnabled);
        preferenceSpin(autosaveGroup, "Autosave interval", "Set to 0 to disable periodic saves",
                       importedConfig.settings.autosaveTimeoutMinutes, 0, 1440, 1, 0, "minutes",
                       PreferenceKey::AutosaveTimeout);
        adw_preferences_page_add(behavior, autosaveGroup);
        auto* navigationGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(navigationGroup, "Zoom");
        preferenceSpin(navigationGroup, "Keyboard zoom step", "Change zoom with the keyboard",
                       importedConfig.settings.zoomStep * 100.0, 0.5, 200.0, 0.5, 1, "%",
                       PreferenceKey::ZoomStep);
        preferenceSpin(navigationGroup, "Scroll zoom step", "Change zoom with the pointer wheel",
                       importedConfig.settings.scrollZoomStep * 100.0, 0.5, 200.0, 0.5, 1, "%",
                       PreferenceKey::ScrollZoomStep);
        preferenceSpin(navigationGroup, "Selection edge-pan speed",
                       "Move the canvas as a selection is dragged beyond its visible edge",
                       importedConfig.settings.edgePanSpeed, 0.0, 100.0, 1.0, 0, "%",
                       PreferenceKey::EdgePanSpeed);
        preferenceSpin(navigationGroup, "Selection edge-pan multiplier",
                       "Increase panning speed as more of the selection moves out of view",
                       importedConfig.settings.edgePanMaxMultiplier, 0.1, 10.0, 0.5, 1, "×",
                       PreferenceKey::EdgePanMaxMultiplier);
        adw_preferences_page_add(behavior, navigationGroup);
        auto* selectionGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(selectionGroup, "Selection");
        preferenceSwitch(selectionGroup, "Snap image rotation",
                         "Snap to 15° increments; hold Alt to temporarily reverse this setting",
                         importedConfig.settings.snapRotation, PreferenceKey::SnapRotation);
        preferenceSpin(selectionGroup, "Rotation snap tolerance",
                       "Snap image rotation to 15° increments near a match",
                       importedConfig.settings.snapRotationTolerance, 0.0, 1.0, 0.05, 2, "",
                       PreferenceKey::RotationSnapTolerance);
        preferenceSwitch(selectionGroup, "Snap spacing to the page grid",
                         "Snap vertical-space boundaries to the current page grid",
                         importedConfig.settings.snapGrid, PreferenceKey::SnapGrid);
        preferenceSpin(selectionGroup, "Grid spacing", "Spacing used when snapping vertical-space boundaries",
                       importedConfig.settings.snapGridSize, 0.1, 1000.0, 0.1, 2, "pt",
                       PreferenceKey::SnapGridSize);
        preferenceSpin(selectionGroup, "Grid snap tolerance", "How close a boundary must be to snap",
                       importedConfig.settings.snapGridTolerance, 0.0, 1.0, 0.05, 2, "",
                       PreferenceKey::SnapGridTolerance);
        adw_preferences_page_add(behavior, selectionGroup);
        adw_preferences_dialog_add(preferencesDialog, behavior);

        auto* toolbarPage = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
        adw_preferences_page_set_name(toolbarPage, "toolbar");
        adw_preferences_page_set_title(toolbarPage, "Toolbar");
        adw_preferences_page_set_icon_name(toolbarPage, "view-grid-symbolic");
        auto* toolboxGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(toolboxGroup, "Floating toolbox rows");
        adw_preferences_group_set_description(
                toolboxGroup,
                "Reorder or remove comma-separated Xournal++ actions such as PEN, HIGHLIGHTER, LASER_POINTER, "
                "PDF_TOOL, UNDO, ZOOM_SLIDER, DRAW_ELLIPSE, FINE, COLOR(n), and DRAW_ARROW. Imported rows are "
                "saved in GTK 4 preferences when you apply changes.");
        static constexpr std::array<PreferenceKey, 4> rowKeys = {
                PreferenceKey::FloatingToolboxRow1,
                PreferenceKey::FloatingToolboxRow2,
                PreferenceKey::FloatingToolboxRow3,
                PreferenceKey::FloatingToolboxRow4,
        };
        static constexpr std::array<const char*, 4> rowTitles = {
                "Row 1", "Row 2", "Row 3", "Row 4",
        };
        for (std::size_t index = 0; index < rowKeys.size(); ++index) {
            const auto value = prototype::serializeFloatingToolboxRow(
                    importedConfig.settings.floatingToolboxRows[index]);
            preferenceTextEntry(toolboxGroup, rowTitles[index], "Comma-separated toolbar actions", value,
                                rowKeys[index]);
        }
        auto* applyRow = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(applyRow), "Save toolbar layout");
        adw_action_row_set_subtitle(applyRow, "Apply the four rows and save them for future sessions");
        auto* applyButton = gtk_button_new_with_label("Apply");
        floatingToolboxApplyButton = applyButton;
        g_signal_connect(applyButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->applyFloatingToolboxRows();
                         }),
                         this);
        adw_action_row_add_suffix(applyRow, applyButton);
        adw_preferences_group_add(toolboxGroup, GTK_WIDGET(applyRow));
        adw_preferences_page_add(toolbarPage, toolboxGroup);
        adw_preferences_dialog_add(preferencesDialog, toolbarPage);

        auto* mainRailGroup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(mainRailGroup, "Main tool rail");
        adw_preferences_group_set_description(
                mainRailGroup,
                "Choose the tools shown directly beside the canvas. Omitted tools stay available under More tools. "
                "Keep SMART_SELECT in this list so it is always one click away.");
        preferenceTextEntry(mainRailGroup, "Tool order", "Comma-separated actions",
                            prototype::serializeMainToolRail(importedConfig.settings.mainToolRail),
                            PreferenceKey::MainToolRail);
        auto* mainRailApplyRow = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mainRailApplyRow), "Save main tool rail");
        adw_action_row_set_subtitle(mainRailApplyRow, "Apply the tool order and save it for future sessions");
        auto* mainRailApplyButton = gtk_button_new_with_label("Apply");
        mainToolRailApplyButton = mainRailApplyButton;
        g_signal_connect(mainRailApplyButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->applyMainToolRail();
                         }), this);
        adw_action_row_add_suffix(mainRailApplyRow, mainRailApplyButton);
        adw_preferences_group_add(mainRailGroup, GTK_WIDGET(mainRailApplyRow));
        adw_preferences_page_add(toolbarPage, mainRailGroup);
        adw_dialog_present(ADW_DIALOG(preferencesDialog), window);
    }

    void openPluginManager() {
        if (!pluginRuntime) {
            showAlert("Plugins unavailable", "The Lua plugin runtime did not initialize.");
            return;
        }
        pluginRuntime->resetPendingStates();
        if (!pluginManagerWindow) {
            buildPluginManager();
        }
        rebuildPluginManagerRows();
        gtk_window_present(pluginManagerWindow);
    }

    void buildPluginManager() {
        pluginManagerWindow = GTK_WINDOW(g_object_ref_sink(gtk_window_new()));
        gtk_window_set_title(pluginManagerWindow, "Manage Plugins");
        gtk_window_set_default_size(pluginManagerWindow, 620, 640);
        gtk_window_set_transient_for(pluginManagerWindow, GTK_WINDOW(window));
        gtk_window_set_modal(pluginManagerWindow, true);
        g_signal_connect(pluginManagerWindow, "close-request", G_CALLBACK(+[](GtkWindow* dialog, gpointer) -> gboolean {
                             gtk_widget_set_visible(GTK_WIDGET(dialog), false);
                             return true;
                         }),
                         nullptr);

        auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        auto* header = adw_header_bar_new();
        auto* title = gtk_label_new("Plugins");
        gtk_widget_add_css_class(title, "title");
        adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
        gtk_window_set_titlebar(pluginManagerWindow, header);

        auto* explanation =
                gtk_label_new("Choose which plugins to load. Changes take effect the next time Xournal++ starts.");
        gtk_label_set_wrap(GTK_LABEL(explanation), true);
        gtk_widget_set_halign(explanation, GTK_ALIGN_START);
        gtk_widget_set_margin_start(explanation, 18);
        gtk_widget_set_margin_end(explanation, 18);
        gtk_widget_set_margin_top(explanation, 12);
        gtk_widget_set_margin_bottom(explanation, 12);
        gtk_box_append(GTK_BOX(root), explanation);

        auto* scroller = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(scroller, true);
        pluginManagerList = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(pluginManagerList), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(pluginManagerList, "boxed-list");
        gtk_widget_set_margin_start(pluginManagerList, 18);
        gtk_widget_set_margin_end(pluginManagerList, 18);
        gtk_widget_set_margin_bottom(pluginManagerList, 12);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), pluginManagerList);
        gtk_box_append(GTK_BOX(root), scroller);

        auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(actions, GTK_ALIGN_END);
        gtk_widget_set_margin_start(actions, 18);
        gtk_widget_set_margin_end(actions, 18);
        gtk_widget_set_margin_bottom(actions, 18);
        auto* cancel = gtk_button_new_with_label("Cancel");
        g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             gtk_widget_set_visible(GTK_WIDGET(self->pluginManagerWindow), false);
                         }),
                         this);
        auto* apply = gtk_button_new_with_label("Apply");
        pluginManagerApplyButton = apply;
        gtk_widget_add_css_class(apply, "suggested-action");
        g_signal_connect(apply, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->applyPluginManagerChanges();
                         }),
                         this);
        gtk_box_append(GTK_BOX(actions), cancel);
        gtk_box_append(GTK_BOX(actions), apply);
        gtk_box_append(GTK_BOX(root), actions);
        gtk_window_set_child(pluginManagerWindow, root);
    }

    void rebuildPluginManagerRows() {
        pluginManagerSwitches.clear();
        while (auto* row = gtk_widget_get_first_child(pluginManagerList)) {
            gtk_list_box_remove(GTK_LIST_BOX(pluginManagerList), row);
        }
        for (const auto& plugin: pluginRuntime->plugins()) {
            auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
            gtk_widget_set_margin_start(row, 14);
            gtk_widget_set_margin_end(row, 14);
            gtk_widget_set_margin_top(row, 10);
            gtk_widget_set_margin_bottom(row, 10);

            auto* details = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            gtk_widget_set_hexpand(details, true);
            auto* name = gtk_label_new(plugin.name.c_str());
            gtk_label_set_xalign(GTK_LABEL(name), 0);
            gtk_widget_add_css_class(name, "heading");
            gtk_box_append(GTK_BOX(details), name);
            std::string summary = plugin.description;
            if (!plugin.author.empty()) {
                if (!summary.empty())
                    summary += " · ";
                summary += plugin.author;
            }
            if (!plugin.version.empty() && plugin.version != "<xournalpp>") {
                if (!summary.empty())
                    summary += " · ";
                summary += "v" + plugin.version;
            }
            if (!plugin.error.empty()) {
                if (!summary.empty())
                    summary += "\n";
                summary += plugin.error;
            }
            auto* description = gtk_label_new(summary.c_str());
            gtk_label_set_xalign(GTK_LABEL(description), 0);
            gtk_label_set_wrap(GTK_LABEL(description), true);
            gtk_widget_add_css_class(description, plugin.error.empty() ? "dim-label" : "error");
            gtk_box_append(GTK_BOX(details), description);
            gtk_box_append(GTK_BOX(row), details);

            auto* toggle = gtk_switch_new();
            gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
            gtk_widget_set_sensitive(toggle, plugin.valid);
            gtk_accessible_update_property(GTK_ACCESSIBLE(toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, plugin.name.c_str(),
                                           -1);
            gtk_switch_set_active(GTK_SWITCH(toggle), plugin.pendingEnabled);
            g_object_set_data_full(G_OBJECT(toggle), "plugin-name", g_strdup(plugin.name.c_str()), g_free);
            g_signal_connect(toggle, "notify::active", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const auto* name = static_cast<const char*>(g_object_get_data(object, "plugin-name"));
                                 if (name && self->pluginRuntime) {
                                     self->pluginRuntime->setPendingEnabled(name,
                                                                            gtk_switch_get_active(GTK_SWITCH(object)));
                                 }
                             }),
                             this);
            pluginManagerSwitches.emplace(plugin.name, toggle);
            gtk_box_append(GTK_BOX(row), toggle);
            gtk_list_box_append(GTK_LIST_BOX(pluginManagerList), row);
        }
    }

    void applyPluginManagerChanges() {
        const auto [enabled, disabled] = pluginRuntime->pendingPluginPaths();
        importedConfig.settings.pluginsEnabled = enabled;
        importedConfig.settings.pluginsDisabled = disabled;
        std::string error;
        if (!prototype::saveGtk4Preferences(preferencesConfigDir, importedConfig.settings, error)) {
            showAlert("Cannot save plugin settings", error);
            return;
        }
        const bool changed = pluginRuntime->applyPendingStates();
        gtk_widget_set_visible(GTK_WIDGET(pluginManagerWindow), false);
        if (changed) {
            showAlert("Restart required", "Plugin changes are saved. Restart Xournal++ to load the updated plugins.");
        }
    }

    void captureTopLevel(const std::string& name, GtkWidget* topLevel) {
        const auto deadline = g_get_monotonic_time() + 750000;
        while (!gtk_widget_get_mapped(topLevel) && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        auto* frameClock = gtk_widget_get_frame_clock(topLevel);
        require(frameClock != nullptr, "Plugin manager has no frame clock");
        const auto previousFrame = gdk_frame_clock_get_frame_counter(frameClock);
        gtk_widget_queue_draw(topLevel);
        while (gdk_frame_clock_get_frame_counter(frameClock) <= previousFrame && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        auto* paintable = gtk_widget_paintable_new(topLevel);
        auto* snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(GDK_PAINTABLE(paintable), GDK_SNAPSHOT(snapshot), gtk_widget_get_width(topLevel),
                               gtk_widget_get_height(topLevel));
        auto* node = gtk_snapshot_free_to_node(snapshot);
        if (!node) {
            g_object_unref(paintable);
            throw SnapshotPending{};
        }
        auto* renderer = gtk_native_get_renderer(GTK_NATIVE(topLevel));
        auto* texture = gsk_renderer_render_texture(renderer, node, nullptr);
        const auto path = options.smokeDirectory + "/" + name + ".png";
        const bool saved = gdk_texture_save_to_png(texture, path.c_str());
        g_object_unref(texture);
        gsk_render_node_unref(node);
        g_object_unref(paintable);
        if (!saved) {
            throw std::runtime_error("Cannot write plugin manager screenshot");
        }
    }

    void verifyQuickScreenshotPlugin() {
        const auto plugin = pluginRuntime->findPlugin("QuickScreenshot");
        require(plugin && plugin->loaded && plugin->menuItems == 1,
                "Bundled QuickScreenshot did not register its GTK4 menu callback");
        quickScreenshotVerifyDone = false;
        quickScreenshotVerifyError.clear();
        require(pluginRuntime->invokeMenu("QuickScreenshot", 0),
                "QuickScreenshot did not dispatch its capture action");

        const auto deadline = g_get_monotonic_time() + 10000000;
        while (!quickScreenshotVerifyDone && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(quickScreenshotVerifyDone,
                "QuickScreenshot did not finish capture and saving before the verification timeout");
        require(quickScreenshotVerifyError.empty(), quickScreenshotVerifyError.c_str());

        const auto path = std::filesystem::path(options.smokeDirectory) / "quick-screenshot.png";
        auto* surface = cairo_image_surface_create_from_png(path.c_str());
        const auto status = cairo_surface_status(surface);
        cairo_surface_destroy(surface);
        require(status == CAIRO_STATUS_SUCCESS, "QuickScreenshot did not save a valid PNG file");

        g_setenv("XOURNALPP_TEST_QUICK_SCREENSHOT_X11", "1", true);
        quickScreenshotVerifyDone = false;
        quickScreenshotVerifyError.clear();
        const bool clipboardCaptureStarted = pluginRuntime->invokeMenu("QuickScreenshot", 0);
        g_unsetenv("XOURNALPP_TEST_QUICK_SCREENSHOT_X11");
        const auto clipboardDeadline = g_get_monotonic_time() + 10000000;
        while (clipboardCaptureStarted && !quickScreenshotVerifyDone &&
               g_get_monotonic_time() < clipboardDeadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(clipboardCaptureStarted && quickScreenshotVerifyDone && quickScreenshotVerifyError.empty(),
                quickScreenshotVerifyError.empty() ?
                        "QuickScreenshot did not finish the X11 clipboard capture path" :
                        quickScreenshotVerifyError.c_str());
        const auto* clipboardFormats = gdk_clipboard_get_formats(gtk_widget_get_clipboard(canvas));
        require(gdk_content_formats_contain_mime_type(clipboardFormats, "image/png"),
                "QuickScreenshot did not publish PNG data to the GTK clipboard");
    }

    void verifyPluginIntegration() {
        require(pluginRuntime != nullptr, "Lua plugin runtime was not created");
        const auto savedInkSize = state.ink.size();
        const auto savedSelection = state.selected;
        const auto savedTextSelection = state.selectedTexts;
        const auto savedImageSelection = state.selectedImages;
        prototype::Ink sample;
        sample.page = state.page;
        sample.points = {{10, 20, 0.25}, {12.5, 24.5, 0.75}};
        sample.brush = {Tool::Pen, {0x12 / 255.0, 0x34 / 255.0, 0x56 / 255.0}, 2.25};
        sample.pressureSensitive = true;
        sample.layerId = state.activeLayerId(state.page);
        sample.layerOrder = 999999;
        state.ink.push_back(sample);

        std::vector<prototype::PluginStroke> allStrokes;
        std::string strokeError;
        require(getPluginStrokes("all", allStrokes, strokeError) && allStrokes.size() == savedInkSize + 1,
                "app.getStrokes('all') did not enumerate every editable stroke");
        const auto sampleOutput = std::find_if(allStrokes.begin(), allStrokes.end(), [&](const auto& stroke) {
            return stroke.reference == reinterpret_cast<std::uintptr_t>(&state.ink.back());
        });
        require(sampleOutput != allStrokes.end() && sampleOutput->x == std::vector<double>({10, 12.5}) &&
                        sampleOutput->y == std::vector<double>({20, 24.5}) &&
                        sampleOutput->pressure == std::vector<double>({0.25, 0.75}) &&
                        sampleOutput->tool == "pen" && std::abs(sampleOutput->width - 2.25) < 1e-9 &&
                        sampleOutput->color == 0x123456 && sampleOutput->page == static_cast<size_t>(state.page + 1) &&
                        sampleOutput->layer == state.activeLayerId(state.page),
                "app.getStrokes('all') did not preserve native point, pressure, style, or index values");
        for (const auto& stroke: allStrokes) {
            require(!stroke.x.empty() && stroke.x.size() == stroke.y.size() && stroke.reference != 0 &&
                            stroke.page && stroke.layer && *stroke.page >= 1 && *stroke.page <= state.pages.size() &&
                            *stroke.layer >= 1 &&
                            *stroke.layer <= state.layersForPage(static_cast<int>(*stroke.page - 1)).layers.size(),
                    "app.getStrokes('all') lost stroke geometry, references, or 1-based page/layer indices");
        }
        state.selected = {state.ink.size() - 1};
        state.selectedTexts.clear();
        state.selectedImages.clear();
        std::vector<prototype::PluginStroke> selectedStrokes;
        require(getPluginStrokes("selection", selectedStrokes, strokeError) && selectedStrokes.size() == 1 &&
                        !selectedStrokes.front().page && !selectedStrokes.front().layer,
                "app.getStrokes('selection') did not return the selected stroke without page metadata");
        std::vector<prototype::PluginStroke> pageStrokes;
        require(getPluginStrokes("page", pageStrokes, strokeError) && !state.hasSelection(),
                "app.getStrokes('page') did not clear the current selection");
        require(std::all_of(pageStrokes.begin(), pageStrokes.end(), [](const auto& stroke) {
                    return !stroke.page && stroke.layer.has_value();
                }),
                "app.getStrokes('page') returned the wrong page/layer metadata");
        state.ink.resize(savedInkSize);
        state.selected = savedSelection;
        state.selectedTexts = savedTextSelection;
        state.selectedImages = savedImageSelection;
        update();
        const auto plugins = pluginRuntime->plugins();
        require(plugins.size() >= 12, "Bundled plugin discovery did not find the full master plugin set");
        const auto colorCycle = pluginRuntime->findPlugin("ColorCycle");
        require(colorCycle && colorCycle->valid && colorCycle->loaded && colorCycle->menuItems == 1,
                "Enabled bundled ColorCycle did not load and register its menu action");
        const auto toolShortcuts = pluginRuntime->findPlugin("ToolShortcuts");
        require(toolShortcuts && toolShortcuts->loaded && toolShortcuts->menuItems == 14,
                "Enabled bundled ToolShortcuts plugin did not register its complete menu set");
        const auto example = pluginRuntime->findPlugin("Example");
        require(example && example->loaded && example->menuItems == 1,
                "Enabled bundled Example plugin did not register its menu callback");
        const auto toggleGrid = pluginRuntime->findPlugin("ToggleGrid");
        require(toggleGrid && toggleGrid->loaded && toggleGrid->menuItems == 1 && nativeSession,
                "Enabled bundled ToggleGrid plugin did not load with a native document worker");
        const auto highlightPosition = pluginRuntime->findPlugin("HighlightPosition");
        require(highlightPosition && highlightPosition->loaded && highlightPosition->menuItems == 1,
                "Enabled bundled HighlightPosition plugin did not register its menu action");
        const auto spaceForNotes = pluginRuntime->findPlugin("SpaceForNotes");
        require(spaceForNotes && spaceForNotes->loaded && spaceForNotes->menuItems == 2,
                "Enabled bundled SpaceForNotes plugin did not register both page insertion modes");
        const auto imageActions = pluginRuntime->findPlugin("ImageActions");
        require(imageActions && imageActions->loaded && imageActions->menuItems == 10,
                "Enabled bundled ImageActions plugin did not register all ten image operations");
        const auto fitToContent = pluginRuntime->findPlugin("FitToContent");
        require(fitToContent && fitToContent->loaded && fitToContent->menuItems == 2,
                "Enabled bundled FitToContent plugin did not register both page-fitting actions");
        const auto exportPlugin = pluginRuntime->findPlugin("Export");
        require(exportPlugin && exportPlugin->loaded && exportPlugin->menuItems == 3,
                "Enabled bundled Export plugin did not register PDF, SVG, and PNG actions");
        const auto quickScreenshotPlugin = pluginRuntime->findPlugin("QuickScreenshot");
        require(quickScreenshotPlugin && quickScreenshotPlugin->loaded && quickScreenshotPlugin->menuItems == 1,
                "Enabled bundled QuickScreenshot plugin did not register its capture action");
        auto* menuModel = gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(pluginsMenuButton));
        require(menuModel && g_menu_model_get_n_items(menuModel) == 2,
                "Plugins menu is missing the enabled plugin group or manager action");
        auto* pluginSection = g_menu_model_get_item_link(menuModel, 0, G_MENU_LINK_SECTION);
        require(pluginSection && g_menu_model_get_n_items(pluginSection) == 10,
                "Plugins menu did not include each enabled plugin group");
        g_object_unref(pluginSection);
        require(g_action_group_has_action(G_ACTION_GROUP(window), "gtk4-plugin-0"),
                "Registered Lua menu action is missing from the application window");
        gtk_menu_button_popup(GTK_MENU_BUTTON(pluginsMenuButton));
        capture("plugins-menu");
        gtk_menu_button_popdown(GTK_MENU_BUTTON(pluginsMenuButton));
        verifyExportPlugin();

        require(pluginRuntime->handleKeyPress(GDK_KEY_v, GdkModifierType(0), false) &&
                        state.brush.tool == Tool::SmartSelect,
                "Bundled ToolShortcuts did not select Smart Select with its canvas shortcut");
        require(!pluginRuntime->handleKeyPress(GDK_KEY_p, GdkModifierType(0), true) &&
                        state.brush.tool == Tool::SmartSelect,
                "Bundled ToolShortcuts stole a key from text editing");
        require(pluginRuntime->handleKeyPress(GDK_KEY_p, GdkModifierType(0), false) && state.brush.tool == Tool::Pen,
                "Bundled ToolShortcuts did not restore the Pen tool");
        auto* application = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
        std::optional<std::string> highlightAction;
        for (guint index = 0; index < 64 && !highlightAction; ++index) {
            const auto actionName = "gtk4-plugin-" + std::to_string(index);
            const auto detailedActionName = "win." + actionName;
            gchar** accelerators = gtk_application_get_accels_for_action(application, detailedActionName.c_str());
            for (guint acceleratorIndex = 0; accelerators && accelerators[acceleratorIndex]; ++acceleratorIndex) {
                if (g_str_equal(accelerators[acceleratorIndex], "<Alt>x")) {
                    highlightAction = actionName;
                    break;
                }
            }
            g_strfreev(accelerators);
        }
        require(highlightAction.has_value(), "HighlightPosition did not register its Alt+X accelerator");
        g_action_group_activate_action(G_ACTION_GROUP(window), highlightAction->c_str(), nullptr);
        require(importedConfig.settings.highlightPosition,
                "HighlightPosition's Alt+X action did not enable the pointer halo");
        pointerPosition = prototype::Point{gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0};
        update();
        capture("position-highlight-on");
        prototype::ImportedSettings reloadedHighlightSettings;
        const auto highlightPreferences =
                prototype::loadGtk4Preferences(preferencesConfigDir, reloadedHighlightSettings);
        require(highlightPreferences.found && reloadedHighlightSettings.highlightPosition &&
                        reloadedHighlightSettings.cursorHighlightColor ==
                                importedConfig.settings.cursorHighlightColor &&
                        std::abs(reloadedHighlightSettings.cursorHighlightRadius -
                                 importedConfig.settings.cursorHighlightRadius) < 1e-8,
                "Pointer highlight action or appearance settings did not persist");
        g_action_group_activate_action(G_ACTION_GROUP(window), highlightAction->c_str(), nullptr);
        require(!importedConfig.settings.highlightPosition,
                "HighlightPosition did not disable the pointer halo on its second activation");

        require(nativeSession && pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
                "SpaceForNotes integration needs the two-page PDF fixture");
        require(pluginRuntime->invokeMenu("SpaceForNotes", 0),
                "SpaceForNotes normal mode did not start from its plugin menu");
        waitForPluginDocumentJob();
        require(pageCount == 4 && state.page == 1 && nativeSession->pdfPageMap == std::vector<int>({0, -1, 1, -1}),
                "SpaceForNotes normal mode did not insert lined pages after consecutive PDF pages");
        require(state.layersForPage(1).backgroundFormat == "lined" &&
                        state.layersForPage(3).backgroundFormat == "lined",
                "SpaceForNotes normal mode did not preserve inserted page background metadata");
        capture("space-for-notes-pdf-gaps");
        require(pluginRuntime->invokeMenu("SpaceForNotes", 1),
                "SpaceForNotes force mode did not start from its plugin menu");
        waitForPluginDocumentJob();
        require(pageCount == 8 && state.page == 1 &&
                        nativeSession->pdfPageMap == std::vector<int>({0, -1, -1, -1, 1, -1, -1, -1}),
                "SpaceForNotes force mode did not add a page after every page in reverse-safe order");
        for (const auto page: {1, 3, 5, 7}) {
            require(state.layersForPage(page).backgroundFormat == "lined",
                    "SpaceForNotes force mode did not preserve inserted page background metadata");
        }
        require(pluginRuntime->invokeMenu("SpaceForNotes", 0),
                "SpaceForNotes normal mode could not be invoked after note pages existed");
        require(pageCount == 8 && state.page == 0 &&
                        nativeSession->pdfPageMap == std::vector<int>({0, -1, -1, -1, 1, -1, -1, -1}),
                "SpaceForNotes normal mode duplicated note pages or failed to restore the first page");
        const auto notesRoundTripPath =
                (std::filesystem::path(options.smokeDirectory) / "space-for-notes-round-trip.xopp").string();
        nativeSession->saveCopy(options.worker, notesRoundTripPath,
                                prototype::bridge::transferInk(state.committedInk()), state.texts,
                                prototype::bridge::transferImages(state.images), state.pageLayers,
                                currentPageSizes(state));
        const prototype::bridge::DocumentSession reopenedNotes(options.worker, notesRoundTripPath);
        require(reopenedNotes.pdfPageMap == std::vector<int>({0, -1, -1, -1, 1, -1, -1, -1}),
                "Saving and reopening a SpaceForNotes document changed PDF and note page placement");
        require(reopenedNotes.initialLayers.size() == 8 && reopenedNotes.initialLayers[1].backgroundFormat == "lined" &&
                        reopenedNotes.initialLayers[3].backgroundFormat == "lined" &&
                        reopenedNotes.initialLayers[5].backgroundFormat == "lined" &&
                        reopenedNotes.initialLayers[7].backgroundFormat == "lined",
                "Saving and reopening a SpaceForNotes document changed note page backgrounds");

        constexpr std::array<std::uint32_t, 11> expectedColors{
                0x000000, 0x008000, 0x00c0ff, 0x00ff00, 0x3333cc, 0x808080,
                0xff0000, 0xff00ff, 0xff8000, 0xffff00, 0xffffff,
        };
        for (const auto expected: expectedColors) {
            g_action_group_activate_action(G_ACTION_GROUP(window), "gtk4-plugin-0", nullptr);
            const std::array<double, 3> expectedRgb{
                    static_cast<double>((expected >> 16U) & 0xffU) / 255.0,
                    static_cast<double>((expected >> 8U) & 0xffU) / 255.0,
                    static_cast<double>(expected & 0xffU) / 255.0,
            };
            require(penBrush.color == expectedRgb, "Bundled ColorCycle did not match its full legacy color sequence");
        }
        Ink selectedStroke;
        selectedStroke.page = 0;
        selectedStroke.layerId = 1;
        selectedStroke.originLayer = 0;
        selectedStroke.brush = {Tool::Pen, {1, 0, 0}, 2};
        selectedStroke.points = {{40, 40, 1}, {50, 50, 1}};
        state.ink.push_back(selectedStroke);
        state.selected = {state.ink.size() - 1};
        require(pluginRuntime->invokeMenu("ColorCycle", 0) &&
                        state.ink.back().brush.color == std::array<double, 3>{0, 0, 0},
                "ColorCycle did not recolor a selected stroke through the legacy selection API");
        state.undo();
        require(state.ink.back().brush.color == selectedStroke.brush.color,
                "ColorCycle selection recoloring was not undoable");
        state.clearSelection();
        state.ink.pop_back();

        navigateToPage(0);
        require(pluginRuntime->invokeMenu("ToggleGrid", 0), "ToggleGrid menu action did not execute");
        waitForPluginDocumentJob();
        require(state.layersForPage(0).backgroundFormat == "plain" && !state.snapGridEnabled,
                "ToggleGrid did not set the native page to plain paper and disable grid snapping");
        capture("toggle-grid-plain");
        require(pluginRuntime->invokeMenu("ToggleGrid", 0), "ToggleGrid second menu action did not execute");
        waitForPluginDocumentJob();
        require(state.layersForPage(0).backgroundFormat == "graph" && state.snapGridEnabled,
                "ToggleGrid did not set the native page to graph paper and enable grid snapping");
        capture("toggle-grid-graph");
        state.undo();
        waitForPluginDocumentJob();
        require(state.layersForPage(0).backgroundFormat == "plain",
                "Undo did not restore the prior page background through the native worker");
        state.redo();
        waitForPluginDocumentJob();
        require(state.layersForPage(0).backgroundFormat == "graph",
                "Redo did not restore the graph page background through the native worker");
        const auto roundTripPath =
                (std::filesystem::path(options.smokeDirectory) / "toggle-grid-round-trip.xopp").string();
        nativeSession->saveCopy(options.worker, roundTripPath, prototype::bridge::transferInk(state.committedInk()),
                                state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                                currentPageSizes(state));
        const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
        require(!reopened.initialLayers.empty() && reopened.initialLayers[0].backgroundFormat == "graph",
                "Saving and reopening a ToggleGrid document did not preserve its graph background");

        verifyImageActionsPlugin();

        g_action_group_activate_action(G_ACTION_GROUP(window), "gtk4-plugin-1", nullptr);
        require(messageDialog &&
                        g_strcmp0(adw_alert_dialog_get_response_label(messageDialog, "plugin-response-2"), "No") ==
                                0,
                "Example plugin did not open its GTK dialog with the legacy button labels");
        answerDialog("plugin-response-2");
        require(messageDialog == nullptr, "Example plugin dialog did not close after choosing a response");

        openPluginManager();
        require(pluginManagerWindow && gtk_widget_get_visible(GTK_WIDGET(pluginManagerWindow)),
                "Manage Plugins action did not present the GTK manager window");
        std::size_t rowCount = 0;
        for (auto* row = gtk_widget_get_first_child(pluginManagerList); row; row = gtk_widget_get_next_sibling(row)) {
            ++rowCount;
        }
        require(rowCount == plugins.size(), "Plugin manager did not show every discovered plugin");
        const auto enabledAgain = pluginRuntime->findPlugin("ColorCycle");
        require(enabledAgain && enabledAgain->pendingEnabled,
                "Plugin manager did not preserve the current enabled state");
        captureTopLevel("plugins-manager", GTK_WIDGET(pluginManagerWindow));

        const auto exampleSwitch = pluginManagerSwitches.find("Example");
        require(exampleSwitch != pluginManagerSwitches.end() && example,
                "Plugin manager is missing the bundled Example plugin control");
        gtk_switch_set_active(GTK_SWITCH(exampleSwitch->second), false);
        g_signal_emit_by_name(pluginManagerApplyButton, "clicked");
        require(messageDialog &&
                        importedConfig.settings.pluginsEnabled.find(example->path.string()) == std::string::npos &&
                        !pluginRuntime->findPlugin("Example")->pendingEnabled,
                "Applying plugin changes did not persist the disabled plugin state");
        answerDialog("close");
        verifyFitToContentPlugin();
        verifyQuickScreenshotPlugin();
        std::cout << "PASS: private GTK4 ColorCycle, Example dialog, ToolShortcuts, Smart Select shortcut, "
                     "HighlightPosition, QuickScreenshot region capture/clipboard, Export PDF/SVG/PNG, ImageActions, "
                     "FitToContent page resizing, "
                     "SpaceForNotes page insertion, "
                     "ToggleGrid paper changes, and background undo/redo\n";
    }

    void verifyExportPlugin() {
        const int pageCountBefore = pageCount;
        require(nativeSession && pageCountBefore == 2,
                "Export plugin verification expects the original two-page document fixture");
        const auto source = std::filesystem::path(options.smokeDirectory) / "export-plugin-source.xopp";
        const auto oldSaveTarget = saveTarget;
        saveTarget = source.string();

        prototype::Ink sample;
        sample.page = 0;
        sample.layerId = state.activeLayerId(0);
        sample.originLayer = state.activeLayerOrigin(0);
        sample.layerOrder = 0;
        sample.brush = {Tool::Pen, {1, 0, 0}, 8};
        sample.points = {{120, 150, 1}, {220, 150, 1}};
        state.ink.push_back(sample);

        for (size_t action = 0; action < 3; ++action) {
            require(pluginRuntime->invokeMenu("Export", action),
                    "Bundled Export menu action did not dispatch through the GTK 4 Lua host");
            waitForPluginDocumentJob();
        }

        const auto stem = source.parent_path() / source.stem();
        auto pdfPath = stem;
        pdfPath += "_export.pdf";
        auto [exportedPdf, sizes] = openPreviewPdf(pdfPath.string());
        require(sizes.size() == 2 && std::filesystem::file_size(pdfPath) > 1000,
                "Export plugin did not produce the expected two-page PDF");
        g_object_unref(exportedPdf);

        for (int page = 1; page <= 2; ++page) {
            auto svgPath = stem;
            svgPath += "_export-" + std::to_string(page) + ".svg";
            require(std::filesystem::is_regular_file(svgPath) && std::filesystem::file_size(svgPath) > 500,
                    "Export plugin did not produce one SVG file per page");

            auto pngPath = stem;
            pngPath += "_export-" + std::to_string(page) + ".png";
            auto* surface = cairo_image_surface_create_from_png(pngPath.c_str());
            require(cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS,
                    "Export plugin did not produce a readable PNG page");
            const int width = cairo_image_surface_get_width(surface);
            const int height = cairo_image_surface_get_height(surface);
            require(width > 1000 && height > 1000,
                    "Default PNG export did not use the master 300-DPI output size");
            if (page == 1) {
                cairo_surface_flush(surface);
                const auto* pixels = cairo_image_surface_get_data(surface);
                const int stride = cairo_image_surface_get_stride(surface);
                size_t redPixels = 0;
                const int x0 = static_cast<int>(120.0 * 300.0 / 72.0);
                const int x1 = static_cast<int>(220.0 * 300.0 / 72.0);
                const int y = static_cast<int>(150.0 * 300.0 / 72.0);
                for (int x = x0; x < x1; ++x) {
                    const auto* pixel = pixels + y * stride + x * 4;
                    if (pixel[2] > 180 && pixel[1] < 100 && pixel[0] < 100) {
                        ++redPixels;
                    }
                }
                require(redPixels > 20,
                        "Export plugin did not include unsaved GTK 4 ink in its PNG output");
            }
            cairo_surface_destroy(surface);
        }

        state.ink.pop_back();
        saveTarget = oldSaveTarget;
        update();
    }

    void verifyFitToContentPlugin() {
        const int originalPage = state.page;
        const auto originalInk = state.ink;
        const auto originalSelection = state.selected;
        const auto originalTextSelection = state.selectedTexts;
        const auto originalImageSelection = state.selectedImages;
        const auto originalRevision = state.revision();
        require(nativeSession && !nativeSession->pdfPageMap.empty(),
                "FitToContent verification requires a native multi-page document");
        const auto blankPage = std::find(nativeSession->pdfPageMap.begin(), nativeSession->pdfPageMap.end(), -1);
        require(blankPage != nativeSession->pdfPageMap.end(),
                "FitToContent verification requires an inserted blank note page");
        const int page = static_cast<int>(std::distance(nativeSession->pdfPageMap.begin(), blankPage));
        navigateToPage(page);
        require(state.page == page, "FitToContent verification could not select the inserted note page");
        const auto originalSize = state.pageSize(page);
        const auto originalLayer = state.activeLayerId(page);
        require(originalLayer > 0, "FitToContent verification requires a foreground layer");
        const auto originalLayerName = state.layersForPage(page).layers.at(originalLayer - 1).name;
        require(state.layerVisible(page, originalLayer),
                "FitToContent verification requires a visible foreground layer");

        state.ink.clear();
        state.selected.clear();
        state.selectedTexts.clear();
        state.selectedImages.clear();
        prototype::Ink content;
        content.page = page;
        content.layerId = originalLayer;
        content.originLayer = state.activeLayerOrigin(page);
        content.layerOrder = 0;
        content.brush = {Tool::Pen, {0.15, 0.35, 0.75}, 4};
        content.points = {{100, 120, 1}, {220, 180, 1}};
        state.ink.push_back(content);

        require(pluginRuntime->invokeMenu("FitToContent", 0),
                "FitToContent layer action did not execute through the plugin menu");
        const auto fitted = state.pageSize(page);
        require(std::abs(fitted.width - 124) < 1e-8 && std::abs(fitted.height - 64) < 1e-8,
                "FitToContent did not account for stroke width when resizing the page");
        capture("fit-to-content-page");
        const auto& layers = state.layersForPage(page);
        require(layers.layers.size() >= originalLayer + 1 &&
                        layers.layers[originalLayer - 1].name == originalLayerName &&
                        !layers.layers[originalLayer - 1].visible &&
                        state.activeLayerId(page) == originalLayer + 1 &&
                        layers.layers[originalLayer].name == "moved for fitting" &&
                        layers.layers[originalLayer].visible,
                "FitToContent did not hide the source layer and name/select its moved-content layer");
        require(state.ink.size() == 2 && state.ink[0].points.front().x == 100 &&
                        std::abs(state.ink[1].points.front().x - 2) < 1e-8 &&
                        std::abs(state.ink[1].points.front().y - 2) < 1e-8 &&
                        std::abs(state.ink[1].points.back().x - 122) < 1e-8 &&
                        std::abs(state.ink[1].points.back().y - 62) < 1e-8 &&
                        state.ink[1].layerId == originalLayer + 1,
                "FitToContent did not move a copy of the layer strokes into fitted coordinates");

        const auto roundTripPath =
                (std::filesystem::path(options.smokeDirectory) / "fit-to-content-round-trip.xopp").string();
        const auto packets = prototype::bridge::transferInk(state.committedInk());
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto pageLayers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        nativeSession->saveCopy(options.worker, roundTripPath, packets, texts, images, pageLayers, pageSizes);
        const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
        require(reopened.initialPageSizes.size() == state.pages.size() &&
                        std::abs(reopened.initialPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                        std::abs(reopened.initialPageSizes[static_cast<size_t>(page)].height - fitted.height) < 1e-8 &&
                        reopened.initialInk.size() == 2,
                "Saving and reopening FitToContent did not preserve fitted page dimensions and strokes");

        const auto nativeSavePath =
                (std::filesystem::path(options.smokeDirectory) / "fit-to-content-native-save.xopp").string();
        nativeSession->save(options.worker, nativeSavePath, packets, texts, images, pageLayers, pageSizes,
                            prototype::bridge::documentVersion(nativeSavePath).token);
        const prototype::bridge::DocumentSession reopenedNativeSave(options.worker, nativeSavePath);
        require(std::abs(reopenedNativeSave.initialPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                        std::abs(reopenedNativeSave.initialPageSizes[static_cast<size_t>(page)].height - fitted.height) <
                                1e-8,
                "Normal Save did not persist the fitted page dimensions");

        const auto autosavePath =
                (std::filesystem::path(options.smokeDirectory) / "fit-to-content.autosave.xopp").string();
        nativeSession->autosave(options.worker, autosavePath, packets, texts, images, pageLayers, pageSizes);
        const prototype::bridge::DocumentSession reopenedAutosave(options.worker, autosavePath);
        require(std::abs(reopenedAutosave.initialPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                        std::abs(reopenedAutosave.initialPageSizes[static_cast<size_t>(page)].height - fitted.height) <
                                1e-8,
                "Autosave did not persist the fitted page dimensions");

        const auto exportPath = (std::filesystem::path(options.smokeDirectory) / "fit-to-content.pdf").string();
        nativeSession->exportPdf(options.worker, exportPath, packets, texts, images, pageLayers, pageSizes);
        auto [exportedPdf, exportedSizes] = openPreviewPdf(exportPath);
        require(exportedSizes.size() == state.pages.size() &&
                        std::abs(exportedSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                        std::abs(exportedSizes[static_cast<size_t>(page)].height - fitted.height) < 1e-8,
                "PDF export did not use the fitted page dimensions");
        g_object_unref(exportedPdf);

        const auto insertedRevision = nativeSession->stageInsertPageAfter(options.worker, page, pageSizes);
        const auto insertedPageSizes = prototype::bridge::readPageSizeTransfer(insertedRevision.pageSizesPath());
        prototype::bridge::DocumentSession::discardRevision(insertedRevision);
        require(insertedPageSizes.size() == state.pages.size() + 1 &&
                        std::abs(insertedPageSizes[static_cast<size_t>(page)].width - fitted.width) < 1e-8 &&
                        std::abs(insertedPageSizes[static_cast<size_t>(page)].height - fitted.height) < 1e-8 &&
                        std::abs(insertedPageSizes[static_cast<size_t>(page + 1)].width - fitted.width) < 1e-8 &&
                        std::abs(insertedPageSizes[static_cast<size_t>(page + 1)].height - fitted.height) < 1e-8,
                "Page insertion did not preserve the fitted page and inherit its dimensions");

        for (int edit = 0; edit < 5; ++edit) {
            require(state.canUndo(), "FitToContent lost one of its five grouped undo steps");
            state.undo();
        }
        require(state.revision() == originalRevision && state.pageSize(page).width == originalSize.width &&
                        state.pageSize(page).height == originalSize.height && state.ink.size() == 1 &&
                        state.layersForPage(page).layers[originalLayer - 1].visible,
                "Undo did not restore the document state from before FitToContent");
        for (int edit = 0; edit < 5; ++edit) {
            require(state.canRedo(), "FitToContent undo history could not be replayed");
            state.redo();
        }
        require(std::abs(state.pageSize(page).width - fitted.width) < 1e-8 && state.ink.size() == 2,
                "Redo did not restore FitToContent's page size and moved stroke");
        for (int edit = 0; edit < 5; ++edit) {
            state.undo();
        }

        state.selected = {0};
        require(pluginRuntime->invokeMenu("FitToContent", 1),
                "FitToContent selection action did not execute through the plugin menu");
        require(std::abs(state.pageSize(page).width - fitted.width) < 1e-8 &&
                        std::abs(state.pageSize(page).height - fitted.height) < 1e-8 && state.ink.size() == 2,
                "FitToContent selection action did not fit the selected stroke");
        for (int edit = 0; edit < 5; ++edit) {
            require(state.canUndo(), "FitToContent selection action lost one of its undo steps");
            state.undo();
        }
        require(state.revision() == originalRevision && state.pageSize(page).width == originalSize.width &&
                        state.pageSize(page).height == originalSize.height && state.ink.size() == 1,
                "Undo did not restore the document state from before the selection fit");

        state.ink = originalInk;
        state.selected = originalSelection;
        state.selectedTexts = originalTextSelection;
        state.selectedImages = originalImageSelection;
        navigateToPage(originalPage);
        update();
    }

    void verifyBeamerIntegration() {
        require(pluginRuntime != nullptr, "Lua plugin runtime was not created");
        const auto beamer = pluginRuntime->findPlugin("BeamerPresentation");
        require(beamer && beamer->loaded && beamer->menuItems == 3,
                "Enabled BeamerPresentation did not register its three menu actions");
        require(nativeSession && pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
                "Beamer integration expects a two-page animation PDF");
        require(nativeSession->pdfPageLabels.size() >= 2 &&
                        nativeSession->pdfPageLabels[0] == nativeSession->pdfPageLabels[1],
                "Beamer fixture must use repeated PDF page labels");

        capture("beamer-animation-start");
        require(pluginRuntime->invokeMenu("BeamerPresentation", 0),
                "Beamer +1 did not dispatch its next-animation action");
        waitForPluginDocumentJob();
        require(pageCount == 2 && state.page == 0 && nativeSession->pdfPageMap == std::vector<int>({1, 1}),
                "Beamer +1 did not advance only the current PDF background during an animation");
        capture("beamer-animation-forward");

        require(pluginRuntime->invokeMenu("BeamerPresentation", 1),
                "Beamer -1 did not dispatch its previous-animation action");
        waitForPluginDocumentJob();
        require(pageCount == 2 && state.page == 0 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
                "Beamer -1 did not restore the previous PDF animation background");

        require(pluginRuntime->invokeMenu("BeamerPresentation", 2),
                "Beamer clean did not dispatch its animation cleanup action");
        waitForPluginDocumentJob();
        require(pageCount == 1 && nativeSession->pdfPageMap == std::vector<int>({0}),
                "Beamer clean did not remove the duplicate-label animation page");
        capture("beamer-animation-cleaned");

        const auto roundTripPath = std::filesystem::path(options.smokeDirectory) / "beamer-cleaned.xopp";
        nativeSession->saveCopy(options.worker, roundTripPath.string(),
                                prototype::bridge::transferInk(state.committedInk()), state.texts,
                                prototype::bridge::transferImages(state.images), state.pageLayers,
                                currentPageSizes(state));
        const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath.string());
        require(reopened.pdfPageMap == std::vector<int>({0}) && reopened.pdfPageLabels.size() >= 2 &&
                        reopened.pdfPageLabels[0] == reopened.pdfPageLabels[1],
                "Saving and reopening the cleaned Beamer document lost its page or attached-PDF labels");
        std::cout << "PASS: private GTK4 Beamer animation advance, reverse, cleanup, and native reopen\n";
    }

    void verifyLayerActionsIntegration() {
        require(pluginRuntime != nullptr, "Lua plugin runtime was not created");
        const auto layerActions = pluginRuntime->findPlugin("LayerActions");
        require(layerActions && layerActions->loaded && layerActions->menuItems == 3,
                "Enabled LayerActions plugin did not register all three menu actions");
        require(nativeSession && pageCount == 2 && nativeSession->pdfPageMap == std::vector<int>({0, 1}),
                "LayerActions integration expects a two-page PDF document");

        for (int page = 0; page < pageCount; ++page) {
            state.selectActivePage(page);
            require(state.addLayerAboveCurrent() && state.addLayerAboveCurrent(),
                    "LayerActions fixture could not create three foreground layers per page");
        }
        state.setLayerVisible(0, 0, false);
        state.setLayerVisible(0, 2, false);
        state.selectActivePage(0);
        navigateToPage(0);

        Ink sourceStroke;
        sourceStroke.page = 0;
        sourceStroke.layerId = 3;
        sourceStroke.originLayer = 2;
        sourceStroke.layerOrder = 0;
        sourceStroke.brush = {Tool::Pen, {1, 0, 0}, 3};
        sourceStroke.points = {{35, 45, 1}, {90, 100, 1}};
        Ink nextPageAnnotation = sourceStroke;
        nextPageAnnotation.page = 1;
        nextPageAnnotation.layerId = 2;
        nextPageAnnotation.originLayer = 1;
        nextPageAnnotation.brush.color = {0, 0, 1};
        nextPageAnnotation.points = {{160, 45, 1}, {210, 100, 1}};
        state.ink = {sourceStroke, nextPageAnnotation};

        require(pluginRuntime->invokeMenu("LayerActions", 0),
                "LayerActions clone did not start its target-page check");
        waitForPluginDocumentJob();
        require(messageDialog &&
                        g_strcmp0(adw_alert_dialog_get_response_label(messageDialog, "plugin-response-1"), "Cancel") ==
                                0 &&
                        g_strcmp0(adw_alert_dialog_get_response_label(messageDialog, "plugin-response-2"), "Proceed") ==
                                0,
                "LayerActions did not ask before replacing an annotated next page");
        answerDialog("plugin-response-1");
        require(messageDialog == nullptr && pageCount == 2 && state.ink.size() == 2 &&
                        nativeSession->pdfPageMap == std::vector<int>({0, 1}),
                "Canceling LayerActions clone modified the existing next page");

        require(pluginRuntime->invokeMenu("LayerActions", 0),
                "LayerActions clone could not be retried after cancellation");
        waitForPluginDocumentJob();
        require(messageDialog != nullptr, "LayerActions omitted the second annotated-page confirmation");
        answerDialog("plugin-response-2");
        waitForPluginDocumentJob();
        require(pageCount == 2 && state.page == 1 && nativeSession->pdfPageMap == std::vector<int>({0, 1}) &&
                        state.ink.size() == 2 && state.ink[0].page == 0 && state.ink[1].page == 1 &&
                        state.ink[0].layerId == 3 && state.ink[1].layerId == 3 &&
                        !state.pageLayers[0].backgroundVisible && state.pageLayers[1].backgroundVisible &&
                        !state.pageLayers[0].layers[1].visible && state.pageLayers[1].layers[1].visible,
                "Proceed did not replace the annotated page with a clone of the source and its next PDF background");
        capture("layer-actions-cloned");

        const auto cloneRoundTripPath =
                (std::filesystem::path(options.smokeDirectory) / "layer-actions-clone-round-trip.xopp").string();
        nativeSession->saveCopy(options.worker, cloneRoundTripPath,
                                prototype::bridge::transferInk(state.committedInk()), state.texts,
                                prototype::bridge::transferImages(state.images), state.pageLayers,
                                currentPageSizes(state));
        const prototype::bridge::DocumentSession cloneReopened(options.worker, cloneRoundTripPath);
        require(cloneReopened.pdfPageMap == std::vector<int>({0, 1}) && cloneReopened.initialInk.size() == 2 &&
                        cloneReopened.initialInk[0].page == 0 && cloneReopened.initialInk[1].page == 1 &&
                        cloneReopened.initialLayers[0].layers.size() == 3 &&
                        cloneReopened.initialLayers[1].layers.size() == 3 &&
                        cloneReopened.initialLayers[0].backgroundVisible &&
                        cloneReopened.initialLayers[1].backgroundVisible,
                "LayerActions clone did not preserve its page layers and editable content through save/reopen");

        require(pluginRuntime->invokeMenu("LayerActions", 0),
                "LayerActions clone did not start its final-page check");
        waitForPluginDocumentJob();
        require(messageDialog &&
                        g_strcmp0(adw_alert_dialog_get_response_label(messageDialog, "plugin-response-1"), "OK") == 0,
                "LayerActions did not report that the final page has no successor");
        answerDialog("plugin-response-1");

        require(pluginRuntime->invokeMenu("LayerActions", 1), "LayerActions hide did not execute");
        require(state.page == 1 && state.pageLayers.size() == 2 &&
                        std::all_of(state.pageLayers.begin(), state.pageLayers.end(), [](const prototype::PageLayers& page) {
                            return page.selectedLayer == 1 && page.layers[0].visible && !page.layers[1].visible &&
                                   !page.layers[2].visible;
                        }) && !state.pageLayers[0].backgroundVisible && state.pageLayers[1].backgroundVisible,
                "LayerActions hide did not select layer one, hide higher layers, and preserve background visibility");

        require(pluginRuntime->invokeMenu("LayerActions", 2), "LayerActions add did not execute");
        require(state.page == 1 && state.pageLayers[0].layers.size() == 4 &&
                        state.pageLayers[1].layers.size() == 4 && state.pageLayers[0].selectedLayer == 4 &&
                        state.pageLayers[1].selectedLayer == 4,
                "LayerActions add did not create and select a top layer on each page");
        state.undo();
        require(state.pageLayers[0].layers.size() == 4 && state.pageLayers[1].layers.size() == 3,
                "Undo did not revert the last per-page LayerActions layer addition");
        state.undo();
        require(state.pageLayers[0].layers.size() == 3 && state.pageLayers[1].layers.size() == 3,
                "Undo did not revert the first per-page LayerActions layer addition");
        state.redo();
        state.redo();
        require(state.pageLayers[0].layers.size() == 4 && state.pageLayers[1].layers.size() == 4,
                "Redo did not restore both per-page LayerActions layer additions");
        capture("layer-actions-complete");

        const auto roundTripPath =
                (std::filesystem::path(options.smokeDirectory) / "layer-actions-round-trip.xopp").string();
        nativeSession->saveCopy(options.worker, roundTripPath, prototype::bridge::transferInk(state.committedInk()),
                                state.texts, prototype::bridge::transferImages(state.images), state.pageLayers,
                                currentPageSizes(state));
        const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
        require(reopened.initialLayers.size() == 2 && reopened.initialLayers[0].layers.size() == 4 &&
                        reopened.initialLayers[1].layers.size() == 4 && reopened.initialInk.size() == 2,
                "LayerActions added top layers and cloned editable content did not persist through native save/reopen");
        std::cout << "PASS: private GTK4 LayerActions annotated-page confirmation, clone, hide, add, undo/redo, and "
                     "native reopen\n";
    }

    void verifyLatexIntegration() {
        require(nativeSession != nullptr, "LaTeX verification requires a native XOPP document");
        require(!latexEditor, "LaTeX editor should not already be open");

        constexpr int page = 0;
        state.selectActivePage(page);
        const auto pageSize = state.pageSize(page);
        prototype::Point insertionPoint{};
        bool foundEmptyPoint = false;
        for (double y = 48; y < pageSize.height - 48 && !foundEmptyPoint; y += 48) {
            for (double x = 48; x < pageSize.width - 48; x += 48) {
                if (!latexTargetAt(page, {x, y}).imageIndex && !latexTargetAt(page, {x, y}).textIndex) {
                    insertionPoint = {x, y};
                    foundEmptyPoint = true;
                    break;
                }
            }
        }
        require(foundEmptyPoint, "LaTeX verification could not find an unused insertion point");

        const auto originalImageCount = state.images.size();
        const auto expectedSource = importedConfig.settings.latexUseExternalEditor ?
                                            std::string("\\frac{1}{2}") :
                                            importedConfig.settings.latexDefaultText;
        selectTool(Tool::Latex);
        const auto screenPoint = state.pageScreenPoint(page, insertionPoint.x, insertionPoint.y);
        openLatexEditorAt(screenPoint.x, screenPoint.y);
        require(latexEditor != nullptr, "The LaTeX tool did not open its editor");

        const gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
        while (latexEditor && !latexEditor->readyForInsertion() && g_get_monotonic_time() < deadline) {
            while (g_main_context_iteration(nullptr, false)) {
            }
            g_usleep(1000);
        }
        require(latexEditor && latexEditor->readyForInsertion(),
                "LaTeX did not compile successfully within 30 seconds");
        latexEditor->activateInsert();
        require(!latexEditor && state.images.size() == originalImageCount + 1,
                "Inserting a compiled formula did not create one editable image");

        const size_t insertedIndex = state.images.size() - 1;
        const auto insertedFormula = state.images[insertedIndex];
        require(insertedFormula.kind == prototype::CanvasImageKind::Latex &&
                        insertedFormula.latexSource == expectedSource,
                "Inserted LaTeX image lost its editable source");
        require(prototype::isCanvasTexPdf(insertedFormula.encodedData),
                "Inserted LaTeX image did not retain a vector PDF");
        require(insertedFormula.nativeWidth > 0 && insertedFormula.nativeHeight > 0,
                "Inserted LaTeX image has invalid vector dimensions");
        require(std::abs(insertedFormula.transform.x - insertionPoint.x) < 1e-6 &&
                        std::abs(insertedFormula.transform.y - insertionPoint.y) < 1e-6,
                "Inserted LaTeX image moved away from the click position");
        waitForNextWindowFrame();
        capture("latex-formula-inserted");

        selectTool(Tool::Latex);
        const auto center = prototype::canvasImageCenter(insertedFormula);
        const auto editPoint = state.pageScreenPoint(page, center.x, center.y);
        openLatexEditorAt(editPoint.x, editPoint.y);
        const gint64 editDeadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
        while (latexEditor && !latexEditor->readyForInsertion() && g_get_monotonic_time() < editDeadline) {
            while (g_main_context_iteration(nullptr, false)) {
            }
            g_usleep(1000);
        }
        require(latexEditor && latexEditor->readyForInsertion(),
                "The LaTeX tool did not reopen the inserted formula for editing");
        latexEditor->activateInsert();
        require(!latexEditor && state.images.size() == originalImageCount + 1 &&
                        state.images[insertedIndex].latexSource == insertedFormula.latexSource &&
                        state.images[insertedIndex].transform.x == insertedFormula.transform.x &&
                        state.images[insertedIndex].transform.y == insertedFormula.transform.y,
                "Editing an existing formula changed its placement or editable source");

        state.undo();
        require(state.images.size() == originalImageCount + 1 &&
                        state.images[insertedIndex].encodedData == insertedFormula.encodedData && state.canUndo(),
                "Undo did not restore the formula before its in-place edit");
        state.undo();
        require(state.images.size() == originalImageCount && state.canRedo(),
                "A second undo did not remove the inserted LaTeX formula");
        state.redo();
        state.redo();
        require(state.images.size() == originalImageCount + 1 &&
                        state.images[insertedIndex].latexSource == expectedSource,
                "Redo did not restore the inserted LaTeX formula");

        const auto roundTripPath = (std::filesystem::path(options.smokeDirectory) / "latex-round-trip.xopp").string();
        nativeSession->saveCopy(options.worker, roundTripPath,
                                prototype::bridge::transferInk(state.committedInk()), state.texts,
                                prototype::bridge::transferImages(state.images), state.pageLayers,
                                currentPageSizes(state));
        const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
        const auto reopenedFormula = std::find_if(
                reopened.initialImages.begin(), reopened.initialImages.end(),
                [&expectedSource, &insertedFormula, page](const prototype::bridge::ImagePacket& image) {
                    return image.kind == prototype::CanvasImageKind::Latex && image.latexSource == expectedSource &&
                           image.page == page &&
                           std::abs(image.transform.x - insertedFormula.transform.x) < 1e-3 &&
                           std::abs(image.transform.y - insertedFormula.transform.y) < 1e-3;
                });
        require(reopenedFormula != reopened.initialImages.end() &&
                        prototype::isCanvasTexPdf(reopenedFormula->encodedData) &&
                        std::abs(reopenedFormula->nativeWidth - insertedFormula.nativeWidth) < 1e-3 &&
                        std::abs(reopenedFormula->nativeHeight - insertedFormula.nativeHeight) < 1e-3,
                "Native save/reopen did not preserve the LaTeX source, PDF or geometry");
        std::cout << "PASS: private GTK4 LaTeX compile, insert, edit, undo/redo and native round trip\n";
    }

    void verifyImageActionsPlugin() {
        require(state.images.empty(), "ImageActions verification expects a clean editable image layer");
        const auto fixture = std::filesystem::path(options.smokeDirectory) / "image-actions-input.png";
        auto source = prototype::readCanvasImageFile(fixture);
        source.page = 0;
        source.transform = {1, 0, 0, 1, 35, 45};
        require(state.insertImage(source), "ImageActions integration could not insert its synthetic image");
        const auto sourceIndex = state.images.size() - 1;
        const auto original = state.images[sourceIndex];

        if (!prototype::canvasImageActionsAvailable()) {
            state.selectedImages = {sourceIndex};
            require(!pluginRuntime->invokeMenu("ImageActions", 0) && messageDialog,
                    "ImageActions without libvips should report its missing dependency");
            answerDialog("close");
            state.undo();
            require(state.images.empty(), "Unavailable ImageActions should leave the selected image unchanged");
            return;
        }

        for (std::size_t mode = 0; mode < 10; ++mode) {
            state.selectedImages = {sourceIndex};
            require(pluginRuntime->invokeMenu("ImageActions", mode),
                    "ImageActions menu operation failed to invoke its GTK host callback");
            const auto selectedCount = (mode == 8 || mode == 9) ? 2U : 1U;
            require(state.selectedImages.size() == selectedCount,
                    "ImageActions did not select all outputs from the grouped operation");
            if (mode == 8) {
                capture("image-actions-split");
                const auto roundTripPath =
                        (std::filesystem::path(options.smokeDirectory) / "image-actions-round-trip.xopp").string();
                nativeSession->saveCopy(options.worker, roundTripPath,
                                        prototype::bridge::transferInk(state.committedInk()), state.texts,
                                        prototype::bridge::transferImages(state.images), state.pageLayers,
                                        currentPageSizes(state));
                const prototype::bridge::DocumentSession reopened(options.worker, roundTripPath);
                require(reopened.initialImages.size() == 2,
                        "Saving and reopening a split image group did not preserve both image parts");
            }

            const auto changedImages = state.images;
            require(state.canUndo(), "ImageActions transformation should be undoable");
            state.undo();
            require(state.images.size() == sourceIndex + 1 &&
                            state.images[sourceIndex].encodedData == original.encodedData &&
                            state.selectedImages == std::vector<size_t>{sourceIndex},
                    "One ImageActions undo should restore its original image and selection");
            require(state.canRedo(), "ImageActions transformation should be redoable");
            state.redo();
            require(state.images.size() == changedImages.size() && state.selectedImages.size() == selectedCount,
                    "One ImageActions redo should restore the complete transformed image group");
            for (size_t index = 0; index < changedImages.size(); ++index) {
                require(state.images[index].encodedData == changedImages[index].encodedData &&
                                state.images[index].pixelWidth == changedImages[index].pixelWidth &&
                                state.images[index].pixelHeight == changedImages[index].pixelHeight,
                        "ImageActions redo changed a transformed output");
            }
            state.undo();
        }
        state.undo();
        require(state.images.empty(), "ImageActions verification should remove its fixture with one undo");
    }

    void waitForPluginDocumentJob() {
        const gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
        while (busy && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(!busy, "ToggleGrid native page-background revision did not finish within 20 seconds");
        waitForNextWindowFrame();
    }

    static guint settingsThemeIndex(prototype::ThemeVariant theme) {
        switch (theme) {
            case prototype::ThemeVariant::ForceLight:
                return 1;
            case prototype::ThemeVariant::ForceDark:
                return 2;
            case prototype::ThemeVariant::UseSystem:
            default:
                return 0;
        }
    }

    void runDocumentJob(const char* status, std::function<void()> work, std::function<void()> success,
                        bool initial = false) {
        if (busy) {
            return;
        }
        resetInput(true);
        busy = true;
        busyText = status;
        gtk_widget_set_sensitive(contentView, false);
        updateTitle();
        auto* application = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
        g_application_hold(application);
        prototype::bridge::documentJob(std::move(work), [this, application, success = std::move(success), initial](
                                                               const std::string& error) {
            busy = false;
            gtk_widget_set_sensitive(contentView, true);
            try {
                if (!error.empty()) {
                    throw std::runtime_error(error);
                }
                success();
            } catch (const std::exception& exception) {
                if (initial && !options.smokeDirectory.empty()) {
                    failed = true;
                    std::cerr << "Cannot load test document: " << exception.what() << '\n';
                    g_application_quit(application);
                } else {
                    showError(exception.what());
                }
            }
            lastDocumentCompletion = g_get_monotonic_time();
            if (!closed) {
                update();
            }
            g_application_release(application);
        });
    }

    void replaceDocument(const std::shared_ptr<prototype::bridge::DocumentSession>& session, const std::string& source,
                         const std::optional<RecoveryInfo>& recovery = std::nullopt) {
        auto [candidate, pageSizes] = openPreviewPdf(session->previewPath().string());
        const int count = static_cast<int>(pageSizes.size());
        std::vector<PopplerDocument*> candidateLayers;
        std::vector<PopplerDocument*> candidateAudioLayers;
        auto [hydratedInk, hydratedImages] = [&] {
            try {
                candidateLayers = openLayerPreviewPdfs(session->layerPreviewPaths, count);
                candidateAudioLayers = openAudioLayerPreviewPdfs(session->audioLayerPreviewPaths, count);
                auto ink = prototype::bridge::sessionInk(session->initialInk);
                std::vector<prototype::CanvasImage> images;
                images.reserve(session->initialImages.size());
                for (const auto& packet: session->initialImages) {
                    images.push_back(prototype::hydrateCanvasImage(prototype::bridge::canvasImage(packet)));
                }
                return std::pair{std::move(ink), std::move(images)};
            } catch (...) {
                g_object_unref(candidate);
                releaseLayerPreviewPdfs(candidateLayers);
                releaseLayerPreviewPdfs(candidateAudioLayers);
                throw;
            }
        }();
        // Commit the replacement only after the new native snapshot and preview
        // are valid. Failed opens leave the old document and undo history intact.
        if (!recovery || recovery->autosavePath != activeAutosavePath) {
            cleanupActiveAutosave();
        }
        input.reset();
        imageGesture.reset();
        selectedImageIndex.reset();
        const auto brush = state.brush;
        const auto eraserMode = state.eraserMode;
        const auto eraserWidth = state.eraserWidth;
        state = CanvasState{};
        prototype::applyImportedSettingsToCanvas(state, importedConfig.settings);
        state.backgroundChangedCallback = [this](int page) { refreshNativeDocumentRevision(page); };
        state.setPages(std::move(pageSizes));
        state.selectActivePage(0);
        state.setLayerDocument(session->initialLayers);
        state.ink = std::move(hydratedInk);
        state.texts = session->initialTexts;
        state.images = std::move(hydratedImages);
        state.brush = brush;
        state.eraserMode = eraserMode;
        state.eraserWidth = eraserWidth;
        savedRevision = recovery ? std::optional<size_t>{} : std::optional<size_t>{0};
        autosavedRevision = 0;
        recoveredChanges = bool(recovery);
        activeAutosavePath = recovery ? recovery->autosavePath : std::string{};
        savedCopyPath.clear();
        exportedPdfPath.clear();
        savedNormally = false;
        g_clear_object(&document);
        document = candidate;
        releaseLayerPreviewPdfs(layerDocuments);
        layerDocuments = std::move(candidateLayers);
        releaseLayerPreviewPdfs(audioLayerDocuments);
        audioLayerDocuments = std::move(candidateAudioLayers);
        audioTargets = session->initialAudioTargets;
        pageCount = count;
        nativeSession = session;
        sourcePath = source;
        saveTarget = recovery ? recovery->saveTarget : session->originalSavePath;
        saveVersion = recovery ? recovery->saveVersion : session->originalVersion;
        pageCache.clear();
        layerPageCache.clear();
        thumbnails.clear();
        thumbnailWidgets.clear();
        pageList = nullptr;
        adw_overlay_split_view_set_sidebar(splitView, createSidebar());
        fitMode = true;
        fitted = false;
        if (canvas) {
            update();
        }
    }

    void openDocument(const std::string& path, bool initial = false) {
        if (autosaving) {
            waitForAutosave([this, path, initial] { openDocument(path, initial); });
            return;
        }
        auto loaded = std::make_shared<std::shared_ptr<prototype::bridge::DocumentSession>>();
        const auto worker = options.worker;
        const auto audioFolder = importedConfig.settings.audioFolder;
        runDocumentJob(
                "Opening document…",
                [loaded, worker, path, audioFolder] {
                    *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, path, audioFolder);
                },
                [this, loaded, path, initial] {
                    replaceDocument(*loaded, path);
                    if (initial && path.empty() && !checkedStartupRecovery) {
                        checkedStartupRecovery = true;
                        checkStartupRecovery();
                    }
                },
                initial);
    }

    void recoverSidecarAutosave(const std::string& path, const std::string& sidecar, bool initial) {
        auto loaded = std::make_shared<std::shared_ptr<prototype::bridge::DocumentSession>>();
        auto targetSave = std::make_shared<std::string>();
        auto targetVersion = std::make_shared<std::string>();
        const auto worker = options.worker;
        const auto audioFolder = importedConfig.settings.audioFolder;
        runDocumentJob(
                "Recovering autosave…",
                [loaded, targetSave, targetVersion, worker, path, sidecar, audioFolder] {
                    *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, sidecar, audioFolder);
                    std::error_code statusError;
                    if (g_ascii_strcasecmp(std::filesystem::path(path).extension().string().c_str(), ".xopp") == 0 &&
                        std::filesystem::is_regular_file(std::filesystem::symlink_status(path, statusError)) &&
                        !statusError) {
                        *targetSave = path;
                        *targetVersion = prototype::bridge::documentVersion(path).token;
                    }
                },
                [this, loaded, targetSave, targetVersion, path, sidecar] {
                    replaceDocument(*loaded, path, RecoveryInfo{*targetSave, *targetVersion, sidecar});
                },
                initial);
    }

    void promptSidecarRecovery(const std::string& path, const std::string& sidecar, bool initial) {
        const auto body = "An autosave file exists for \"" + std::filesystem::path(path).filename().string() +
                          "\". Recover unsaved changes from the autosave file, or discard it and open the saved "
                          "document?";
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Autosave file detected", body.c_str()));
        adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "discard", "Discard Autosave", "recover",
                                       "Recover", nullptr);
        adw_alert_dialog_set_response_appearance(messageDialog, "discard", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_appearance(messageDialog, "recover", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(messageDialog, "cancel");
        adw_alert_dialog_set_default_response(messageDialog, "recover");
        struct SidecarPrompt {
            Prototype* self;
            std::string path;
            std::string sidecar;
            bool initial;
        };
        g_signal_connect_data(
                messageDialog, "response",
                G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                    auto* prompt = static_cast<SidecarPrompt*>(data);
                    auto* self = prompt->self;
                    self->messageDialog = nullptr;
                    const std::string reply = response ? response : "";
                    if (reply == "discard") {
                        prototype::bridge::removeAutosaveFiles(prompt->sidecar);
                        self->openDocument(prompt->path, prompt->initial);
                    } else if (reply == "recover") {
                        self->recoverSidecarAutosave(prompt->path, prompt->sidecar, prompt->initial);
                    } else if (prompt->initial && !self->nativeSession) {
                        self->openDocument("", true);
                    }
                }),
                new SidecarPrompt{this, path, sidecar, initial},
                +[](gpointer data, GClosure*) { delete static_cast<SidecarPrompt*>(data); }, GConnectFlags(0));
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
    }

    void restoreStartupRecovery(const std::string& recoveryPath) {
        auto loaded = std::make_shared<std::shared_ptr<prototype::bridge::DocumentSession>>();
        const auto adopted =
                prototype::bridge::autosavePathForTarget({}, cacheRoot(), static_cast<long long>(::getpid())).string();
        const auto worker = options.worker;
        const auto audioFolder = importedConfig.settings.audioFolder;
        runDocumentJob(
                "Restoring recovery file…",
                [loaded, worker, recoveryPath, adopted, audioFolder] {
                    *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, recoveryPath, audioFolder);
                    (*loaded)->autosave(worker, adopted, (*loaded)->initialInk, (*loaded)->initialTexts,
                                        (*loaded)->initialImages, (*loaded)->initialLayers,
                                        (*loaded)->initialPageSizes);
                    if (adopted != recoveryPath) {
                        prototype::bridge::removeAutosaveFiles(recoveryPath);
                    }
                },
                [this, loaded, adopted] { replaceDocument(*loaded, "", RecoveryInfo{"", "", adopted}); });
    }

    void checkStartupRecovery() {
        const auto found =
                prototype::bridge::findStartupRecoveryFile(configRoot(), cacheRoot(), static_cast<long long>(::getpid()));
        if (!found || busy || choosingFile || messageDialog) {
            return;
        }
        const auto recoveryPath = found->string();
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
                "Recovery file detected",
                "Xournal++ did not exit cleanly last time. Would you like to restore the unsaved recovery file?"));
        adw_alert_dialog_add_responses(messageDialog, "delete", "Delete file", "restore", "Restore file", nullptr);
        adw_alert_dialog_set_response_appearance(messageDialog, "delete", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_appearance(messageDialog, "restore", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(messageDialog, "delete");
        adw_alert_dialog_set_default_response(messageDialog, "restore");
        struct RecoveryPrompt {
            Prototype* self;
            std::string path;
        };
        g_signal_connect_data(
                messageDialog, "response",
                G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                    auto* prompt = static_cast<RecoveryPrompt*>(data);
                    auto* self = prompt->self;
                    self->messageDialog = nullptr;
                    const std::string reply = response ? response : "";
                    if (reply == "delete") {
                        prototype::bridge::removeAutosaveFiles(prompt->path);
                    } else if (reply == "restore") {
                        self->restoreStartupRecovery(prompt->path);
                    }
                }),
                new RecoveryPrompt{this, recoveryPath},
                +[](gpointer data, GClosure*) { delete static_cast<RecoveryPrompt*>(data); }, GConnectFlags(0));
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
    }

    void requestOpenDocument(const std::string& path, bool initial = false) {
        if (busy || choosingFile || messageDialog) {
            return;
        }
        if (autosaving) {
            waitForAutosave([this, path, initial] { requestOpenDocument(path, initial); });
            return;
        }
        if (!path.empty() && prototype::bridge::isCacheProtectedPath(path, cacheRoot())) {
            showError("Do not open autosave files directly from the cache folder (" +
                      (cacheRoot() / "xournalpp").string() +
                      "). Copy the file elsewhere or use automatic recovery on startup.");
            return;
        }
        if (!options.worker.empty() && !path.empty() && !prototype::bridge::isRecoveryArtifact(path)) {
            const auto extension = std::filesystem::path(path).extension().string();
            if (g_ascii_strcasecmp(extension.c_str(), ".xopp") == 0 ||
                g_ascii_strcasecmp(extension.c_str(), ".xoj") == 0) {
                const auto sidecar =
                        prototype::bridge::autosavePathForTarget(path, cacheRoot(), static_cast<long long>(::getpid()));
                std::error_code statusError;
                if (sidecar.string() != activeAutosavePath &&
                    std::filesystem::is_regular_file(std::filesystem::symlink_status(sidecar, statusError)) &&
                    !statusError) {
                    promptSidecarRecovery(path, sidecar.string(), initial);
                    return;
                }
            }
        }
        openDocument(path, initial);
    }

    void saveCopyTo(const std::string& path, std::function<void()> after = {}) {
        if (!nativeSession || busy) {
            return;
        }
        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this, path, after = std::move(after)]() mutable {
                saveCopyTo(path, std::move(after));
            });
            return;
        }
        resetInput(true);
        const auto revision = state.revision();
        const auto packets = prototype::bridge::transferInk(state.ink);
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        const auto session = nativeSession;
        const auto worker = options.worker;
        runDocumentJob("Saving copy…", [session, worker, path, packets, texts, images, layers, pageSizes] {
            session->saveCopy(worker, path, packets, texts, images, layers, pageSizes);
        },
                       [this, revision, path, after = std::move(after)] {
            savedRevision = revision;
            autosavedRevision = revision;
            recoveredChanges = false;
            savedCopyPath = path;
            cleanupActiveAutosave();
            if (after) {
                after();
            }
        });
    }

    void save(std::function<void()> after = {}) {
        if (!nativeSession || busy || choosingFile || messageDialog) {
            return;
        }
        if (autosaving) {
            waitForAutosave([this, after = std::move(after)]() mutable { save(std::move(after)); });
            return;
        }
        if (saveTarget.empty()) {
            chooseSave(std::move(after));
        } else {
            saveTo(saveTarget, saveVersion, std::move(after));
        }
    }

    void saveTo(const std::string& path, const std::string& expected = {}, std::function<void()> after = {}) {
        if (!nativeSession || busy) {
            return;
        }
        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this, path, expected, after = std::move(after)]() mutable {
                saveTo(path, expected, std::move(after));
            });
            return;
        }
        resetInput(true);
        const auto revision = state.revision();
        const auto packets = prototype::bridge::transferInk(state.ink);
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto version = std::make_shared<std::string>();
        runDocumentJob("Saving…", [session, worker, path, expected, packets, texts, images, layers, pageSizes, version] {
            const auto before = expected.empty() ? prototype::bridge::documentVersion(path).token : expected;
            *version = session->save(worker, path, packets, texts, images, layers, pageSizes, before);
        }, [this, path, revision, version, after = std::move(after)] {
            saveTarget = path;
            saveVersion = *version;
            sourcePath = path;
            savedNormally = true;
            savedCopyPath.clear();
            savedRevision = revision;
            autosavedRevision = revision;
            recoveredChanges = false;
            cleanupActiveAutosave();
            if (after) {
                after();
            }
        });
    }

    void chooseOpen() {
        if (busy || choosingFile || messageDialog || options.worker.empty()) {
            return;
        }
        if (autosaving) {
            waitForAutosave([this] { chooseOpen(); });
            return;
        }
        choosingFile = true;
        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Open document");
        auto* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "Xournal++ documents and PDFs");
        gtk_file_filter_add_pattern(filter, "*.xopp");
        gtk_file_filter_add_pattern(filter, "*.xoj");
        gtk_file_filter_add_pattern(filter, "*.pdf");
        gtk_file_dialog_set_default_filter(dialog, filter);
        g_object_unref(filter);
        gtk_file_dialog_open(dialog, GTK_WINDOW(window), nullptr, +[](GObject* source, GAsyncResult* result,
                                                                      gpointer data) {
            auto* self = static_cast<Prototype*>(data);
            self->choosingFile = false;
            GError* error = nullptr;
            auto* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
            self->finishFileChoice(file, error, [self](const std::string& path) { self->requestOpenDocument(path); });
        }, this);
        g_object_unref(dialog);
    }

    void chooseSave(std::function<void()> after = {}) {
        if (busy || choosingFile || messageDialog || !nativeSession) {
            return;
        }
        if (autosaving) {
            waitForAutosave([this, after = std::move(after)]() mutable { chooseSave(std::move(after)); });
            return;
        }
        resetInput(true);
        if (!smokeSaveChoice.empty()) {
            const auto path = std::exchange(smokeSaveChoice, {});
            saveTo(path, {}, std::move(after));
            return;
        }
        choosingFile = true;
        struct Choice {
            Prototype* self;
            std::function<void()> after;
        };
        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Save As");
        const auto name = sourcePath.empty() ? std::string("Notebook.xopp") :
                                              std::filesystem::path(sourcePath).stem().string() + ".xopp";
        gtk_file_dialog_set_initial_name(dialog, name.c_str());
        gtk_file_dialog_save(dialog, GTK_WINDOW(window), nullptr, +[](GObject* source, GAsyncResult* result,
                                                                      gpointer data) {
            std::unique_ptr<Choice> choice(static_cast<Choice*>(data));
            auto* self = choice->self;
            self->choosingFile = false;
            GError* error = nullptr;
            auto* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
            self->finishFileChoice(file, error, [self, after = std::move(choice->after)](const std::string& path) {
                self->saveTo(path, {}, after);
            });
        }, new Choice{this, std::move(after)});
        g_object_unref(dialog);
    }

    void finishFileChoice(GFile* file, GError* error, const std::function<void(const std::string&)>& chosen) {
        if (file) {
            auto* path = g_file_get_path(file);
            g_object_unref(file);
            if (path) {
                const std::string localPath = path;
                g_free(path);
                chosen(localPath);
            } else {
                showError("Choose a local file for this document.");
            }
        } else if (error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
                   !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
                   !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            showError(error->message);
        }
        g_clear_error(&error);
    }

    void protectDocument(std::function<void()> after) {
        if (busy || choosingFile || messageDialog) {
            return;
        }
        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this, after = std::move(after)]() mutable { protectDocument(std::move(after)); });
            return;
        }
        resetInput(true);
        if (!modified()) {
            after();
            return;
        }
        afterConfirmation = std::move(after);
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Save changes?",
                                       "This document has unsaved changes. Save them before continuing?"));
        adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "discard", "Discard", "save",
                                         saveTarget.empty() ? "Save As…" : "Save", nullptr);
        adw_alert_dialog_set_response_enabled(messageDialog, "save", bool(nativeSession));
        adw_alert_dialog_set_response_appearance(messageDialog, "discard", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_appearance(messageDialog, "save", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(messageDialog, "cancel");
        adw_alert_dialog_set_default_response(messageDialog, "save");
        g_signal_connect(messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response,
                                                                    gpointer data) {
            auto* self = static_cast<Prototype*>(data);
            self->messageDialog = nullptr;
            auto after = std::move(self->afterConfirmation);
            if (std::string(response) == "discard") {
                self->cleanupActiveAutosave();
                after();
            } else if (std::string(response) == "save") {
                self->save(std::move(after));
            }
        }), this);
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
    }

    void navigateToPage(int number) {
        if (number < 0 || number >= pageCount) {
            return;
        }
        if (number != state.page) {
            selectedImageIndex.reset();
        }
        resetInput();
        if (canvas) {
            state.scrollToPage(number, gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
            fitMode = false;
        } else {
            state.selectActivePage(number);
        }
        fitted = false;
        if (canvas) {
            update();
        }
    }

    void openGotoPageDialog() {
        if (busy || choosingFile || messageDialog || pageCount < 1) {
            return;
        }
        resetInput(true);
        const auto body = "Enter a page number between 1 and " + std::to_string(pageCount) + ".";
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Go to page", body.c_str()));
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), gtk_label_new("Page:"));
        gotoPageSpin = gtk_spin_button_new_with_range(1, std::max(1, pageCount), 1);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(gotoPageSpin), true);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gotoPageSpin), state.page + 1);
        gtk_editable_set_width_chars(GTK_EDITABLE(gotoPageSpin), 5);
        gtk_box_append(GTK_BOX(row), gotoPageSpin);
        adw_alert_dialog_set_extra_child(messageDialog, row);
        adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "go", "Go to Page", nullptr);
        adw_alert_dialog_set_response_appearance(messageDialog, "go", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(messageDialog, "cancel");
        adw_alert_dialog_set_default_response(messageDialog, "go");
        g_signal_connect(gotoPageSpin, "activate", G_CALLBACK(+[](GtkSpinButton*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->messageDialog) {
                                 self->answerDialog("go");
                             }
                         }),
                         this);
        g_signal_connect(messageDialog, "response",
                         G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const int target = self->gotoPageSpin ?
                                                        gtk_spin_button_get_value_as_int(
                                                                GTK_SPIN_BUTTON(self->gotoPageSpin)) -
                                                                1 :
                                                        -1;
                             self->messageDialog = nullptr;
                             self->gotoPageSpin = nullptr;
                             if (response && std::string_view(response) == "go" && target >= 0 &&
                                 target < self->pageCount) {
                                 self->navigateToPage(target);
                             }
                             if (self->canvas) {
                                 gtk_widget_grab_focus(self->canvas);
                             }
                         }),
                         this);
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
    }

    void openLayerRenameDialog(int pageIndex, size_t layerId) {
        if (busy || choosingFile || messageDialog || !nativeSession || pageIndex < 0 ||
            static_cast<size_t>(pageIndex) >= state.pageLayers.size()) {
            return;
        }
        const auto& pageLayers = state.layersForPage(pageIndex);
        if (layerId > pageLayers.layers.size()) {
            return;
        }
        finishTextEditing();
        resetInput(true);

        std::string currentName;
        const bool background = layerId == 0;
        if (background) {
            currentName = pageLayers.backgroundName.value_or("Background");
        } else {
            currentName = pageLayers.layers[layerId - 1].name;
            if (currentName.empty()) {
                currentName = "Layer " + std::to_string(layerId);
            }
        }

        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
                background ? "Rename Background" : "Rename Layer", "Change the name shown in the layer list."));
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_halign(row, GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(row), gtk_label_new("Name:"));
        layerRenameEntry = gtk_entry_new();
        gtk_entry_set_activates_default(GTK_ENTRY(layerRenameEntry), true);
        gtk_entry_set_max_length(GTK_ENTRY(layerRenameEntry), 4096);
        gtk_widget_set_hexpand(layerRenameEntry, true);
        gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), currentName.c_str());
        gtk_box_append(GTK_BOX(row), layerRenameEntry);
        adw_alert_dialog_set_extra_child(messageDialog, row);
        adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "rename", "Rename", nullptr);
        adw_alert_dialog_set_response_appearance(messageDialog, "rename", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(messageDialog, "cancel");
        adw_alert_dialog_set_default_response(messageDialog, "rename");
        layerRenamePage = pageIndex;
        layerRenameId = layerId;
        g_signal_connect(layerRenameEntry, "changed", G_CALLBACK(+[](GtkEditable* entry, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto* value = gtk_editable_get_text(entry);
                             adw_alert_dialog_set_response_enabled(
                                     self->messageDialog, "rename", std::char_traits<char>::length(value) <= 4096);
                         }), this);
        g_signal_connect(layerRenameEntry, "activate", G_CALLBACK(+[](GtkEntry*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->messageDialog) {
                                 self->answerDialog("rename");
                             }
                         }), this);
        g_signal_connect(messageDialog, "response",
                         G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const int targetPage = self->layerRenamePage;
                             const size_t targetLayer = self->layerRenameId;
                             const std::string newName = self->layerRenameEntry ?
                                     gtk_editable_get_text(GTK_EDITABLE(self->layerRenameEntry)) : "";
                             self->messageDialog = nullptr;
                             self->layerRenameEntry = nullptr;
                             if (response && std::string_view(response) == "rename" &&
                                 self->state.renameLayer(targetPage, targetLayer, newName)) {
                                 self->thumbnails.clear();
                                 self->rebuildLayerStack();
                                 self->update();
                             }
                             if (self->canvas) {
                                 gtk_widget_grab_focus(self->canvas);
                             }
                         }), this);
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
        gtk_widget_grab_focus(layerRenameEntry);
        gtk_editable_set_position(GTK_EDITABLE(layerRenameEntry), -1);
    }

    void exportPdfTo(const std::string& path) {
        if (!nativeSession || busy) {
            return;
        }
        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this, path] { exportPdfTo(path); });
            return;
        }
        resetInput(true);
        const auto packets = prototype::bridge::transferInk(state.ink);
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        const auto session = nativeSession;
        const auto worker = options.worker;
        // Exporting to PDF never alters .xopp savedRevision, saveTarget, or active autosave artifacts.
        runDocumentJob("Exporting PDF…",
                       [session, worker, path, packets, texts, images, layers, pageSizes] {
                           session->exportPdf(worker, path, packets, texts, images, layers, pageSizes);
                       },
                       [this, path] { exportedPdfPath = path; });
    }

    void startPluginExport(prototype::DocumentExportOptions exportOptions) {
        if (!nativeSession || busy) {
            return;
        }
        const auto path = exportOptions.outputFile;
        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this, exportOptions = std::move(exportOptions)]() mutable {
                startPluginExport(std::move(exportOptions));
            });
            return;
        }
        resetInput(true);
        const auto packets = prototype::bridge::transferInk(state.ink);
        const auto texts = state.texts;
        const auto images = prototype::bridge::transferImages(state.images);
        const auto layers = state.pageLayers;
        const auto pageSizes = currentPageSizes(state);
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto extension = std::filesystem::path(path).extension().string();
        const auto status = extension == ".pdf" ? "Exporting PDF…" :
                            extension == ".svg" ? "Exporting SVG…" : "Exporting PNG…";
        runDocumentJob(status,
                       [session, worker, exportOptions = std::move(exportOptions), packets, texts, images, layers,
                        pageSizes] {
                           session->exportDocument(worker, exportOptions, packets, texts, images, layers, pageSizes);
                       },
                       [this, path, extension] {
                           if (extension == ".pdf") {
                               exportedPdfPath = path;
                           }
                       });
    }

    bool exportDocumentFromPlugin(const prototype::DocumentExportOptions& exportOptions, std::string& error) {
        if (exportOptions.outputFile.empty()) {
            error = "Missing output file!";
            return false;
        }
        if (!nativeSession || options.worker.empty()) {
            error = "Document export requires the GTK 4 document worker";
            return false;
        }
        if (closed || choosingFile || messageDialog || busy) {
            error = "A document operation is already in progress";
            return false;
        }

        const auto extension = std::filesystem::path(exportOptions.outputFile).extension().string();
        if (extension != ".pdf" && extension != ".svg" && extension != ".png") {
            return true;
        }

        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this, exportOptions] { startPluginExport(exportOptions); });
        } else {
            startPluginExport(exportOptions);
        }
        return true;
    }

    void chooseExportPdf() {
        if (busy || choosingFile || messageDialog || !nativeSession) {
            return;
        }
        if (autosaving) {
            waitForAutosave([this] { chooseExportPdf(); });
            return;
        }
        resetInput(true);
        if (!smokeExportChoice.empty()) {
            const auto path = std::exchange(smokeExportChoice, {});
            exportPdfTo(path);
            return;
        }
        choosingFile = true;
        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Export as PDF");
        auto* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "PDF documents");
        gtk_file_filter_add_pattern(filter, "*.pdf");
        gtk_file_dialog_set_default_filter(dialog, filter);
        g_object_unref(filter);
        const auto name = sourcePath.empty() ? std::string("Notebook.pdf") :
                                               std::filesystem::path(sourcePath).stem().string() + ".pdf";
        gtk_file_dialog_set_initial_name(dialog, name.c_str());
        gtk_file_dialog_save(
                dialog, GTK_WINDOW(window), nullptr,
                +[](GObject* source, GAsyncResult* result, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    self->choosingFile = false;
                    GError* error = nullptr;
                    auto* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
                    self->finishFileChoice(file, error, [self](const std::string& path) { self->exportPdfTo(path); });
                },
                this);
        g_object_unref(dialog);
    }

    void insertPageAfterCurrent() {
        if (!nativeSession || busy || choosingFile || messageDialog) {
            return;
        }
        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this] { insertPageAfterCurrent(); });
            return;
        }
        resetInput(true);
        const int afterPage = state.page;
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto pageSizes = currentPageSizes(state);
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Inserting page…",
                [session, worker, afterPage, pageSizes, staged] {
                    *staged = session->stageInsertPageAfter(worker, afterPage, pageSizes);
                },
                [this, session, afterPage, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<PopplerDocument*> candidateLayers;
                    std::vector<PopplerDocument*> candidateAudioLayers;
                    std::vector<prototype::PageSize> sizes;
                    prototype::bridge::LayerDocument layerState;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                        candidateLayers = openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                        candidateAudioLayers = openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(),
                                                                          static_cast<int>(sizes.size()));
                        layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                        if (layerState.size() != sizes.size()) {
                            throw std::runtime_error("Inserted page layer metadata does not match the document");
                        }
                    } catch (...) {
                        if (candidate) {
                            g_object_unref(candidate);
                        }
                        releaseLayerPreviewPdfs(candidateLayers);
                        releaseLayerPreviewPdfs(candidateAudioLayers);
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }
                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    releaseLayerPreviewPdfs(layerDocuments);
                    layerDocuments = std::move(candidateLayers);
                    releaseLayerPreviewPdfs(audioLayerDocuments);
                    audioLayerDocuments = std::move(candidateAudioLayers);
                    audioTargets = session->audioTargets;
                    pageCount = static_cast<int>(sizes.size());
                    state.insertPageAfter(afterPage, sizes.at(static_cast<size_t>(afterPage + 1)));
                    state.setPages(std::move(sizes));
                    state.pageLayers.at(static_cast<size_t>(afterPage + 1)) =
                            layerState.at(static_cast<size_t>(afterPage + 1));
                    state.selectActivePage(afterPage + 1);
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    pageCache.clear();
                    layerPageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    navigateToPage(afterPage + 1);
                });
    }

    bool addEmptyPagesFromPlugin(bool force, std::string& error) {
        if (!nativeSession || options.worker.empty()) {
            error = "Adding note pages requires a native document and document worker";
            return false;
        }
        if (busy || choosingFile || messageDialog || closed) {
            error = "A document operation is already in progress";
            return false;
        }
        if (autosaving) {
            waitForAutosave([this, force] {
                std::string deferredError;
                if (!addEmptyPagesFromPlugin(force, deferredError)) {
                    showError(deferredError);
                }
            });
            return true;
        }
        if (nativeSession->pdfPageMap.size() != static_cast<size_t>(pageCount) ||
            state.pages.size() != static_cast<size_t>(pageCount)) {
            error = "The loaded document page metadata is inconsistent";
            return false;
        }

        std::vector<int> afterPages;
        for (int page = pageCount - 1; page >= 0; --page) {
            const auto pageIndex = static_cast<size_t>(page);
            const bool currentHasPdfBackground = nativeSession->pdfPageMap[pageIndex] >= 0;
            const bool nextHasPdfBackground =
                    page + 1 < pageCount && nativeSession->pdfPageMap[static_cast<size_t>(page + 1)] >= 0;
            if (force || (currentHasPdfBackground && (page + 1 == pageCount || nextHasPdfBackground))) {
                afterPages.push_back(page);
            }
        }

        if (afterPages.empty()) {
            navigateToPage(0);
            return true;
        }

        finishTextEditing();
        resetInput(true);
        const int finalPage = afterPages.back() == 0 ? 1 : 0;
        const auto sourcePageSizes = state.pages;
        const auto pageSizes = currentPageSizes(state);
        const auto session = nativeSession;
        const auto worker = options.worker;
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Adding note pages…",
                [session, worker, afterPages, pageSizes, staged] {
                    *staged = session->stageInsertPagesAfter(worker, afterPages, pageSizes);
                },
                [this, session, afterPages, sourcePageSizes, finalPage, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<PopplerDocument*> candidateLayers;
                    std::vector<PopplerDocument*> candidateAudioLayers;
                    std::vector<prototype::PageSize> sizes;
                    prototype::bridge::LayerDocument layerState;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                        candidateLayers =
                                openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                        candidateAudioLayers = openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(),
                                                                         static_cast<int>(sizes.size()));
                        layerState = prototype::bridge::readLayerTransfer(staged->layerStatePath());
                        if (layerState.size() != sizes.size()) {
                            throw std::runtime_error("Inserted page layer metadata does not match the document");
                        }
                    } catch (...) {
                        if (candidate) {
                            g_object_unref(candidate);
                        }
                        releaseLayerPreviewPdfs(candidateLayers);
                        releaseLayerPreviewPdfs(candidateAudioLayers);
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }
                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    releaseLayerPreviewPdfs(layerDocuments);
                    layerDocuments = std::move(candidateLayers);
                    releaseLayerPreviewPdfs(audioLayerDocuments);
                    audioLayerDocuments = std::move(candidateAudioLayers);
                    audioTargets = session->audioTargets;
                    pageCount = static_cast<int>(sizes.size());
                    for (const int afterPage: afterPages) {
                        state.insertPageAfter(afterPage, sourcePageSizes.at(static_cast<size_t>(afterPage)));
                    }
                    state.setPages(std::move(sizes));
                    for (const int afterPage: afterPages) {
                        const auto insertedPage = static_cast<size_t>(
                                afterPage + 1 +
                                std::count_if(afterPages.begin(), afterPages.end(),
                                              [afterPage](int pageBefore) { return pageBefore < afterPage; }));
                        state.pageLayers.at(insertedPage) = layerState.at(insertedPage);
                    }
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    pageCache.clear();
                    layerPageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    navigateToPage(finalPage);
                });
        return true;
    }

    void deleteCurrentPage() {
        if (!nativeSession || pageCount <= 1 || busy || choosingFile || messageDialog) {
            return;
        }
        finishTextEditing();
        if (autosaving) {
            waitForAutosave([this] { deleteCurrentPage(); });
            return;
        }
        resetInput(true);
        const int removeIndex = state.page;
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto pageSizes = currentPageSizes(state);
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Deleting page…",
                [session, worker, removeIndex, pageSizes, staged] {
                    *staged = session->stageDeletePageAt(worker, removeIndex, pageSizes);
                },
                [this, session, removeIndex, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<PopplerDocument*> candidateLayers;
                    std::vector<PopplerDocument*> candidateAudioLayers;
                    std::vector<prototype::PageSize> sizes;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                        candidateLayers = openLayerPreviewPdfs(staged->layerPreviewPaths(), static_cast<int>(sizes.size()));
                        candidateAudioLayers = openAudioLayerPreviewPdfs(staged->audioLayerPreviewPaths(),
                                                                          static_cast<int>(sizes.size()));
                    } catch (...) {
                        if (candidate) {
                            g_object_unref(candidate);
                        }
                        releaseLayerPreviewPdfs(candidateLayers);
                        releaseLayerPreviewPdfs(candidateAudioLayers);
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }
                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    releaseLayerPreviewPdfs(layerDocuments);
                    layerDocuments = std::move(candidateLayers);
                    releaseLayerPreviewPdfs(audioLayerDocuments);
                    audioLayerDocuments = std::move(candidateAudioLayers);
                    audioTargets = session->audioTargets;
                    pageCount = static_cast<int>(sizes.size());
                    state.deletePageAt(removeIndex);
                    state.setPages(std::move(sizes));
                    const int nextActive = std::min(removeIndex, pageCount - 1);
                    state.selectActivePage(nextActive);
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    pageCache.clear();
                    layerPageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    navigateToPage(nextActive);
                });
    }

    GtkWidget* button(const char* icon, const char* tooltip, GCallback callback) {
        auto* result = gtk_button_new_from_icon_name(icon);
        gtk_widget_set_tooltip_text(result, tooltip);
        g_signal_connect(result, "clicked", callback, this);
        return result;
    }

    void syncSizeDropdown() {
        if (!sizeDropdown) {
            return;
        }
        const auto matched = prototype::behavior::matchToolSize(prototype::widthsForTool(state.brush.tool),
                                                                state.brush.width);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), matched ? static_cast<guint>(*matched) : 5u);
    }

    PopplerPage* pdfPageForPage(int page) {
        PopplerDocument* source = document;
        int sourcePage = page;
        if (nativeSession) {
            if (page < 0 || static_cast<size_t>(page) >= nativeSession->pdfPageMap.size()) {
                return nullptr;
            }
            sourcePage = nativeSession->pdfPageMap[static_cast<size_t>(page)];
            if (sourcePage < 0) {
                return nullptr;
            }
            const auto textSourcePath = nativeSession->backgroundPdfPath().string();
            if (textSourcePath != pdfTextDocumentPath) {
                g_clear_object(&pdfTextDocument);
                if (std::filesystem::is_regular_file(nativeSession->backgroundPdfPath())) {
                    std::vector<prototype::PageSize> ignoredSizes;
                    std::tie(pdfTextDocument, ignoredSizes) = openPreviewPdf(textSourcePath);
                    pdfTextDocumentPath = textSourcePath;
                }
            }
            source = pdfTextDocument;
        }
        if (!source || sourcePage < 0 || sourcePage >= poppler_document_get_n_pages(source)) {
            return nullptr;
        }
        return poppler_document_get_page(source, sourcePage);
    }

    bool smartSelectHasSelectedObjectAt(int page, prototype::Point point) const {
        if (!state.hasSelection()) {
            return false;
        }
        const auto bounds = state.selectionBox();
        if (!bounds || bounds->page != page) {
            return false;
        }
        const double padding = 5 / std::max(0.1, state.zoom);
        return point.x >= bounds->x0 - padding && point.x <= bounds->x1 + padding &&
               point.y >= bounds->y0 - padding && point.y <= bounds->y1 + padding;
    }

    bool smartSelectHasEditableObjectAt(int page, prototype::Point point) const {
        return state.pickSmartSelectObject(page, point).has_value();
    }

    bool pdfTextAtCanvasPoint(int pageIndex, prototype::Point point) {
        auto* page = pdfPageForPage(pageIndex);
        if (!page) {
            return false;
        }
        const bool found = prototype::pdfTextAt(page, point.x, point.y);
        g_object_unref(page);
        return found;
    }

    bool beginSmartSelectPdfGesture(Device device, double x, double y) {
        if (state.brush.tool != Tool::SmartSelect || input.shiftHeld || input.spaceHeld ||
            (device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
            return false;
        }
        const auto hit = state.hitPage(x, y);
        if (!hit || smartSelectHasSelectedObjectAt(hit->page, hit->local)) {
            return false;
        }

        const bool altPressed = input.altHeld;
        if (!altPressed && smartSelectHasEditableObjectAt(hit->page, hit->local)) {
            return false;
        }
        if (!altPressed && !pdfTextAtCanvasPoint(hit->page, hit->local)) {
            return false;
        }

        smartSelectPdfGesture = SmartSelectPdfGesture{device, {x, y}, {x, y}, altPressed};
        return true;
    }

    void updatePdfActionsPosition(bool popup) {
        if (!pdfActionsPopover || !pdfTextSelection || !pdfTextSelection->finalized ||
            !pdfTextSelection->region || cairo_region_num_rectangles(pdfTextSelection->region.get()) == 0) {
            return;
        }
        cairo_rectangle_int_t extents{};
        cairo_region_get_extents(pdfTextSelection->region.get(), &extents);
        const auto first = state.pageScreenPoint(pdfTextSelection->page, extents.x, extents.y);
        const auto second = state.pageScreenPoint(pdfTextSelection->page, extents.x + extents.width,
                                                  extents.y + extents.height);
        GdkRectangle pointing{static_cast<int>(std::floor(std::min(first.x, second.x))),
                              static_cast<int>(std::floor(std::min(first.y, second.y))),
                              std::max(1, static_cast<int>(std::ceil(std::abs(second.x - first.x)))),
                              std::max(1, static_cast<int>(std::ceil(std::abs(second.y - first.y))))};
        gtk_popover_set_pointing_to(pdfActionsPopover, &pointing);
        if (pdfSelectionStyleButton) {
            const char* label = pdfTextSelection->style == prototype::PdfTextSelectionStyle::Linear ?
                                        "Rectangle" : "Linear";
            gtk_button_set_label(GTK_BUTTON(pdfSelectionStyleButton), label);
        }
        if (popup) {
            gtk_popover_popup(pdfActionsPopover);
        }
    }

    void clearPdfTextSelection(bool update = true) {
        pdfTextSelection.reset();
        if (pdfActionsPopover) {
            gtk_popover_popdown(pdfActionsPopover);
        }
        if (update && canvas) {
            this->update();
        }
    }

    void beginPdfTextSelection(Device device, double x, double y) {
        clearPdfTextSelection(false);
        state.clearSelection();
        selectedImageIndex.reset();
        const auto hit = state.hitPage(x, y);
        if (!hit) {
            update();
            return;
        }
        auto* page = pdfPageForPage(hit->page);
        if (!page) {
            update();
            return;
        }
        PdfTextSelectionState selection;
        selection.owner = device;
        selection.page = hit->page;
        selection.tool = state.brush.tool;
        selection.style = pdfSelectionStyleForTool(selection.tool);
        selection.bounds = {hit->local.x, hit->local.y, hit->local.x, hit->local.y};
        selection.dragging = true;
        selection.region = prototype::pdfTextSelectionPreview(page, selection.bounds, selection.style);
        g_object_unref(page);
        pdfTextSelection = std::move(selection);
        update();
    }

    void updatePdfTextSelection(Device device, double x, double y) {
        if (!pdfTextSelection || !pdfTextSelection->dragging || pdfTextSelection->owner != device) {
            return;
        }
        const auto size = state.pageSize(pdfTextSelection->page);
        const auto local = state.pageLocalPoint(pdfTextSelection->page, x, y);
        pdfTextSelection->bounds.x2 = std::clamp(local.x, 0.0, size.width);
        pdfTextSelection->bounds.y2 = std::clamp(local.y, 0.0, size.height);
        auto* page = pdfPageForPage(pdfTextSelection->page);
        if (!page) {
            clearPdfTextSelection(false);
            update();
            return;
        }
        pdfTextSelection->region = prototype::pdfTextSelectionPreview(page, pdfTextSelection->bounds,
                                                                       pdfTextSelection->style);
        g_object_unref(page);
        update();
    }

    void finishPdfTextSelection(Device device) {
        if (!pdfTextSelection || !pdfTextSelection->dragging || pdfTextSelection->owner != device) {
            return;
        }
        pdfTextSelection->dragging = false;
        auto* page = pdfPageForPage(pdfTextSelection->page);
        if (!page) {
            clearPdfTextSelection(false);
            update();
            return;
        }
        auto result = prototype::selectPdfText(page, pdfTextSelection->bounds, pdfTextSelection->style);
        g_object_unref(page);
        if (result.text.empty() || result.textRects.empty()) {
            clearPdfTextSelection(false);
            update();
            return;
        }
        pdfTextSelection->text = std::move(result.text);
        pdfTextSelection->textRects = std::move(result.textRects);
        pdfTextSelection->region = std::move(result.region);
        pdfTextSelection->finalized = true;
        if (auto* primary = gdk_display_get_primary_clipboard(gtk_widget_get_display(canvas))) {
            gdk_clipboard_set_text(primary, pdfTextSelection->text.c_str());
        }
        updatePdfActionsPosition(true);
        update();
    }

    void switchPdfTextSelectionStyle() {
        const bool smartSelectPdfResult = state.brush.tool == Tool::SmartSelect && pdfTextSelection &&
                                          pdfTextSelection->finalized;
        if (!prototype::isPdfTextSelectionTool(state.brush.tool) && !smartSelectPdfResult) {
            return;
        }
        const auto currentStyle = pdfTextSelection ? pdfTextSelection->style :
                                                    pdfSelectionStyleForTool(state.brush.tool);
        const Tool next = currentStyle == prototype::PdfTextSelectionStyle::Linear ? Tool::SelectPdfTextRect :
                                                                                     Tool::SelectPdfTextLinear;
        if (!smartSelectPdfResult) {
            state.brush.tool = next;
            selectionTool = next;
            if (selectionModeDropdown) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), selectionModeIndex(next));
            }
        }
        if (pdfTextSelection && pdfTextSelection->finalized) {
            if (!smartSelectPdfResult) {
                pdfTextSelection->tool = next;
            }
            const auto nextStyle = pdfSelectionStyleForTool(next);
            if (nextStyle == prototype::PdfTextSelectionStyle::Rectangle &&
                currentStyle != prototype::PdfTextSelectionStyle::Rectangle &&
                !pdfTextSelection->textRects.empty()) {
                auto bounds = pdfTextSelection->textRects.front();
                for (const auto& rect: pdfTextSelection->textRects) {
                    bounds.x1 = std::min(bounds.x1, std::min(rect.x1, rect.x2));
                    bounds.y1 = std::min(bounds.y1, std::min(rect.y1, rect.y2));
                    bounds.x2 = std::max(bounds.x2, std::max(rect.x1, rect.x2));
                    bounds.y2 = std::max(bounds.y2, std::max(rect.y1, rect.y2));
                }
                pdfTextSelection->bounds = bounds;
            }
            pdfTextSelection->style = nextStyle;
            auto* page = pdfPageForPage(pdfTextSelection->page);
            if (page) {
                auto result = prototype::selectPdfText(page, pdfTextSelection->bounds,
                                                       pdfTextSelection->style);
                g_object_unref(page);
                pdfTextSelection->text = std::move(result.text);
                pdfTextSelection->textRects = std::move(result.textRects);
                pdfTextSelection->region = std::move(result.region);
                if (pdfTextSelection->text.empty() || pdfTextSelection->textRects.empty()) {
                    clearPdfTextSelection(false);
                }
            }
        }
        updatePdfActionsPosition(false);
        update();
    }

    void selectPdfTextAtPoint(double x, double y, prototype::PdfTextSelectionStyle style) {
        const auto hit = state.hitPage(x, y);
        if (!hit) {
            clearPdfTextSelection(false);
            update();
            return;
        }
        auto* page = pdfPageForPage(hit->page);
        if (!page) {
            clearPdfTextSelection(false);
            update();
            return;
        }

        constexpr double clickRadius = 1.0;
        PdfTextSelectionState selection;
        selection.page = hit->page;
        selection.tool = state.brush.tool;
        selection.style = style;
        selection.bounds = {hit->local.x - clickRadius, hit->local.y - clickRadius,
                            hit->local.x + clickRadius, hit->local.y + clickRadius};
        auto result = prototype::selectPdfText(page, selection.bounds, style);
        g_object_unref(page);
        if (result.text.empty() || result.textRects.empty()) {
            clearPdfTextSelection(false);
            update();
            return;
        }
        selection.text = std::move(result.text);
        selection.textRects = std::move(result.textRects);
        selection.region = std::move(result.region);
        selection.finalized = true;
        pdfTextSelection = std::move(selection);
        if (auto* primary = gdk_display_get_primary_clipboard(gtk_widget_get_display(canvas))) {
            gdk_clipboard_set_text(primary, pdfTextSelection->text.c_str());
        }
        updatePdfActionsPosition(true);
        update();
    }

    void copyPdfTextSelection() {
        if (!pdfTextSelection || !pdfTextSelection->finalized || pdfTextSelection->text.empty()) {
            return;
        }
        auto* display = gtk_widget_get_display(canvas);
        if (auto* clipboard = gdk_display_get_clipboard(display)) {
            gdk_clipboard_set_text(clipboard, pdfTextSelection->text.c_str());
        }
        if (auto* primary = gdk_display_get_primary_clipboard(display)) {
            gdk_clipboard_set_text(primary, pdfTextSelection->text.c_str());
        }
        clearPdfTextSelection(false);
        update();
    }

    static bool samePdfHighlight(const Ink& stroke, int page, size_t layerId, size_t originLayer,
                                 const PopplerRectangle& rect, int fill) {
        constexpr double tolerance = 1.0;
        if (stroke.page != page || stroke.layerId != layerId || stroke.originLayer != originLayer ||
            stroke.brush.tool != Tool::Highlighter || stroke.pressureSensitive || stroke.fill != fill ||
            stroke.capStyle != 1 || stroke.points.size() != 2) {
            return false;
        }
        const double left = std::min(rect.x1, rect.x2);
        const double right = std::max(rect.x1, rect.x2);
        const double top = std::min(rect.y1, rect.y2);
        const double bottom = std::max(rect.y1, rect.y2);
        const double y = (top + bottom) / 2;
        const double width = bottom - top;
        const double strokeLeft = std::min(stroke.points[0].x, stroke.points[1].x);
        const double strokeRight = std::max(stroke.points[0].x, stroke.points[1].x);
        return std::abs(stroke.points[0].y - stroke.points[1].y) <= tolerance &&
               std::abs(strokeLeft - left) <= tolerance && std::abs(strokeRight - right) <= tolerance &&
               std::abs(stroke.points[0].y - y) <= tolerance && std::abs(stroke.brush.width - width) <= tolerance;
    }

    void createPdfTextAnnotation(PdfTextAnnotation annotation) {
        if (!pdfTextSelection || !pdfTextSelection->finalized || pdfTextSelection->textRects.empty()) {
            return;
        }
        const bool isHighlight = annotation == PdfTextAnnotation::Highlight;
        const auto color = isHighlight ? highlightBrush.color : pdfTextColor;
        const int fill = isHighlight ? pdfTextMarkerOpacity : 230;
        const size_t layerId = state.activeLayerId(pdfTextSelection->page);
        const size_t originLayer = state.activeLayerOrigin(pdfTextSelection->page);
        std::vector<std::pair<size_t, Ink>> replacements;
        std::vector<Ink> additions;
        std::vector<size_t> used;

        for (const auto& rect: pdfTextSelection->textRects) {
            const double left = std::min(rect.x1, rect.x2);
            const double right = std::max(rect.x1, rect.x2);
            const double top = std::min(rect.y1, rect.y2);
            const double bottom = std::max(rect.y1, rect.y2);
            const double height = bottom - top;
            if (!(right > left) || !(height > 0)) {
                continue;
            }
            const double y = annotation == PdfTextAnnotation::Underline ? bottom : (top + bottom) / 2;
            Ink stroke{pdfTextSelection->page, {{left, y, 1}, {right, y, 1}},
                       {Tool::Highlighter, color, isHighlight ? height : 1.0}};
            stroke.fill = fill;
            stroke.capStyle = 1;

            bool matched = false;
            if (isHighlight) {
                for (size_t i = 0; i < state.ink.size(); ++i) {
                    if (std::find(used.begin(), used.end(), i) != used.end() ||
                        !samePdfHighlight(state.ink[i], pdfTextSelection->page, layerId, originLayer, rect, fill)) {
                        continue;
                    }
                    used.push_back(i);
                    matched = true;
                    if (state.ink[i].brush.color != color) {
                        auto replacement = state.ink[i];
                        replacement.brush.color = color;
                        replacements.emplace_back(i, std::move(replacement));
                    }
                    break;
                }
            }
            if (!matched) {
                additions.push_back(std::move(stroke));
            }
        }
        if (!replacements.empty() || !additions.empty()) {
            state.applyInkEdits(std::move(replacements), std::move(additions));
        }
        clearPdfTextSelection(false);
        update();
    }

    void setPdfHighlightColor(const std::array<double, 3>& color) {
        highlightBrush.color = color;
        if (prototype::isPdfTextSelectionTool(state.brush.tool)) {
            state.brush.color = color;
        }
        if (pdfHighlightColorButton) {
            const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]),
                               static_cast<float>(color[2]), 1};
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pdfHighlightColorButton), &rgba);
        }
        update();
    }

    void createPdfActionsPopover() {
        pdfActionsPopover = GTK_POPOVER(gtk_popover_new());
        gtk_popover_set_position(pdfActionsPopover, GTK_POS_TOP);
        gtk_popover_set_autohide(pdfActionsPopover, false);
        auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_add_css_class(actions, "toolbar");
        gtk_widget_add_css_class(actions, "linked");
        gtk_widget_set_margin_top(actions, 4);
        gtk_widget_set_margin_bottom(actions, 4);
        gtk_widget_set_margin_start(actions, 4);
        gtk_widget_set_margin_end(actions, 4);

        pdfCopyButton = button(
                "edit-copy-symbolic", "Copy selected PDF text",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->copyPdfTextSelection(); }));
        gtk_box_append(GTK_BOX(actions), pdfCopyButton);
        pdfHighlightButton = button(
                "format-text-highlight-symbolic", "Highlight selected PDF text",
                G_CALLBACK((+[](GtkButton*, gpointer data) {
                    static_cast<Prototype*>(data)->createPdfTextAnnotation(PdfTextAnnotation::Highlight);
                })));
        gtk_box_append(GTK_BOX(actions), pdfHighlightButton);
        pdfUnderlineButton = button(
                "format-text-underline-symbolic", "Underline selected PDF text",
                G_CALLBACK((+[](GtkButton*, gpointer data) {
                    static_cast<Prototype*>(data)->createPdfTextAnnotation(PdfTextAnnotation::Underline);
                })));
        gtk_box_append(GTK_BOX(actions), pdfUnderlineButton);
        pdfStrikethroughButton = button(
                "format-text-strikethrough-symbolic", "Strikethrough selected PDF text",
                G_CALLBACK((+[](GtkButton*, gpointer data) {
                    static_cast<Prototype*>(data)->createPdfTextAnnotation(PdfTextAnnotation::Strikethrough);
                })));
        gtk_box_append(GTK_BOX(actions), pdfStrikethroughButton);

        pdfSelectionStyleButton = gtk_button_new_with_label("Rectangle");
        gtk_widget_set_tooltip_text(pdfSelectionStyleButton, "Switch PDF text selection style");
        g_signal_connect(pdfSelectionStyleButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->switchPdfTextSelectionStyle();
                         }),
                         this);
        gtk_box_append(GTK_BOX(actions), pdfSelectionStyleButton);
        gtk_box_append(GTK_BOX(actions), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

        auto* markerDialog = gtk_color_dialog_new();
        gtk_color_dialog_set_with_alpha(markerDialog, false);
        pdfHighlightColorButton = gtk_color_dialog_button_new(markerDialog);
        gtk_widget_set_tooltip_text(pdfHighlightColorButton, "PDF highlight color");
        gtk_accessible_update_property(GTK_ACCESSIBLE(pdfHighlightColorButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "PDF highlight color", -1);
        const auto& markerColor = highlightBrush.color;
        const GdkRGBA markerRgba{static_cast<float>(markerColor[0]), static_cast<float>(markerColor[1]),
                                 static_cast<float>(markerColor[2]), 1};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pdfHighlightColorButton), &markerRgba);
        g_signal_connect(pdfHighlightColorButton, "notify::rgba",
                         G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto* rgba = gtk_color_dialog_button_get_rgba(
                                     GTK_COLOR_DIALOG_BUTTON(self->pdfHighlightColorButton));
                             self->setPdfHighlightColor({rgba->red, rgba->green, rgba->blue});
                         })),
                         this);
        gtk_box_append(GTK_BOX(actions), pdfHighlightColorButton);

        pdfHighlightPaletteButton = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pdfHighlightPaletteButton), "view-grid-symbolic");
        gtk_widget_set_tooltip_text(pdfHighlightPaletteButton, "Highlight color presets");
        auto* presetPopover = gtk_popover_new();
        auto* presetGrid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(presetGrid), 2);
        gtk_grid_set_column_spacing(GTK_GRID(presetGrid), 2);
        gtk_widget_set_margin_top(presetGrid, 6);
        gtk_widget_set_margin_bottom(presetGrid, 6);
        gtk_widget_set_margin_start(presetGrid, 6);
        gtk_widget_set_margin_end(presetGrid, 6);
        for (size_t index = 0; index < pdfHighlightPresets.size(); ++index) {
            const auto& preset = pdfHighlightPresets[index];
            auto* swatch = gtk_button_new();
            pdfHighlightPresetButtons[index] = swatch;
            gtk_widget_add_css_class(swatch, "flat");
            gtk_widget_set_tooltip_text(swatch, preset.name);
            gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL, preset.name, -1);
            auto* dot = gtk_drawing_area_new();
            gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(dot), 24);
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(dot), 24);
            auto* color = g_new(GdkRGBA, 1);
            *color = {static_cast<float>(preset.color[0]), static_cast<float>(preset.color[1]),
                      static_cast<float>(preset.color[2]), 1};
            g_object_set_data_full(G_OBJECT(dot), "pdf-highlight-preset-color", color, g_free);
            gtk_drawing_area_set_draw_func(
                    GTK_DRAWING_AREA(dot),
                    +[](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer) {
                        auto* rgba = static_cast<GdkRGBA*>(g_object_get_data(
                                G_OBJECT(area), "pdf-highlight-preset-color"));
                        if (!rgba) {
                            return;
                        }
                        cairo_arc(cr, width / 2.0, height / 2.0, std::min(width, height) / 2.0 - 2, 0, 2 * G_PI);
                        gdk_cairo_set_source_rgba(cr, rgba);
                        cairo_fill_preserve(cr);
                        cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.75);
                        cairo_set_line_width(cr, 1);
                        cairo_stroke(cr);
                    },
                    nullptr, nullptr);
            gtk_button_set_child(GTK_BUTTON(swatch), dot);
            g_object_set_data(G_OBJECT(swatch), "pdf-highlight-preset-index",
                              GUINT_TO_POINTER(static_cast<guint>(index + 1)));
            g_signal_connect(swatch, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const auto index = static_cast<size_t>(GPOINTER_TO_UINT(
                                         g_object_get_data(G_OBJECT(button), "pdf-highlight-preset-index")) - 1);
                                 if (index < pdfHighlightPresets.size()) {
                                     self->setPdfHighlightColor(pdfHighlightPresets[index].color);
                                 }
                                 auto* popover = gtk_menu_button_get_popover(
                                         GTK_MENU_BUTTON(self->pdfHighlightPaletteButton));
                                 if (popover) {
                                     gtk_popover_popdown(GTK_POPOVER(popover));
                                 }
                             }), this);
            gtk_grid_attach(GTK_GRID(presetGrid), swatch, static_cast<int>(index % 3),
                            static_cast<int>(index / 3), 1, 1);
        }
        gtk_popover_set_child(GTK_POPOVER(presetPopover), presetGrid);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(pdfHighlightPaletteButton), presetPopover);
        gtk_box_append(GTK_BOX(actions), pdfHighlightPaletteButton);

        pdfMarkerOpacityButton = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pdfMarkerOpacityButton), "applications-graphics-symbolic");
        gtk_widget_set_tooltip_text(pdfMarkerOpacityButton, "PDF highlight marker opacity");
        auto* opacityPopover = gtk_popover_new();
        auto* opacityBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_top(opacityBox, 8);
        gtk_widget_set_margin_bottom(opacityBox, 8);
        gtk_widget_set_margin_start(opacityBox, 8);
        gtk_widget_set_margin_end(opacityBox, 8);
        auto* opacityLabel = gtk_label_new("Marker opacity");
        gtk_widget_set_halign(opacityLabel, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(opacityBox), opacityLabel);
        pdfMarkerOpacityScale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
        gtk_widget_set_size_request(pdfMarkerOpacityScale, 160, -1);
        gtk_scale_set_draw_value(GTK_SCALE(pdfMarkerOpacityScale), true);
        gtk_range_set_value(GTK_RANGE(pdfMarkerOpacityScale), pdfTextMarkerOpacity);
        gtk_accessible_update_property(GTK_ACCESSIBLE(pdfMarkerOpacityScale), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "PDF highlight marker opacity", -1);
        g_signal_connect(pdfMarkerOpacityScale, "value-changed",
                         G_CALLBACK(+[](GtkRange* range, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->pdfTextMarkerOpacity = static_cast<int>(std::lround(gtk_range_get_value(range)));
                         }), this);
        gtk_box_append(GTK_BOX(opacityBox), pdfMarkerOpacityScale);
        gtk_popover_set_child(GTK_POPOVER(opacityPopover), opacityBox);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(pdfMarkerOpacityButton), opacityPopover);
        gtk_box_append(GTK_BOX(actions), pdfMarkerOpacityButton);

        auto* textDialog = gtk_color_dialog_new();
        gtk_color_dialog_set_with_alpha(textDialog, false);
        pdfTextColorButton = gtk_color_dialog_button_new(textDialog);
        gtk_widget_set_tooltip_text(pdfTextColorButton, "PDF underline and strikethrough color");
        gtk_accessible_update_property(GTK_ACCESSIBLE(pdfTextColorButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "PDF annotation text color", -1);
        const GdkRGBA textRgba{static_cast<float>(pdfTextColor[0]), static_cast<float>(pdfTextColor[1]),
                               static_cast<float>(pdfTextColor[2]), 1};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(pdfTextColorButton), &textRgba);
        g_signal_connect(pdfTextColorButton, "notify::rgba",
                         G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto* rgba = gtk_color_dialog_button_get_rgba(
                                     GTK_COLOR_DIALOG_BUTTON(self->pdfTextColorButton));
                             self->pdfTextColor = {rgba->red, rgba->green, rgba->blue};
                         })),
                         this);
        gtk_box_append(GTK_BOX(actions), pdfTextColorButton);

        gtk_popover_set_child(pdfActionsPopover, actions);
        gtk_widget_set_parent(GTK_WIDGET(pdfActionsPopover), canvasOverlay);
    }

    void createLinkInfoPopover() {
        linkInfoPopover = gtk_popover_new();
        gtk_popover_set_autohide(GTK_POPOVER(linkInfoPopover), false);
        linkInfoLabel = gtk_label_new("");
        gtk_label_set_selectable(GTK_LABEL(linkInfoLabel), true);
        gtk_label_set_wrap(GTK_LABEL(linkInfoLabel), true);
        gtk_widget_set_margin_top(linkInfoLabel, 8);
        gtk_widget_set_margin_bottom(linkInfoLabel, 8);
        gtk_widget_set_margin_start(linkInfoLabel, 10);
        gtk_widget_set_margin_end(linkInfoLabel, 10);
        gtk_popover_set_child(GTK_POPOVER(linkInfoPopover), linkInfoLabel);
        gtk_overlay_add_overlay(GTK_OVERLAY(canvasOverlay), linkInfoPopover);
        gtk_widget_set_can_target(linkInfoPopover, false);
    }

    void addFloatingToolboxToolAction(GtkWidget* row, Tool tool, const char* label) {
        auto* toolButton = gtk_toggle_button_new_with_label(label);
        gtk_widget_add_css_class(toolButton, "flat");
        gtk_widget_set_size_request(toolButton, 82, 36);
        floatingToolboxToolButtons.emplace_back(tool, toolButton);
        g_object_set_data(G_OBJECT(toolButton), "floating-toolbox-tool", GINT_TO_POINTER(static_cast<int>(tool)));
        g_signal_connect(toolButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!gtk_toggle_button_get_active(button) || self->syncingTools) {
                                 return;
                             }
                             const auto selectedTool = static_cast<Tool>(GPOINTER_TO_INT(
                                     g_object_get_data(G_OBJECT(button), "floating-toolbox-tool")));
                             self->selectTool(selectedTool);
                             self->hideFloatingToolbox();
                         }),
                         this);
        gtk_box_append(GTK_BOX(row), toolButton);
    }

    void addFloatingToolboxAction(GtkWidget* row, const std::string& token) {
        auto addTool = [&](Tool tool, const char* label) { addFloatingToolboxToolAction(row, tool, label); };
        auto addButton = [&](const char* label, GCallback callback) {
            auto* action = gtk_button_new_with_label(label);
            gtk_widget_add_css_class(action, "flat");
            g_signal_connect(action, "clicked", callback, this);
            gtk_box_append(GTK_BOX(row), action);
        };

        if (token == "PEN") {
            addTool(Tool::Pen, "Pen");
        } else if (token == "HIGHLIGHTER") {
            addTool(Tool::Highlighter, "Highlight");
        } else if (token == "ERASER") {
            addTool(Tool::Eraser, "Eraser");
        } else if (token == "HAND") {
            addTool(Tool::Hand, "Hand");
        } else if (token == "SELECT") {
            addTool(Tool::SelectRectangle, "Select");
        } else if (token == "SELECT_RECT") {
            addTool(Tool::SelectRectangle, "Rectangle select");
        } else if (token == "SELECT_LASSO" || token == "SELECT_REGION") {
            addTool(Tool::SelectLasso, "Lasso");
        } else if (token == "SMART_SELECT") {
            addTool(Tool::SmartSelect, "Smart Select");
        } else if (token == "SELECT_OBJECT") {
            addTool(Tool::SelectObject, "Select object");
        } else if (token == "TEXT") {
            addTool(Tool::Text, "Text");
        } else if (token == "LATEX") {
            addTool(Tool::Latex, "LaTeX");
        } else if (token == "LINK") {
            addTool(Tool::Link, "Link");
        } else if (token == "PLAY_OBJECT") {
            addTool(Tool::PlayObject, "Play audio");
        } else if (token == "LASER_POINTER") {
            addTool(Tool::LaserPointerPen, "Laser pen");
            addTool(Tool::LaserPointerHighlighter, "Laser marker");
        } else if (token == "PDF_TOOL") {
            addTool(Tool::SelectPdfTextLinear, "PDF text");
            addTool(Tool::SelectPdfTextRect, "PDF area");
        } else if (token == "UNDO") {
            addButton("Undo", G_CALLBACK(+[](GtkButton*, gpointer data) {
                          auto* self = static_cast<Prototype*>(data);
                          g_signal_emit_by_name(self->undoButton, "clicked");
                      }));
        } else if (token == "REDO") {
            addButton("Redo", G_CALLBACK(+[](GtkButton*, gpointer data) {
                          auto* self = static_cast<Prototype*>(data);
                          g_signal_emit_by_name(self->redoButton, "clicked");
                      }));
        } else if (token == "ZOOM_SLIDER") {
            auto* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 25.0, 400.0, 5.0);
            gtk_widget_set_size_request(scale, 132, -1);
            gtk_scale_set_draw_value(GTK_SCALE(scale), true);
            gtk_scale_set_digits(GTK_SCALE(scale), 0);
            gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
            gtk_scale_set_format_value_func(
                    GTK_SCALE(scale),
                    +[](GtkScale*, double value, gpointer) { return g_strdup_printf("%.0f%%", value); }, nullptr,
                    nullptr);
            gtk_widget_set_tooltip_text(scale, "Zoom percentage");
            gtk_range_set_value(GTK_RANGE(scale), state.zoom * 100.0);
            g_signal_connect(scale, "value-changed", G_CALLBACK(+[](GtkRange* range, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const double x = gtk_widget_get_width(self->canvas) / 2.0;
                                 const double y = gtk_widget_get_height(self->canvas) / 2.0;
                                 self->zoom(gtk_range_get_value(range) / 100.0, x, y);
                             }),
                             this);
            gtk_box_append(GTK_BOX(row), scale);
        } else if (token == "DRAW_ELLIPSE" || token == "DRAW_RECTANGLE" || token == "DRAW_ARROW" ||
                   token == "DRAW_LINE") {
            const auto mode = token == "DRAW_ELLIPSE" ? prototype::DrawingMode::Ellipse :
                              token == "DRAW_RECTANGLE" ? prototype::DrawingMode::Rectangle :
                              token == "DRAW_ARROW" ? prototype::DrawingMode::Arrow : prototype::DrawingMode::Line;
            const char* label = token == "DRAW_ELLIPSE" ? "Ellipse" :
                                token == "DRAW_RECTANGLE" ? "Rectangle" :
                                token == "DRAW_ARROW" ? "Arrow" : "Line";
            addButton(label, G_CALLBACK(+[](GtkButton* button, gpointer data) {
                          auto* self = static_cast<Prototype*>(data);
                          const auto selectedMode = static_cast<prototype::DrawingMode>(GPOINTER_TO_INT(
                                  g_object_get_data(G_OBJECT(button), "floating-drawing-mode")));
                          if (self->drawingModeDropdown) {
                              gtk_drop_down_set_selected(GTK_DROP_DOWN(self->drawingModeDropdown),
                                                         static_cast<guint>(selectedMode));
                          } else {
                              self->state.drawingMode = selectedMode;
                              self->update();
                          }
                      }));
            g_object_set_data(G_OBJECT(gtk_widget_get_last_child(row)), "floating-drawing-mode",
                              GINT_TO_POINTER(static_cast<int>(mode)));
        } else if (token == "TOOL_FILL") {
            auto* unsupported = gtk_button_new_with_label("Fill");
            gtk_widget_set_sensitive(unsupported, false);
            gtk_widget_set_tooltip_text(unsupported, "Filled native drawing shapes are not ported yet");
            gtk_box_append(GTK_BOX(row), unsupported);
        } else if (token == "FINE" || token == "MEDIUM" || token == "THICK" || token == "VERY_FINE" ||
                   token == "VERY_THICK") {
            const guint preset = token == "VERY_FINE" ? 0u : token == "FINE" ? 1u : token == "MEDIUM" ? 2u :
                                 token == "THICK" ? 3u : 4u;
            const char* label = token == "VERY_FINE" ? "Very fine" : token == "FINE" ? "Fine" :
                                token == "MEDIUM" ? "Medium" : token == "THICK" ? "Thick" : "Very thick";
            addButton(label, G_CALLBACK(+[](GtkButton* button, gpointer data) {
                          auto* self = static_cast<Prototype*>(data);
                          const auto selectedPreset = GPOINTER_TO_UINT(
                                  g_object_get_data(G_OBJECT(button), "floating-toolbox-preset"));
                          if (self->sizeDropdown) {
                              gtk_drop_down_set_selected(GTK_DROP_DOWN(self->sizeDropdown), selectedPreset);
                          }
                      }));
            g_object_set_data(G_OBJECT(gtk_widget_get_last_child(row)), "floating-toolbox-preset",
                              GUINT_TO_POINTER(preset));
        } else if (token.starts_with("COLOR(") && token.back() == ')') {
            unsigned int swatchIndex = 0;
            bool validIndex = true;
            for (std::size_t index = 6; index + 1 < token.size(); ++index) {
                const unsigned int digit = static_cast<unsigned int>(token[index] - '0');
                if (swatchIndex > 100 || digit > 9) {
                    validIndex = false;
                    break;
                }
                swatchIndex = swatchIndex * 10 + digit;
            }
            if (!validIndex || swatchIndex >= importedConfig.palette.swatches.size()) {
                auto* unsupported = gtk_button_new_with_label(token.c_str());
                gtk_widget_set_sensitive(unsupported, false);
                gtk_widget_set_tooltip_text(unsupported, "This color index is not present in the imported palette");
                gtk_box_append(GTK_BOX(row), unsupported);
            } else {
                auto* swatch = gtk_button_new();
                gtk_widget_add_css_class(swatch, "flat");
                auto* dot = gtk_drawing_area_new();
                gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(dot), 20);
                gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(dot), 20);
                const auto color = importedConfig.palette.swatches[swatchIndex].color;
                gtk_drawing_area_set_draw_func(
                        GTK_DRAWING_AREA(dot),
                        +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
                            const auto* rgba = static_cast<const std::array<double, 3>*>(data);
                            cairo_arc(cr, width / 2.0, height / 2.0, std::max(2.0, std::min(width, height) / 2.0 - 2.0),
                                      0, 2 * G_PI);
                            cairo_set_source_rgb(cr, (*rgba)[0], (*rgba)[1], (*rgba)[2]);
                            cairo_fill_preserve(cr);
                            cairo_set_source_rgba(cr, 0.45, 0.45, 0.45, 0.65);
                            cairo_set_line_width(cr, 1.0);
                            cairo_stroke(cr);
                        },
                        new std::array<double, 3>(color), +[](gpointer data) {
                            delete static_cast<std::array<double, 3>*>(data);
                        });
                gtk_button_set_child(GTK_BUTTON(swatch), dot);
                gtk_widget_set_tooltip_text(swatch, importedConfig.palette.swatches[swatchIndex].name.c_str());
                gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                               importedConfig.palette.swatches[swatchIndex].name.c_str(), -1);
                g_object_set_data(G_OBJECT(swatch), "floating-toolbox-color-index", GUINT_TO_POINTER(swatchIndex));
                g_signal_connect(swatch, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                                     auto* self = static_cast<Prototype*>(data);
                                     const auto index = GPOINTER_TO_UINT(
                                             g_object_get_data(G_OBJECT(button), "floating-toolbox-color-index"));
                                     self->selectPaletteColor(index);
                                 }),
                                 this);
                gtk_box_append(GTK_BOX(row), swatch);
            }
        } else if (token == "COLOR_SELECT") {
            addButton("Color…", G_CALLBACK(+[](GtkButton*, gpointer data) {
                          auto* self = static_cast<Prototype*>(data);
                          gtk_widget_activate(self->colorButton);
                      }));
        } else if (token == "ZOOM_IN" || token == "ZOOM_OUT") {
            const bool zoomIn = token == "ZOOM_IN";
            addButton(zoomIn ? "Zoom in" : "Zoom out", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                          auto* self = static_cast<Prototype*>(data);
                          const bool zoomIn = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "zoom-in")) != 0;
                          self->zoom(self->state.steppedZoom(zoomIn, false), gtk_widget_get_width(self->canvas) / 2.0,
                                     gtk_widget_get_height(self->canvas) / 2.0);
                      }));
            g_object_set_data(G_OBJECT(gtk_widget_get_last_child(row)), "zoom-in", GINT_TO_POINTER(zoomIn));
        } else if (token == "SEPARATOR") {
            gtk_box_append(GTK_BOX(row), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        } else {
            auto* unsupported = gtk_button_new_with_label(token.c_str());
            gtk_widget_set_sensitive(unsupported, false);
            gtk_widget_set_tooltip_text(unsupported, "This toolbar action is not ported yet");
            gtk_box_append(GTK_BOX(row), unsupported);
        }
    }

    void rebuildFloatingToolboxRows() {
        if (!floatingToolboxRowsContainer) {
            return;
        }
        if (floatingToolboxToolButtons.size() > floatingToolboxStaticToolButtonCount) {
            floatingToolboxToolButtons.erase(
                    floatingToolboxToolButtons.begin() +
                            static_cast<std::ptrdiff_t>(floatingToolboxStaticToolButtonCount),
                    floatingToolboxToolButtons.end());
        }
        while (auto* child = gtk_widget_get_first_child(floatingToolboxRowsContainer)) {
            gtk_box_remove(GTK_BOX(floatingToolboxRowsContainer), child);
        }

        for (const auto& items: importedConfig.settings.floatingToolboxRows) {
            auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            for (const auto& token: items) {
                addFloatingToolboxAction(row, token);
            }
            if (gtk_widget_get_first_child(row)) {
                gtk_box_append(GTK_BOX(floatingToolboxRowsContainer), row);
            }
        }
        if (floatingToolbox && gtk_widget_get_visible(floatingToolbox)) {
            positionFloatingToolbox(floatingToolboxAnchor.x, floatingToolboxAnchor.y);
        }
    }

    void selectPaletteColor(size_t index) {
        if (index >= importedConfig.palette.swatches.size() || !colorButton) {
            return;
        }
        const auto& color = importedConfig.palette.swatches[index].color;
        const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]),
                           static_cast<float>(color[2]), 1.0f};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
        if (palettePopover) {
            gtk_popover_popdown(GTK_POPOVER(palettePopover));
        }
        gtk_widget_grab_focus(textEditor && textEditor->active() ? textEditor->widget() : canvas);
    }

    void createFloatingToolbox() {
        floatingToolbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(floatingToolbox, "card");
        gtk_widget_add_css_class(floatingToolbox, "floating-toolbox");
        gtk_widget_set_halign(floatingToolbox, GTK_ALIGN_START);
        gtk_widget_set_valign(floatingToolbox, GTK_ALIGN_START);
        gtk_widget_set_visible(floatingToolbox, false);
        gtk_widget_set_margin_start(floatingToolbox, 10);
        gtk_widget_set_margin_top(floatingToolbox, 10);

        auto* contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_top(contents, 10);
        gtk_widget_set_margin_bottom(contents, 10);
        gtk_widget_set_margin_start(contents, 10);
        gtk_widget_set_margin_end(contents, 10);
        gtk_box_append(GTK_BOX(floatingToolbox), contents);

        auto* heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        auto* title = gtk_label_new("Floating toolbox");
        gtk_widget_add_css_class(title, "heading");
        gtk_widget_set_halign(title, GTK_ALIGN_START);
        gtk_widget_set_hexpand(title, true);
        gtk_box_append(GTK_BOX(heading), title);
        auto* close = button(
                "window-close-symbolic", "Hide floating toolbox",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->hideFloatingToolbox(); }));
        gtk_box_append(GTK_BOX(contents), heading);

        auto* grid = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        const std::array<std::pair<Tool, const char*>, 14> tools = {{
                {Tool::Pen, "Pen"},
                {Tool::Highlighter, "Highlight"},
                {Tool::Eraser, "Eraser"},
                {Tool::Hand, "Hand"},
                {Tool::SelectRectangle, "Rectangle"},
                {Tool::SelectLasso, "Lasso"},
                {Tool::SelectObject, "Select object"},
                {Tool::Text, "Text"},
                {Tool::Link, "Link"},
                {Tool::PlayObject, "Play audio"},
                {Tool::LaserPointerPen, "Laser pen"},
                {Tool::LaserPointerHighlighter, "Laser marker"},
                {Tool::SelectPdfTextLinear, "PDF text"},
                {Tool::SelectPdfTextRect, "PDF area"},
        }};
        floatingToolboxToolButtons.clear();
        for (size_t index = 0; index < tools.size(); ++index) {
            const auto [tool, label] = tools[index];
            auto* toolButton = gtk_toggle_button_new_with_label(label);
            gtk_widget_add_css_class(toolButton, "flat");
            gtk_widget_set_size_request(toolButton, 82, 38);
            gtk_grid_attach(GTK_GRID(grid), toolButton, static_cast<int>(index % 4), static_cast<int>(index / 4), 1, 1);
            floatingToolboxToolButtons.emplace_back(tool, toolButton);
            g_object_set_data(G_OBJECT(toolButton), "floating-toolbox-tool", GINT_TO_POINTER(static_cast<int>(tool)));
            g_signal_connect(toolButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 if (!gtk_toggle_button_get_active(button) || self->syncingTools) {
                                     return;
                                 }
                                 const auto tool = static_cast<Tool>(
                                         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "floating-toolbox-tool")));
                                 self->selectTool(tool);
                                 self->hideFloatingToolbox();
                             }),
                             this);
        }
        addFloatingToolboxToolAction(heading, Tool::SmartSelect, "Smart Select");
        gtk_box_append(GTK_BOX(heading), close);
        auto* moreTools = gtk_expander_new("More tools");
        gtk_expander_set_child(GTK_EXPANDER(moreTools), grid);
        gtk_box_append(GTK_BOX(contents), moreTools);
        floatingToolboxStaticToolButtonCount = floatingToolboxToolButtons.size();

        floatingToolboxRowsContainer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_append(GTK_BOX(contents), floatingToolboxRowsContainer);
        rebuildFloatingToolboxRows();

        gtk_overlay_add_overlay(GTK_OVERLAY(canvasOverlay), floatingToolbox);
        auto* motion = gtk_event_controller_motion_new();
        g_signal_connect(motion, "enter", G_CALLBACK(+[](GtkEventControllerMotion*, double, double, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->floatingToolboxHideSource) {
                                 g_source_remove(self->floatingToolboxHideSource);
                                 self->floatingToolboxHideSource = 0;
                             }
                         }),
                         this);
        g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
                             static_cast<Prototype*>(data)->scheduleFloatingToolboxHide();
                         }),
                         this);
        gtk_widget_add_controller(floatingToolbox, motion);
    }

    void positionFloatingToolbox(double x, double y) {
        if (!floatingToolbox || !canvas) {
            return;
        }
        int naturalWidth = 0;
        gtk_widget_measure(floatingToolbox, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &naturalWidth, nullptr, nullptr);
        int naturalHeight = 0;
        gtk_widget_measure(floatingToolbox, GTK_ORIENTATION_VERTICAL, naturalWidth, nullptr, &naturalHeight, nullptr,
                           nullptr);
        const int canvasWidth = gtk_widget_get_width(canvas);
        const int canvasHeight = gtk_widget_get_height(canvas);
        const int margin = 10;
        const int maxX = std::max(margin, canvasWidth - naturalWidth - margin);
        const int maxY = std::max(margin, canvasHeight - naturalHeight - margin);
        const int left = std::clamp(static_cast<int>(std::lround(x - naturalWidth / 2.0)), margin, maxX);
        const int top = std::clamp(static_cast<int>(std::lround(y - naturalHeight / 2.0)), margin, maxY);
        gtk_widget_set_margin_start(floatingToolbox, left);
        gtk_widget_set_margin_top(floatingToolbox, top);
    }

    void showFloatingToolbox(double x, double y) {
        if (!floatingToolbox) {
            return;
        }
        if (floatingToolboxHideSource) {
            g_source_remove(floatingToolboxHideSource);
            floatingToolboxHideSource = 0;
        }
        floatingToolboxAnchor = {x, y};
        gtk_widget_set_visible(floatingToolbox, true);
        positionFloatingToolbox(x, y);
        gtk_widget_queue_allocate(canvasOverlay);
    }

    void hideFloatingToolbox() {
        if (floatingToolboxHideSource) {
            g_source_remove(floatingToolboxHideSource);
            floatingToolboxHideSource = 0;
        }
        if (floatingToolbox) {
            gtk_widget_set_visible(floatingToolbox, false);
        }
    }

    void scheduleFloatingToolboxHide() {
        if (!floatingToolbox || !gtk_widget_get_visible(floatingToolbox) || floatingToolboxHideSource) {
            return;
        }
        floatingToolboxHideSource = g_timeout_add(
                180,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    self->floatingToolboxHideSource = 0;
                    self->hideFloatingToolbox();
                    return G_SOURCE_REMOVE;
                },
                this);
    }

    guint selectionModeIndex(Tool tool) const {
        if (tool == Tool::VerticalSpace) {
            return 8;
        }
        if (state.selectionAcrossLayers && tool == Tool::SelectRectangle) {
            return 3;
        }
        if (state.selectionAcrossLayers && tool == Tool::SelectLasso) {
            return 4;
        }
        switch (tool) {
            case Tool::SelectRectangle:
                return 0;
            case Tool::SelectLasso:
                return 1;
            case Tool::SmartSelect:
                return 2;
            case Tool::SelectObject:
                return 5;
            case Tool::SelectPdfTextLinear:
                return 6;
            case Tool::SelectPdfTextRect:
                return 7;
            default:
                return 0;
        }
    }

    void activateSelection(Tool tool, bool acrossLayers = false) {
        selectionTool = tool;
        state.selectionAcrossLayers = acrossLayers;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
        selectTool(tool);
    }

    void copyImagesToClipboard(const std::vector<prototype::CanvasImage>& images) {
        const auto native = prototype::clipboard::encodeImages(images);
        const auto png = encodeImagesForClipboard(images);
        auto* nativeBytes = g_bytes_new(native.data(), native.size());
        auto* pngBytes = g_bytes_new(png.data(), png.size());
        GdkContentProvider* providers[] = {
                gdk_content_provider_new_for_bytes(prototype::clipboard::mime, nativeBytes),
                gdk_content_provider_new_for_bytes("image/png", pngBytes)};
        g_bytes_unref(nativeBytes);
        g_bytes_unref(pngBytes);
        auto* provider = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
        const bool copied = gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas), provider);
        g_object_unref(provider);
        if (!copied) {
            throw std::runtime_error("Cannot claim the clipboard");
        }
    }

    void copySelection(bool cut = false) {
        if (busy) return;
        resetInput(true);
        try {
            if (selectedImageIndex && *selectedImageIndex < state.images.size()) {
                copyImagesToClipboard({state.images[*selectedImageIndex]});
                if (cut) {
                    state.deleteImage(*selectedImageIndex);
                    selectedImageIndex.reset();
                }
                update();
                return;
            }
            if (!state.hasSelection()) return;
            if (!state.selectedTexts.empty()) {
                throw std::runtime_error("Copying text selections is not supported yet");
            }
            if (!state.selectedImages.empty()) {
                if (!state.selected.empty()) {
                    throw std::runtime_error("Copying mixed image and stroke selections is not supported yet");
                }
                std::vector<prototype::CanvasImage> images;
                images.reserve(state.selectedImages.size());
                for (const auto index: state.selectedImages) {
                    images.push_back(state.images.at(index));
                }
                copyImagesToClipboard(images);
                if (cut) state.deleteSelection();
                update();
                return;
            }
            const auto bytes = prototype::clipboard::encode(state.copySelection());
            auto* data = g_bytes_new(bytes.data(), bytes.size());
            auto* provider = gdk_content_provider_new_for_bytes(prototype::clipboard::mime, data);
            const bool copied = gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas), provider);
            g_object_unref(provider);
            g_bytes_unref(data);
            if (!copied) throw std::runtime_error("Cannot claim the clipboard");
            if (cut) state.deleteSelection();
            update();
        } catch (const std::exception& error) {
            showError(error.what());
        }
    }

    void duplicateSelection() {
        if (busy) return;
        resetInput(true);
        if (selectedImageIndex && *selectedImageIndex < state.images.size()) {
            const auto duplicate = state.images[*selectedImageIndex];
            const auto bounds = prototype::canvasImageBounds(duplicate);
            selectedImageIndex.reset();
            if (!state.pasteImages({duplicate}, duplicate.page,
                                   {(bounds.x0 / 2.0 + bounds.x1 / 2.0) + 20,
                                    (bounds.y0 / 2.0 + bounds.y1 / 2.0) + 20})) {
                showError("Cannot duplicate the selected image");
            }
            update();
            return;
        }
        if (!state.hasSelection()) return;
        if (!state.selectedTexts.empty()) {
            showError("Duplicating text selections is not supported yet");
            return;
        }
        if (!state.selectedImages.empty()) {
            if (!state.selected.empty()) {
                showError("Duplicating mixed image and stroke selections is not supported yet");
                return;
            }
            const auto bounds = state.selectionBox();
            std::vector<prototype::CanvasImage> images;
            images.reserve(state.selectedImages.size());
            for (const auto index: state.selectedImages) {
                images.push_back(state.images.at(index));
            }
            if (!bounds || !state.pasteImages(std::move(images), bounds->page,
                                              {(bounds->x0 / 2.0 + bounds->x1 / 2.0) + 20,
                                               (bounds->y0 / 2.0 + bounds->y1 / 2.0) + 20})) {
                showError("Cannot duplicate the selected images");
                return;
            }
            update();
            return;
        }
        const auto box = state.selectionBox();
        state.pasteStrokes(state.copySelection(), box->page,
                           {(box->x0+box->x1)/2 + 20, (box->y0+box->y1)/2 + 20});
        update();
    }

    void finishClipboardRead(const std::string& error) {
        if (clipboardTimer) g_source_remove(clipboardTimer);
        clipboardTimer = 0;
        g_clear_object(&clipboardCancellation);
        busy = false;
        gtk_widget_set_sensitive(contentView, true);
        if (!error.empty()) showError(error);
        update();
        g_application_release(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
    }

    bool placePastedImages(std::vector<prototype::CanvasImage> images) {
        if (images.empty()) {
            return false;
        }
        const auto pageSize = state.pageSize(state.page);
        auto center = state.pageLocalPoint(state.page, gtk_widget_get_width(canvas) / 2.0,
                                           gtk_widget_get_height(canvas) / 2.0);
        center.x = std::clamp(center.x, 0.0, pageSize.width);
        center.y = std::clamp(center.y, 0.0, pageSize.height);

        selectedImageIndex.reset();
        if (!state.pasteImages(std::move(images), state.page, center)) {
            return false;
        }
        activateSelection(Tool::SmartSelect);
        return true;
    }

    bool placePastedImage(prototype::CanvasImage image) {
        return placePastedImages({std::move(image)});
    }

    void pasteExternalImage() {
        gdk_clipboard_read_texture_async(gtk_widget_get_clipboard(canvas), clipboardCancellation,
                                        +[](GObject* object, GAsyncResult* result, gpointer data) {
            auto* self = static_cast<Prototype*>(data);
            GError* error = nullptr;
            auto* texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(object), result, &error);
            if (!texture) {
                const std::string message = error ? error->message : "Clipboard has no image";
                g_clear_error(&error);
                self->finishClipboardRead(message);
                return;
            }

            const int width = gdk_texture_get_width(texture);
            const int height = gdk_texture_get_height(texture);
            if (width <= 0 || height <= 0 ||
                static_cast<size_t>(width) > prototype::maxCanvasImagePixels / static_cast<size_t>(height)) {
                g_object_unref(texture);
                self->finishClipboardRead("Clipboard image exceeds the 32 megapixel limit");
                return;
            }
            GBytes* png = gdk_texture_save_to_png_bytes(texture);
            g_object_unref(texture);
            if (!png) {
                self->finishClipboardRead("Cannot encode the clipboard image");
                return;
            }
            gsize length = 0;
            const auto* bytes = static_cast<const char*>(g_bytes_get_data(png, &length));
            if (length == 0 || length > prototype::maxCanvasImageBytes) {
                g_bytes_unref(png);
                self->finishClipboardRead("Clipboard image exceeds the 64 MiB limit");
                return;
            }
            auto image = std::make_shared<prototype::CanvasImage>();
            image->encodedData.assign(bytes, length);
            g_bytes_unref(png);
            auto* cancellation = self->clipboardCancellation;
            prototype::bridge::documentJob([image, width, height, cancellation] {
                if (g_cancellable_is_cancelled(cancellation)) {
                    throw std::runtime_error("Clipboard read cancelled");
                }
                int decodedWidth = 0;
                int decodedHeight = 0;
                image->surface = prototype::decodeCanvasImage(image->encodedData, decodedWidth, decodedHeight);
                if (decodedWidth != width || decodedHeight != height) {
                    throw std::runtime_error("Clipboard image dimensions changed while decoding");
                }
                image->pixelWidth = decodedWidth;
                image->pixelHeight = decodedHeight;
            }, [self, image](const std::string& decodeError) {
                std::string failure = decodeError;
                if (failure.empty() && !self->placePastedImage(std::move(*image))) {
                    failure = "Cannot place clipboard image";
                }
                self->finishClipboardRead(failure);
            });
        }, this);
    }

    void pasteSelection() {
        if (busy || choosingFile || messageDialog) return;
        resetInput(true);
        // Freeze document actions through completion, so a delayed clipboard
        // cannot insert into a newly opened document. Bound both time and size.
        busy = true;
        busyText = "Reading clipboard…";
        gtk_widget_set_sensitive(contentView, false);
        updateTitle();
        g_application_hold(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
        clipboardCancellation = g_cancellable_new();
        clipboardTimer = g_timeout_add_seconds(5, +[](gpointer data) -> gboolean {
            auto* self = static_cast<Prototype*>(data);
            self->clipboardTimer = 0;
            g_cancellable_cancel(self->clipboardCancellation);
            return G_SOURCE_REMOVE;
        }, this);
        const char* types[] = {prototype::clipboard::mime, nullptr};
        gdk_clipboard_read_async(gtk_widget_get_clipboard(canvas), types, G_PRIORITY_DEFAULT,
                                clipboardCancellation, +[](GObject* object, GAsyncResult* result, gpointer data) {
            auto* self = static_cast<Prototype*>(data);
            GError* error = nullptr;
            auto* rawStream = gdk_clipboard_read_finish(GDK_CLIPBOARD(object), result, nullptr, &error);
            if (!rawStream) {
                g_clear_error(&error);
                self->pasteExternalImage();
                return;
            }
            auto stream = std::shared_ptr<GInputStream>(rawStream, [](GInputStream* stream) { g_object_unref(stream); });
            auto decoded = std::make_shared<prototype::clipboard::DecodedPayload>();
            auto* cancellation = self->clipboardCancellation;
            prototype::bridge::documentJob([stream, decoded, cancellation] {
                std::string bytes;
                char buffer[16384];
                while (true) {
                    GError* error = nullptr;
                    const auto read = g_input_stream_read(stream.get(), buffer, sizeof(buffer), cancellation, &error);
                    if (read < 0) {
                        const std::string message = error ? error->message : "Cannot read clipboard";
                        g_clear_error(&error);
                        throw std::runtime_error(message);
                    }
                    if (!read) break;
                    if (size_t(read) > prototype::clipboard::maxBytes - bytes.size())
                        throw std::runtime_error("Clipboard exceeds the 64 MiB limit");
                    bytes.append(buffer, size_t(read));
                }
                *decoded = prototype::clipboard::decodePayload(bytes);
                for (auto& image: decoded->images) {
                    const int expectedWidth = image.pixelWidth;
                    const int expectedHeight = image.pixelHeight;
                    int width = 0;
                    int height = 0;
                    image.surface = prototype::decodeCanvasImage(image.encodedData, width, height);
                    const double scaleX = static_cast<double>(expectedWidth) / width;
                    const double scaleY = static_cast<double>(expectedHeight) / height;
                    image.transform.xx *= scaleX;
                    image.transform.yx *= scaleX;
                    image.transform.xy *= scaleY;
                    image.transform.yy *= scaleY;
                    image.pixelWidth = width;
                    image.pixelHeight = height;
                }
            }, [self, decoded](const std::string& error) {
                std::string failure = error;
                if (failure.empty()) {
                    if (!decoded->images.empty()) {
                        if (!self->placePastedImages(std::move(decoded->images)))
                            failure = "Cannot place clipboard images";
                    } else {
                        self->selectedImageIndex.reset();
                        const auto sz = self->state.pageSize(self->state.page);
                        auto center = self->state.pageLocalPoint(self->state.page,
                                gtk_widget_get_width(self->canvas) / 2.0, gtk_widget_get_height(self->canvas) / 2.0);
                        center.x = std::clamp(center.x, 0.0, sz.width);
                        center.y = std::clamp(center.y, 0.0, sz.height);
                        if (!self->state.pasteStrokes(std::move(decoded->strokes), self->state.page, center))
                            failure = "Cannot place clipboard strokes";
                        else self->activateSelection(self->selectionTool);
                    }
                }
                self->finishClipboardRead(failure);
            });
        }, this);
    }

    std::optional<size_t> textAt(int page, prototype::Point point) const {
        std::optional<size_t> best;
        size_t bestOrder = 0;
        for (size_t index = 0; index < state.texts.size(); ++index) {
            const auto& item = state.texts[index];
            if (!item.isLink && item.page == page && prototype::textContainsPoint(item, point.x, point.y) &&
                (!best || item.layerOrder >= bestOrder)) {
                best = index;
                bestOrder = item.layerOrder;
            }
        }
        return best;
    }

    void closeLinkEditor() {
        if (messageDialog) {
            adw_dialog_close(ADW_DIALOG(messageDialog));
        }
        messageDialog = nullptr;
        linkTextInput = nullptr;
        linkUrlInput = nullptr;
        linkPrefixDropdown = nullptr;
        linkFontEntry = nullptr;
        linkFontSize = nullptr;
        linkAlignmentDropdown = nullptr;
        editingLinkIndex.reset();
        linkEditorPage = -1;
    }

    void openLinkEditor(int page, prototype::Point position, std::optional<size_t> index) {
        if (busy || choosingFile || messageDialog) {
            return;
        }
        if (!nativeSession) {
            showError("Link editing requires an open document worker.");
            return;
        }
        if (index && (*index >= state.texts.size() || !state.texts[*index].isLink)) {
            return;
        }
        resetInput(true);
        if (linkInfoPopover) {
            gtk_popover_popdown(GTK_POPOVER(linkInfoPopover));
        }

        const auto* existing = index ? &state.texts[*index] : nullptr;
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(existing ? "Edit Link" : "Add Link",
                                                              "Link text is shown underlined on the page."));
        auto* grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
        gtk_widget_set_margin_top(grid, 8);
        gtk_widget_set_margin_bottom(grid, 8);
        gtk_widget_set_margin_start(grid, 8);
        gtk_widget_set_margin_end(grid, 8);

        auto attachRow = [&](int row, const char* title, GtkWidget* control) {
            auto* label = gtk_label_new(title);
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            gtk_widget_set_hexpand(control, true);
            gtk_grid_attach(GTK_GRID(grid), control, 1, row, 1, 1);
        };

        auto* textView = gtk_text_view_new();
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(textView), GTK_WRAP_WORD_CHAR);
        auto* textScroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(textScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(textScroll), textView);
        gtk_widget_set_size_request(textScroll, 340, 76);
        attachRow(0, "Text", textScroll);
        linkTextInput = GTK_TEXT_VIEW(textView);

        const char* prefixOptions[] = {"https://", "http://", "mailto:", "file://", "Custom", nullptr};
        linkPrefixDropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(prefixOptions));
        auto* urlBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_append(GTK_BOX(urlBox), GTK_WIDGET(linkPrefixDropdown));
        linkUrlInput = GTK_ENTRY(gtk_entry_new());
        gtk_widget_set_hexpand(GTK_WIDGET(linkUrlInput), true);
        gtk_box_append(GTK_BOX(urlBox), GTK_WIDGET(linkUrlInput));
        attachRow(1, "URL", urlBox);

        linkFontEntry = GTK_ENTRY(gtk_entry_new());
        gtk_editable_set_width_chars(GTK_EDITABLE(linkFontEntry), 18);
        attachRow(2, "Font", GTK_WIDGET(linkFontEntry));
        linkFontSize = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, 200, 0.5));
        gtk_spin_button_set_digits(linkFontSize, 1);
        attachRow(3, "Size", GTK_WIDGET(linkFontSize));
        const char* alignmentOptions[] = {"Left", "Center", "Right", nullptr};
        linkAlignmentDropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(alignmentOptions));
        attachRow(4, "Alignment", GTK_WIDGET(linkAlignmentDropdown));

        if (existing) {
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), existing->text.c_str(), -1);
            std::string urlValue = existing->linkUrl;
            guint prefix = 4;
            const std::array<std::string, 4> prefixes{"https://", "http://", "mailto:", "file://"};
            for (guint candidate = 0; candidate < prefixes.size(); ++candidate) {
                if (urlValue.starts_with(prefixes[candidate])) {
                    prefix = candidate;
                    urlValue.erase(0, prefixes[candidate].size());
                    break;
                }
            }
            gtk_drop_down_set_selected(linkPrefixDropdown, prefix);
            gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), urlValue.c_str());
            gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), existing->fontName.c_str());
            gtk_spin_button_set_value(linkFontSize, existing->fontSize);
            gtk_drop_down_set_selected(linkAlignmentDropdown, static_cast<guint>(existing->alignment));
        } else {
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), "Link", -1);
            gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), "xournalpp.github.io");
            gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), state.textFontName.c_str());
            gtk_spin_button_set_value(linkFontSize, state.textFontSize);
            gtk_drop_down_set_selected(linkPrefixDropdown, 0);
            gtk_drop_down_set_selected(linkAlignmentDropdown, 0);
        }

        adw_alert_dialog_set_extra_child(messageDialog, grid);
        adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "save", existing ? "Save" : "Add", nullptr);
        adw_alert_dialog_set_response_appearance(messageDialog, "save", ADW_RESPONSE_SUGGESTED);
        adw_alert_dialog_set_close_response(messageDialog, "cancel");
        adw_alert_dialog_set_default_response(messageDialog, "save");
        editingLinkIndex = index;
        linkEditorPage = page;
        linkEditorPosition = position;
        g_signal_connect(messageDialog, "response", G_CALLBACK((+[](AdwAlertDialog*, const char* response, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const bool save = response && std::string_view(response) == "save";
                             std::optional<prototype::TextItem> edited;
                             const auto oldIndex = self->editingLinkIndex;
                             if (save && self->linkTextInput && self->linkUrlInput && self->linkPrefixDropdown &&
                                 self->linkFontEntry && self->linkFontSize && self->linkAlignmentDropdown) {
                                 GtkTextIter start, end;
                                 auto* buffer = gtk_text_view_get_buffer(self->linkTextInput);
                                 gtk_text_buffer_get_bounds(buffer, &start, &end);
                                 gchar* rawText = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
                                 std::string text = rawText ? rawText : "";
                                 g_free(rawText);
                                 if (std::all_of(text.begin(), text.end(), [](unsigned char ch) { return g_ascii_isspace(ch); })) {
                                     text = "Link";
                                 }

                                 static constexpr std::array<const char*, 4> prefixes{
                                         "https://", "http://", "mailto:", "file://"};
                                 const guint prefix = gtk_drop_down_get_selected(self->linkPrefixDropdown);
                                 std::string url = gtk_editable_get_text(GTK_EDITABLE(self->linkUrlInput));
                                 if (url.empty()) {
                                     url = "xournalpp.github.io";
                                 }
                                 if (prefix < prefixes.size()) {
                                     url.insert(0, prefixes[prefix]);
                                 }
                                 auto item = oldIndex && *oldIndex < self->state.texts.size() ?
                                                     self->state.texts[*oldIndex] : prototype::TextItem{};
                                 item.isLink = true;
                                 item.linkUrl = std::move(url);
                                 item.text = std::move(text);
                                 const char* font = gtk_editable_get_text(GTK_EDITABLE(self->linkFontEntry));
                                 item.fontName = font && *font ? font : "Sans";
                                 item.fontSize = gtk_spin_button_get_value(self->linkFontSize);
                                 item.alignment = std::min<int>(2, gtk_drop_down_get_selected(self->linkAlignmentDropdown));
                                 item.justify = false;
                                 item.wrapWidth = -1;
                                 item.styleRuns.clear();
                                 if (!oldIndex) {
                                     item.page = self->linkEditorPage;
                                     item.x = self->linkEditorPosition.x;
                                     item.y = self->linkEditorPosition.y;
                                     item.color = 0xffff00ffU;
                                 }
                                 const auto [width, height] = prototype::textNaturalSize(item);
                                 item.layoutWidth = width;
                                 item.layoutHeight = height;
                                 edited = std::move(item);
                             }
                             self->messageDialog = nullptr;
                             self->linkTextInput = nullptr;
                             self->linkUrlInput = nullptr;
                             self->linkPrefixDropdown = nullptr;
                             self->linkFontEntry = nullptr;
                             self->linkFontSize = nullptr;
                             self->linkAlignmentDropdown = nullptr;
                             self->editingLinkIndex.reset();
                             self->linkEditorPage = -1;
                             if (edited) {
                                 const bool changed = oldIndex ? self->state.replaceText(*oldIndex, std::move(*edited)) :
                                                                 self->state.insertText(std::move(*edited));
                                 if (!changed) {
                                     self->showError("The link could not be added to the active layer.");
                                 } else {
                                     self->thumbnails.clear();
                                 }
                             }
                             if (self->canvas) {
                                 gtk_widget_grab_focus(self->canvas);
                                 self->update();
                             }
                         })), this);
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
        gtk_widget_grab_focus(GTK_WIDGET(linkUrlInput));
    }

    void openLinkUrl(const std::string& url) {
        if (!linkVerifyMode.empty()) {
            openedLinkForTest = url;
            return;
        }
        g_app_info_launch_default_for_uri_async(
                url.c_str(), nullptr, nullptr,
                +[](GObject*, GAsyncResult* result, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    GError* error = nullptr;
                    if (!g_app_info_launch_default_for_uri_finish(result, &error)) {
                        const std::string message = error ? error->message : "No application can open this link.";
                        self->showAlert("Unable to open link", message);
                    }
                    g_clear_error(&error);
                },
                this);
    }

    void showLinkInfo(size_t index) {
        if (!linkInfoPopover || index >= state.texts.size() || !state.texts[index].isLink) {
            return;
        }
        linkInfoIndex = index;
        const auto& item = state.texts[index];
        const auto message = item.linkUrl + "\nCtrl-click to open · Double-click to edit";
        gtk_label_set_text(GTK_LABEL(linkInfoLabel), message.c_str());

        std::array<prototype::Point, 4> corners{{
                {0, 0}, {item.layoutWidth, 0},
                {item.layoutWidth, item.layoutHeight}, {0, item.layoutHeight}}};
        for (auto& corner: corners) {
            const auto local = prototype::Point{item.xx * corner.x + item.xy * corner.y,
                                                item.yx * corner.x + item.yy * corner.y};
            corner = state.pageScreenPoint(item.page, local.x + item.x, local.y + item.y);
        }
        double left = corners[0].x, right = corners[0].x, top = corners[0].y, bottom = corners[0].y;
        for (const auto& corner: corners) {
            left = std::min(left, corner.x);
            right = std::max(right, corner.x);
            top = std::min(top, corner.y);
            bottom = std::max(bottom, corner.y);
        }
        GdkRectangle pointing{static_cast<int>(std::floor(left)), static_cast<int>(std::floor(top)),
                              std::max(1, static_cast<int>(std::ceil(right - left))),
                              std::max(1, static_cast<int>(std::ceil(bottom - top)))};
        gtk_popover_set_pointing_to(GTK_POPOVER(linkInfoPopover), &pointing);
        if (!gtk_widget_get_visible(linkInfoPopover)) {
            gtk_popover_popup(GTK_POPOVER(linkInfoPopover));
        }
    }

    void updateLinkHover(double x, double y) {
        if (state.brush.tool != Tool::Link || !canvas) {
            return;
        }
        const auto hit = state.hitPage(x, y);
        const auto link = hit ? state.pickLink(hit->page, hit->local) : std::nullopt;
        gtk_widget_set_cursor_from_name(canvas, link ? "pointer" : nullptr);
        if (link) {
            if (linkInfoIndex != link) {
                showLinkInfo(*link);
            }
        } else {
            linkInfoIndex.reset();
            if (linkInfoPopover) {
                gtk_popover_popdown(GTK_POPOVER(linkInfoPopover));
            }
        }
    }

    void handleLinkClick(double x, double y, int presses, bool controlDown) {
        const auto hit = state.hitPage(x, y);
        if (!hit) {
            return;
        }
        state.selectActivePage(hit->page);
        if (const auto index = state.pickLink(hit->page, hit->local)) {
            if (controlDown) {
                openLinkUrl(state.texts[*index].linkUrl);
            } else if (presses >= 2) {
                openLinkEditor(hit->page, {state.texts[*index].x, state.texts[*index].y}, index);
            } else {
                showLinkInfo(*index);
            }
        } else {
            openLinkEditor(hit->page, hit->local, std::nullopt);
        }
    }

    static uint32_t packTextColor(const std::array<double, 3>& color) {
        const auto red = static_cast<uint32_t>(std::lround(std::clamp(color[0], 0.0, 1.0) * 255));
        const auto green = static_cast<uint32_t>(std::lround(std::clamp(color[1], 0.0, 1.0) * 255));
        const auto blue = static_cast<uint32_t>(std::lround(std::clamp(color[2], 0.0, 1.0) * 255));
        return 0xff000000U | (red << 16U) | (green << 8U) | blue;
    }

    void syncTextColorControl(uint32_t color) {
        state.textColor = {static_cast<double>((color >> 16U) & 0xffU) / 255.0,
                           static_cast<double>((color >> 8U) & 0xffU) / 255.0,
                           static_cast<double>(color & 0xffU) / 255.0};
        if (state.brush.tool == Tool::Text) {
            state.brush.color = state.textColor;
        }
        if (colorButton) {
            const GdkRGBA rgba{static_cast<float>(state.textColor[0]), static_cast<float>(state.textColor[1]),
                               static_cast<float>(state.textColor[2]), 1};
            syncingBrush = true;
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
            syncingBrush = false;
        }
    }

    void updateTextToolbarVisibility() {
        const bool editingText = textEditor && textEditor->active();
        const bool textContext = state.brush.tool == Tool::Text || editingText;
        const bool regularColorContext = state.brush.tool != Tool::Eraser && state.brush.tool != Tool::Hand &&
                                         state.brush.tool != Tool::Link &&
                                         state.brush.tool != Tool::VerticalSpace &&
                                         !prototype::isSelectionTool(state.brush.tool);
        if (textFontEntry) {
            gtk_widget_set_visible(textFontEntry, textContext);
        }
        if (textFontSize) {
            gtk_widget_set_visible(textFontSize, textContext);
        }
        if (textFormatBox) {
            gtk_widget_set_visible(textFormatBox, textContext);
        }
        if (colorBox) {
            gtk_widget_set_visible(colorBox, textContext || regularColorContext);
        }
        if (colorButton) {
            gtk_widget_set_tooltip_text(colorButton, textContext ? "Text color" : "Ink color");
        }
    }

    bool beginTextTransform(Device device, double x, double y) {
        if (state.brush.tool != Tool::Text || !nativeSession || (textEditor && textEditor->active()) || busy || choosingFile ||
            messageDialog || input.spaceHeld || input.panning() ||
            (device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
            return false;
        }

        auto hit = state.hitPage(x, y);
        if (!hit) {
            hit = state.hitSelectionHandle(x, y);
        }
        if (!hit) {
            return false;
        }
        if (state.pickLink(hit->page, hit->local)) {
            return false;
        }

        const bool textOnlySelection = !state.selectedTexts.empty() && state.selected.empty() &&
                                       state.selectedImages.empty();
        const auto bounds = state.selectionBox();
        bool insideSelectedBounds = false;
        bool onSelectedHandle = false;
        if (textOnlySelection && bounds && bounds->page == hit->page) {
            const auto padding = 5 / state.zoom;
            insideSelectedBounds = hit->local.x >= bounds->x0 - padding && hit->local.x <= bounds->x1 + padding &&
                                   hit->local.y >= bounds->y0 - padding && hit->local.y <= bounds->y1 + padding;
            for (const auto& handle: state.selectionHandles()) {
                if (std::hypot(handle.x - hit->local.x, handle.y - hit->local.y) <= 5 / state.zoom) {
                    onSelectedHandle = true;
                    break;
                }
            }
        }

        const auto pickedText = state.pickText(hit->page, hit->local);
        const bool pickedTextSelected = pickedText && textOnlySelection &&
                std::find(state.selectedTexts.begin(), state.selectedTexts.end(), *pickedText) !=
                        state.selectedTexts.end();
        if (!onSelectedHandle && pickedText && !pickedTextSelected) {
            state.clearSelection();
            state.selectedTexts = {*pickedText};
        } else if (!onSelectedHandle && !pickedText && !insideSelectedBounds) {
            return false;
        } else if (!textOnlySelection && pickedText) {
            state.clearSelection();
            state.selectedTexts = {*pickedText};
        } else if (!textOnlySelection && !pickedText) {
            return false;
        }

        state.selectActivePage(hit->page);
        selectedImageIndex.reset();
        const auto tool = state.brush.tool;
        state.brush.tool = Tool::SmartSelect;
        const bool started = state.beginSelection(x, y);
        state.brush.tool = tool;
        if (!started || !state.movingSelection()) {
            if (started) {
                state.cancelSelection();
            }
            return false;
        }

        textSelectionOwner = device;
        if (device == Device::Mouse) {
            textDragHandled = true;
        }
        return true;
    }

    void applyTextColor(uint32_t color) {
        if (textEditor && textEditor->active()) {
            textEditor->setTextColor(color);
            gtk_widget_grab_focus(textEditor->widget());
        } else if (state.brush.tool == Tool::Text && !state.selectedTexts.empty()) {
            if (state.recolorSelectedTexts(color)) {
                thumbnails.clear();
            }
        }
    }

    void startTextEditing(double x, double y) {
        if (!textEditor || !nativeSession || busy || choosingFile || messageDialog) {
            return;
        }
        const auto hit = state.hitPage(x, y);
        if (!hit) {
            return;
        }
        if (state.pickLink(hit->page, hit->local)) {
            return;
        }
        finishTextEditing();
        resetInput();
        state.clearSelection();
        state.selectActivePage(hit->page);
        const auto index = textAt(hit->page, hit->local);
        prototype::TextItem item;
        if (index) {
            item = state.texts[*index];
            syncTextColorControl(item.color);
        } else {
            item.page = hit->page;
            item.x = hit->local.x;
            item.y = hit->local.y;
            item.fontName = state.textFontName;
            item.fontSize = state.textFontSize;
            item.color = packTextColor(state.textColor);
        }
        textEditor->begin(item, index, state.zoom);
        textEditor->setWrap(item.wrapWidth >= 0);
        gtk_editable_set_text(GTK_EDITABLE(textFontEntry), item.fontName.c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), item.fontSize);
        updateTextToolbarVisibility();
        updateTextEditorPosition();
        update();
    }

    void finishTextEditing() {
        if (!textEditor || !textEditor->active()) {
            return;
        }
        auto completed = textEditor->finish();
        if (completed) {
            const auto [layoutWidth, layoutHeight] = prototype::textNaturalSize(completed->item);
            completed->item.layoutWidth = layoutWidth;
            completed->item.layoutHeight = layoutHeight;
            if (completed->existingIndex) {
                if (completed->hasContent) {
                    state.replaceText(*completed->existingIndex, completed->item);
                } else {
                    state.deleteText(*completed->existingIndex);
                }
            } else if (completed->hasContent) {
                state.insertText(completed->item);
            }
        }
        updateTextToolbarVisibility();
        if (canvas) {
            gtk_widget_grab_focus(canvas);
        }
        update();
    }

    void updateTextEditorPosition() {
        if (!textEditor || !textEditor->active() || !canvas) {
            return;
        }
        const auto* item = textEditor->item();
        if (!item) {
            return;
        }
        textEditor->setZoom(state.zoom);
        const auto screen = state.pageScreenPoint(item->page, item->x, item->y);
        const auto [measuredWidth, measuredHeight] = prototype::measureText(*item);
        const double availableWidth = std::max(40.0, gtk_widget_get_width(canvas) - std::max(0.0, screen.x) - 8.0);
        const double requestedWidth = item->wrapWidth >= 0 ? item->wrapWidth * state.zoom + 16.0 :
                                                              std::max(180.0, (measuredWidth + 20.0) * state.zoom);
        const int width = static_cast<int>(std::clamp(requestedWidth, 40.0, availableWidth));
        const int height = static_cast<int>(std::max(36.0, (measuredHeight + 16.0) * state.zoom));
        textEditor->position(screen.x, screen.y, width, height);
    }

    void applyTextFormat(prototype::TextEditorOverlay::Format format) {
        if (textEditor && textEditor->active()) {
            textEditor->applyFormat(format);
            gtk_widget_grab_focus(textEditor->widget());
        } else if (state.brush.tool == Tool::Text && !state.selectedTexts.empty()) {
            if (state.updateSelectedTexts([&](prototype::TextItem& item) {
                    prototype::TextEditorOverlay::applyFormatToItem(item, format);
                    const auto [width, height] = prototype::textNaturalSize(item);
                    item.layoutWidth = width;
                    item.layoutHeight = height;
                })) {
                thumbnails.clear();
                update();
            }
        }
    }

    void selectTool(Tool tool) {
        if (tool != Tool::FloatingToolbox) {
            hideFloatingToolbox();
        }
        if (textEditor && textEditor->active()) {
            finishTextEditing();
        }
        if (!prototype::isPdfTextSelectionTool(tool) && pdfTextSelection) {
            clearPdfTextSelection(false);
        }
        if (tool != Tool::SmartSelect && tool != Tool::Hand) {
            selectedImageIndex.reset();
        }
        syncingTools = true;
        if (textButton) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(textButton), tool == Tool::Text);
        }
        if (latexButton) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(latexButton), tool == Tool::Latex);
        }
        if (linkButton) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(linkButton), tool == Tool::Link);
        }
        if (playObjectButton) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(playObjectButton), tool == Tool::PlayObject);
        }
        if (floatingToolboxToolButton) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(floatingToolboxToolButton), tool == Tool::FloatingToolbox);
        }
        for (const auto& [toolButtonTool, toolButton]: floatingToolboxToolButtons) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButton), toolButtonTool == tool);
        }
        if (laserButtons[0]) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(laserButtons[0]), tool == Tool::LaserPointerPen);
        }
        if (laserButtons[1]) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(laserButtons[1]),
                                         tool == Tool::LaserPointerHighlighter);
        }
        if (prototype::isSelectionTool(tool) || tool == Tool::VerticalSpace) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
        } else if (tool == Tool::Hand) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[4]), true);
        } else if (tool == Tool::Text || tool == Tool::Latex || tool == Tool::Link || tool == Tool::PlayObject ||
                   tool == Tool::FloatingToolbox) {
            for (auto* button: toolButtons) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), false);
            }
        } else if (prototype::isLaserPointerTool(tool)) {
            for (auto* button: toolButtons) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), false);
            }
        } else if (static_cast<size_t>(tool) < 3) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[static_cast<size_t>(tool)]), true);
        }
        syncingTools = false;
        if (state.brush.tool == Tool::Pen) {
            penBrush = state.brush;
        } else if (state.brush.tool == Tool::Highlighter) {
            highlightBrush = state.brush;
        } else if (state.brush.tool == Tool::Eraser) {
            eraserBrush = state.brush;
            state.eraserWidth = eraserBrush.width;
        } else if (state.brush.tool == Tool::Latex) {
            latexBrush = state.brush;
        } else if (state.brush.tool == Tool::LaserPointerPen) {
            laserPenBrush = state.brush;
        } else if (state.brush.tool == Tool::LaserPointerHighlighter) {
            laserHighlighterBrush = state.brush;
        }
        if (textSelectionOwner) {
            stopSelectionEdgePan();
            state.cancelSelection();
        }
        textSelectionOwner.reset();
        pendingStylusTextTap.reset();
        smartSelectPdfGesture.reset();
        textDragHandled = false;
        input.cancel();
        state.cancelVerticalSpace();
        if (!prototype::isSelectionTool(tool) && tool != Tool::Hand) {
            state.clearSelection();
        }
        state.brush = tool == Tool::Highlighter || prototype::isPdfTextSelectionTool(tool) ? highlightBrush :
                      tool == Tool::Eraser ? eraserBrush :
                      tool == Tool::LaserPointerPen ? laserPenBrush :
                      tool == Tool::LaserPointerHighlighter ? laserHighlighterBrush :
                      tool == Tool::Latex ? latexBrush : penBrush;
        state.brush.tool = tool;
        if (tool == Tool::Text) {
            state.brush.color = state.textColor;
        }
        if (tool == Tool::Eraser) {
            state.brush.color = prototype::behavior::whiteoutColor;
            state.eraserWidth = state.brush.width;
        }
        syncingBrush = true;
        if (tool != Tool::Eraser) {
            const auto& color = state.brush.color;
            const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]),
                               static_cast<float>(color[2]), 1};
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
        }
        gtk_widget_set_visible(colorBox, tool != Tool::Eraser && tool != Tool::Hand && tool != Tool::Link &&
                                                tool != Tool::PlayObject &&
                                                tool != Tool::FloatingToolbox &&
                                                tool != Tool::VerticalSpace &&
                                                !prototype::isSelectionTool(tool));
        gtk_widget_set_visible(sizeDropdown, !prototype::isSelectionTool(tool) && tool != Tool::Text &&
                                                     tool != Tool::Latex &&
                                                     tool != Tool::Hand && tool != Tool::VerticalSpace &&
                                                     tool != Tool::Link && tool != Tool::PlayObject &&
                                                     tool != Tool::FloatingToolbox);
        gtk_widget_set_visible(widthButton, !prototype::isSelectionTool(tool) && tool != Tool::Text &&
                                                    tool != Tool::Latex &&
                                                    tool != Tool::Hand && tool != Tool::VerticalSpace &&
                                                    tool != Tool::Link && tool != Tool::PlayObject &&
                                                    tool != Tool::FloatingToolbox);
        gtk_widget_set_visible(selectionLabel, prototype::isSelectionTool(tool));
        gtk_widget_set_visible(selectionModeDropdown,
                               prototype::isSelectionTool(tool) || tool == Tool::VerticalSpace);
        gtk_widget_set_visible(selectionActions,
                               prototype::isSelectionTool(tool) && !prototype::isPdfTextSelectionTool(tool));
        gtk_widget_set_tooltip_text(selectionLabel,
                                    prototype::isPdfTextSelectionTool(tool) ?
                                            "Drag across selectable PDF text. Choose Linear or Rectangle, then copy "
                                            "or annotate the selected text." :
                                    tool == Tool::SelectObject ?
                                            "Click the nearest object on the active layer; Shift-click to add. "
                                            "Drag the selected object or its handles to transform it." :
                                            "Drag to select or move; drag a square handle to resize, or the circle "
                                            "handle to rotate.");
        const bool drawingTool = tool == Tool::Pen || tool == Tool::Highlighter;
        gtk_widget_set_visible(drawingOptionsExpander, drawingTool);
        const bool stabilizeFreehand = drawingTool &&
                (state.drawingMode == prototype::DrawingMode::Freehand ||
                 state.drawingMode == prototype::DrawingMode::ShapeRecognizer);
        gtk_widget_set_visible(averagingDropdown, stabilizeFreehand);
        gtk_widget_set_visible(preprocessorDropdown, stabilizeFreehand);
        gtk_widget_set_visible(textFontEntry, tool == Tool::Text);
        gtk_widget_set_visible(textFontSize, tool == Tool::Text);
        gtk_widget_set_visible(textFormatBox, tool == Tool::Text);
        if (prototype::isSelectionTool(tool)) {
            selectionTool = tool;
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), selectionModeIndex(tool));
        } else if (tool == Tool::VerticalSpace) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 8);
        }
        gtk_widget_set_sensitive(colorButton, tool != Tool::Eraser && tool != Tool::PlayObject);
        gtk_widget_set_sensitive(paletteButton, tool != Tool::Eraser && tool != Tool::PlayObject);
        if (paletteRailBox) {
            gtk_widget_set_sensitive(paletteRailBox, tool != Tool::Eraser && tool != Tool::PlayObject &&
                                                           tool != Tool::Hand && tool != Tool::Link);
        }
        gtk_widget_set_visible(eraserModeDropdown, tool == Tool::Eraser);
        if (tool != Tool::Link && linkInfoPopover) {
            gtk_popover_popdown(GTK_POPOVER(linkInfoPopover));
            linkInfoIndex.reset();
        }
        if (canvas) {
            gtk_widget_set_cursor_from_name(canvas, tool == Tool::Hand ? "grab" : tool == Tool::Link ? "pointer" :
                                                   tool == Tool::Latex ? "text" :
                                                   prototype::isPdfTextSelectionTool(tool) ? "text" : nullptr);
        }
        updateTextToolbarVisibility();
        gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), static_cast<guint>(state.eraserMode));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown), static_cast<guint>(state.drawingMode));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(averagingDropdown),
                                   static_cast<guint>(input.stabilizerConfig.averaging));
        gtk_drop_down_set_selected(GTK_DROP_DOWN(preprocessorDropdown),
                                   static_cast<guint>(input.stabilizerConfig.preprocessor));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(widthButton), state.brush.width);
        syncSizeDropdown();
        syncingBrush = false;
        if (canvas) {
            update();
        }
    }

    GtkWidget* createPaletteSwatch(size_t index) {
        const auto& entry = importedConfig.palette.swatches[index];
        auto* swatch = gtk_button_new();
        gtk_widget_add_css_class(swatch, "flat");
        gtk_widget_set_size_request(swatch, 42, 34);
        auto* dot = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(dot), 20);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(dot), 20);
        g_object_set_data(G_OBJECT(dot), "swatch-index", GUINT_TO_POINTER(static_cast<guint>(index)));
        gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(dot),
                +[](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    const auto swatchIndex = static_cast<size_t>(
                            GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(area), "swatch-index")));
                    if (swatchIndex >= self->importedConfig.palette.swatches.size()) {
                        return;
                    }
                    const auto& color = self->importedConfig.palette.swatches[swatchIndex].color;
                    const double radius = std::max(2.0, std::min(width, height) / 2.0 - 2.0);
                    cairo_arc(cr, width / 2.0, height / 2.0, radius, 0, 2 * G_PI);
                    cairo_set_source_rgb(cr, color[0], color[1], color[2]);
                    cairo_fill_preserve(cr);
                    cairo_set_source_rgba(cr, 0.45, 0.45, 0.45, 0.65);
                    cairo_set_line_width(cr, 1.0);
                    cairo_stroke(cr);
                },
                this, nullptr);
        gtk_button_set_child(GTK_BUTTON(swatch), dot);
        gtk_widget_set_tooltip_text(swatch, entry.name.c_str());
        gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL, entry.name.c_str(), -1);
        g_object_set_data(G_OBJECT(swatch), "swatch-index", GUINT_TO_POINTER(static_cast<guint>(index)));
        g_signal_connect(swatch, "clicked", G_CALLBACK((+[](GtkButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto swatchIndex = static_cast<size_t>(
                                     GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "swatch-index")));
                             if (swatchIndex < self->importedConfig.palette.swatches.size()) {
                                 const auto& color = self->importedConfig.palette.swatches[swatchIndex].color;
                                 const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]),
                                                    static_cast<float>(color[2]), 1.0f};
                                 gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(self->colorButton), &rgba);
                             }
                             if (self->palettePopover) {
                                 gtk_popover_popdown(GTK_POPOVER(self->palettePopover));
                             }
                             gtk_widget_grab_focus(self->textEditor && self->textEditor->active() ?
                                                           self->textEditor->widget() : self->canvas);
                         })),
                         this);
        return swatch;
    }

    GtkWidget* mainToolWidget(prototype::MainToolAction action) const {
        switch (action) {
            case prototype::MainToolAction::SmartSelect:
                return toolButtons[3];
            case prototype::MainToolAction::Pen:
                return toolButtons[0];
            case prototype::MainToolAction::Highlighter:
                return toolButtons[1];
            case prototype::MainToolAction::Eraser:
                return toolButtons[2];
            case prototype::MainToolAction::Text:
                return textButton;
            case prototype::MainToolAction::Latex:
                return latexButton;
            case prototype::MainToolAction::Hand:
                return toolButtons[4];
            case prototype::MainToolAction::Link:
                return linkButton;
            case prototype::MainToolAction::LaserPointerPen:
                return laserButtons[0];
            case prototype::MainToolAction::LaserPointerHighlighter:
                return laserButtons[1];
            case prototype::MainToolAction::PlayObject:
                return playObjectButton;
            case prototype::MainToolAction::FloatingToolbox:
                return floatingToolboxToolButton;
        }
        return nullptr;
    }

    void rebuildMainToolRail() {
        std::vector<GtkWidget*> retainedWidgets;
        const auto clearBox = [&retainedWidgets](GtkWidget* box) {
            for (auto* child = gtk_widget_get_first_child(box); child;) {
                auto* next = gtk_widget_get_next_sibling(child);
                g_object_ref(child);
                retainedWidgets.push_back(child);
                gtk_box_remove(GTK_BOX(box), child);
                child = next;
            }
        };
        clearBox(mainToolGroup);
        clearBox(moreToolsGroup);

        const auto& actions = importedConfig.settings.mainToolRail;
        for (const auto action: actions) {
            if (auto* widget = mainToolWidget(action)) {
                gtk_box_append(GTK_BOX(mainToolGroup), widget);
            }
        }
        bool hasMore = false;
        for (const auto& entry: prototype::mainToolRailNames) {
            if (std::find(actions.begin(), actions.end(), entry.first) != actions.end()) {
                continue;
            }
            if (auto* widget = mainToolWidget(entry.first)) {
                gtk_box_append(GTK_BOX(moreToolsGroup), widget);
                hasMore = true;
            }
        }
        gtk_widget_set_visible(moreToolsButton, hasMore);
        for (auto* widget: retainedWidgets) {
            g_object_unref(widget);
        }
    }

    GtkWidget* createMainToolRail() {
        auto* contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        mainToolGroup = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(mainToolGroup, "linked");
        moreToolsButton = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(moreToolsButton), "view-more-symbolic");
        gtk_widget_set_tooltip_text(moreToolsButton, "More tools");
        auto* moreToolsPopover = gtk_popover_new();
        moreToolsGroup = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(moreToolsGroup, "linked");
        gtk_widget_set_margin_top(moreToolsGroup, 6);
        gtk_widget_set_margin_bottom(moreToolsGroup, 6);
        gtk_widget_set_margin_start(moreToolsGroup, 6);
        gtk_widget_set_margin_end(moreToolsGroup, 6);
        gtk_popover_set_child(GTK_POPOVER(moreToolsPopover), moreToolsGroup);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(moreToolsButton), moreToolsPopover);
        gtk_box_append(GTK_BOX(contents), mainToolGroup);
        gtk_box_append(GTK_BOX(contents), moreToolsButton);
        gtk_box_append(GTK_BOX(contents), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

        paletteRailBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        paletteSwatchButtons.clear();
        for (size_t index = 0; index < importedConfig.palette.swatches.size(); ++index) {
            auto* swatch = createPaletteSwatch(index);
            paletteSwatchButtons.push_back(swatch);
            gtk_box_append(GTK_BOX(paletteRailBox), swatch);
        }
        gtk_box_append(GTK_BOX(contents), paletteRailBox);
        rebuildMainToolRail();

        auto* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), contents);
        gtk_widget_set_size_request(scroll, 58, -1);
        gtk_widget_set_vexpand(scroll, true);
        gtk_widget_add_css_class(scroll, "sidebar");
        return scroll;
    }

    GtkWidget* createPalette() {
        paletteButton = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(paletteButton), "view-more-symbolic");
        const auto buttonTip = "Quick colors (" + importedConfig.palette.paletteName + ")";
        gtk_widget_set_tooltip_text(paletteButton, buttonTip.c_str());
        palettePopover = gtk_popover_new();

        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        paletteContent = box;
        gtk_widget_set_margin_top(box, 6);
        gtk_widget_set_margin_bottom(box, 6);
        gtk_widget_set_margin_start(box, 6);
        gtk_widget_set_margin_end(box, 6);

        auto* title = gtk_label_new(importedConfig.palette.paletteName.c_str());
        gtk_widget_add_css_class(title, "heading");
        gtk_widget_set_halign(title, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), title);

        auto* grid = gtk_grid_new();
        gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
        gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
        gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);

        const int columns = std::clamp(importedConfig.palette.columns, 4, 8);
        for (size_t i = 0; i < importedConfig.palette.swatches.size(); ++i) {
            auto* swatch = createPaletteSwatch(i);
            gtk_grid_attach(GTK_GRID(grid), swatch, static_cast<int>(i) % columns, static_cast<int>(i) / columns, 1, 1);
        }
        gtk_box_append(GTK_BOX(box), grid);
        gtk_popover_set_child(GTK_POPOVER(palettePopover), box);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(paletteButton), palettePopover);
        return paletteButton;
    }

    GtkWidget* createSidebar() {
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_add_css_class(box, "background");
        auto* title = gtk_label_new("Pages");
        gtk_widget_add_css_class(title, "heading");
        gtk_widget_set_margin_top(title, 12);
        gtk_box_append(GTK_BOX(box), title);
        auto* scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scroll, true);
        pageList = GTK_LIST_BOX(gtk_list_box_new());
        gtk_widget_add_css_class(GTK_WIDGET(pageList), "navigation-sidebar");
        for (int i = 0; i < pageCount; ++i) {
            auto* row = gtk_list_box_row_new();
            auto* contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
            auto* preview = gtk_drawing_area_new();
            gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(preview), 112);
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(preview), 150);
            g_object_set_data(G_OBJECT(preview), "page", GINT_TO_POINTER(i));
            gtk_drawing_area_set_draw_func(
                    GTK_DRAWING_AREA(preview),
                    +[](GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data) {
                        static_cast<Prototype*>(data)->drawThumbnail(
                                cr, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "page")), width, height);
                    },
                    this, nullptr);
            thumbnailWidgets.push_back(preview);
            gtk_box_append(GTK_BOX(contents), preview);
            gtk_box_append(GTK_BOX(contents), gtk_label_new(("Page " + std::to_string(i + 1)).c_str()));
            gtk_widget_set_margin_top(contents, 8);
            gtk_widget_set_margin_bottom(contents, 8);
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), contents);
            gtk_list_box_append(pageList, row);
        }
        g_signal_connect(pageList, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingPage && row && gtk_list_box_row_get_index(row) != self->state.page) {
                                 self->navigateToPage(gtk_list_box_row_get_index(row));
                                 if (adw_overlay_split_view_get_collapsed(self->splitView)) {
                                     adw_overlay_split_view_set_show_sidebar(self->splitView, false);
                                 }
                                 gtk_widget_grab_focus(self->canvas);
                             }
                         }),
                         this);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(pageList));
        gtk_box_append(GTK_BOX(box), scroll);
        auto* layerPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(layerPanel, 8);
        gtk_widget_set_margin_end(layerPanel, 8);
        auto* layerHeader = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        auto* layerTitle = gtk_label_new("Layers");
        gtk_widget_add_css_class(layerTitle, "heading");
        gtk_widget_set_hexpand(layerTitle, true);
        gtk_widget_set_halign(layerTitle, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(layerHeader), layerTitle);
        auto* layerActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        gtk_widget_add_css_class(layerActions, "linked");
        layerAddButton = button("list-add-symbolic", "Add layer above current",
                                G_CALLBACK(+[](GtkButton*, gpointer data) {
                                    auto* self = static_cast<Prototype*>(data);
                                    self->finishTextEditing();
                                    self->resetInput(true);
                                    if (self->state.addLayerAboveCurrent()) {
                                        self->selectedImageIndex.reset();
                                        self->rebuildLayerStack();
                                        self->update();
                                    }
                                }));
        layerDeleteButton = button("list-remove-symbolic", "Delete current layer",
                                   G_CALLBACK(+[](GtkButton*, gpointer data) {
                                       auto* self = static_cast<Prototype*>(data);
                                       self->finishTextEditing();
                                       self->resetInput(true);
                                       if (self->state.deleteCurrentLayer()) {
                                           self->selectedImageIndex.reset();
                                           self->rebuildLayerStack();
                                           self->update();
                                       }
                                   }));
        layerMergeButton = gtk_button_new_with_label("Merge");
        gtk_widget_set_tooltip_text(layerMergeButton, "Merge current layer down");
        gtk_widget_add_css_class(layerMergeButton, "flat");
        g_signal_connect(layerMergeButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->finishTextEditing();
                             self->resetInput(true);
                             if (self->state.mergeCurrentLayerDown()) {
                                 self->selectedImageIndex.reset();
                                 self->rebuildLayerStack();
                                 self->update();
                             }
                         }), this);
        layerMenuButton = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(layerMenuButton), "view-more-symbolic");
        gtk_widget_set_tooltip_text(layerMenuButton, "More layer actions");
        layerPopover = gtk_popover_new();
        auto* layerMenu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_top(layerMenu, 6);
        gtk_widget_set_margin_bottom(layerMenu, 6);
        gtk_widget_set_margin_start(layerMenu, 6);
        gtk_widget_set_margin_end(layerMenu, 6);
        const auto appendLayerAction = [this, layerMenu](const char* title, GCallback callback) {
            auto* action = gtk_button_new_with_label(title);
            gtk_widget_set_halign(action, GTK_ALIGN_FILL);
            gtk_widget_add_css_class(action, "flat");
            g_signal_connect(action, "clicked", callback, this);
            gtk_box_append(GTK_BOX(layerMenu), action);
            return action;
        };
        layerRenameButton = appendLayerAction(
                "Rename Layer…", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                    const auto& pageLayers = self->state.layersForPage(self->state.page);
                    if (pageLayers.selectedLayer > 0) {
                        self->openLayerRenameDialog(self->state.page, pageLayers.selectedLayer);
                    }
                }));
        layerRenameBackgroundButton = appendLayerAction(
                "Rename Background…", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                    self->openLayerRenameDialog(self->state.page, 0);
                }));
        layerDuplicateButton = appendLayerAction(
                "Duplicate Layer", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                    self->finishTextEditing();
                    self->resetInput(true);
                    if (self->state.duplicateCurrentLayer()) {
                        self->refreshAfterLayerMutation();
                    }
                }));
        auto* separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_margin_top(separator, 4);
        gtk_widget_set_margin_bottom(separator, 4);
        gtk_box_append(GTK_BOX(layerMenu), separator);
        layerMoveUpButton = appendLayerAction(
                "Move Layer Up", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                    self->finishTextEditing();
                    self->resetInput(true);
                    if (self->state.moveCurrentLayer(true)) {
                        self->refreshAfterLayerMutation();
                    }
                }));
        layerMoveDownButton = appendLayerAction(
                "Move Layer Down", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                    self->finishTextEditing();
                    self->resetInput(true);
                    if (self->state.moveCurrentLayer(false)) {
                        self->refreshAfterLayerMutation();
                    }
                }));
        auto* visibilitySeparator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_margin_top(visibilitySeparator, 4);
        gtk_widget_set_margin_bottom(visibilitySeparator, 4);
        gtk_box_append(GTK_BOX(layerMenu), visibilitySeparator);
        layerShowAllButton = appendLayerAction(
                "Show All Layers", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                    self->finishTextEditing();
                    self->resetInput(true);
                    if (self->state.setAllLayersVisible(self->state.page, true)) {
                        self->refreshAfterLayerMutation();
                    }
                }));
        layerHideAllButton = appendLayerAction(
                "Hide All Layers", G_CALLBACK(+[](GtkButton*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    gtk_popover_popdown(GTK_POPOVER(self->layerPopover));
                    self->finishTextEditing();
                    self->resetInput(true);
                    if (self->state.setAllLayersVisible(self->state.page, false)) {
                        self->refreshAfterLayerMutation();
                    }
                }));
        gtk_popover_set_child(GTK_POPOVER(layerPopover), layerMenu);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(layerMenuButton), layerPopover);
        gtk_box_append(GTK_BOX(layerActions), layerAddButton);
        gtk_box_append(GTK_BOX(layerActions), layerDeleteButton);
        gtk_box_append(GTK_BOX(layerActions), layerMergeButton);
        gtk_box_append(GTK_BOX(layerActions), layerMenuButton);
        gtk_box_append(GTK_BOX(layerHeader), layerActions);
        gtk_box_append(GTK_BOX(layerPanel), layerHeader);
        layerRows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_size_request(layerRows, -1, 112);
        gtk_box_append(GTK_BOX(layerPanel), layerRows);
        gtk_box_append(GTK_BOX(box), layerPanel);
        rebuildLayerStack();
        return box;
    }

    void rebuildLayerStack() {
        if (!layerRows) {
            return;
        }
        const auto& pageLayers = state.layersForPage(state.page);
        std::vector<GtkWidget*> existingRows;
        for (auto* child = gtk_widget_get_first_child(layerRows); child;
             child = gtk_widget_get_next_sibling(child)) {
            existingRows.push_back(child);
        }
        const bool sameRows = existingRows.size() == pageLayers.layers.size() + 1 &&
                              std::all_of(existingRows.begin(), existingRows.end(), [&](GtkWidget* row) {
                                  auto* visibility = gtk_widget_get_first_child(row);
                                  auto* select = visibility ? gtk_widget_get_next_sibling(visibility) : nullptr;
                                  return visibility && select &&
                                         gtk_widget_get_next_sibling(select) == nullptr;
                              }) &&
                              [&] {
                                  for (size_t index = 0; index < existingRows.size(); ++index) {
                                      const size_t expectedId = index == 0 ? 0 : pageLayers.layers.size() - index + 1;
                                      auto* visibility = gtk_widget_get_first_child(existingRows[index]);
                                      const size_t actualId = static_cast<size_t>(
                                              GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(visibility), "layer-id")));
                                      if (actualId != expectedId) {
                                          return false;
                                      }
                                  }
                                  return true;
                              }();
        if (sameRows) {
            for (size_t index = 0; index < existingRows.size(); ++index) {
                const size_t layerId = index == 0 ? 0 : pageLayers.layers.size() - index + 1;
                const bool visible = layerId == 0 ? pageLayers.backgroundVisible :
                                                    pageLayers.layers[layerId - 1].visible;
                const bool active = layerId != 0 && layerId == pageLayers.selectedLayer;
                const auto& layer = layerId == 0 ? prototype::CanvasLayer{} : pageLayers.layers[layerId - 1];
                const auto title = layerId == 0 ? pageLayers.backgroundName.value_or("Background") :
                                   (layer.name.empty() ? "Layer " + std::to_string(layerId) : layer.name);
                auto* visibility = GTK_TOGGLE_BUTTON(gtk_widget_get_first_child(existingRows[index]));
                auto* select = GTK_BUTTON(gtk_widget_get_next_sibling(GTK_WIDGET(visibility)));
                gtk_toggle_button_set_active(visibility, visible);
                gtk_button_set_icon_name(GTK_BUTTON(visibility), visible ? "view-reveal-symbolic" :
                                                                          "view-conceal-symbolic");
                gtk_widget_set_tooltip_text(GTK_WIDGET(visibility), visible ? "Hide layer" : "Show layer");
                gtk_button_set_label(select, title.c_str());
                gtk_widget_set_tooltip_text(GTK_WIDGET(select), title.c_str());
                if (active) {
                    gtk_widget_remove_css_class(GTK_WIDGET(select), "flat");
                    gtk_widget_add_css_class(GTK_WIDGET(select), "suggested-action");
                } else {
                    gtk_widget_add_css_class(GTK_WIDGET(select), "flat");
                    gtk_widget_remove_css_class(GTK_WIDGET(select), "suggested-action");
                }
            }
            gtk_widget_set_sensitive(layerAddButton, bool(nativeSession) && pageLayers.layers.size() < 1000);
            gtk_widget_set_sensitive(layerDeleteButton, bool(nativeSession) && !pageLayers.layers.empty());
            gtk_widget_set_sensitive(layerMergeButton, bool(nativeSession) && pageLayers.selectedLayer > 1);
            updateLayerActionSensitivity(pageLayers);
            layerPanelPage = state.page;
            layerPanelSnapshot = pageLayers;
            return;
        }
        for (auto* child = gtk_widget_get_first_child(layerRows); child;) {
            auto* next = gtk_widget_get_next_sibling(child);
            gtk_box_remove(GTK_BOX(layerRows), child);
            child = next;
        }
        const auto appendVisibilityRow = [this](size_t layerId, const std::string& title, bool visible,
                                                bool active) {
            auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
            auto* visibility = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
            gtk_button_set_icon_name(GTK_BUTTON(visibility), visible ? "view-reveal-symbolic" :
                                                                       "view-conceal-symbolic");
            gtk_toggle_button_set_active(visibility, visible);
            gtk_widget_add_css_class(GTK_WIDGET(visibility), "flat");
            gtk_widget_set_tooltip_text(GTK_WIDGET(visibility), visible ? "Hide layer" : "Show layer");
            g_object_set_data(G_OBJECT(visibility), "layer-id", GUINT_TO_POINTER(static_cast<guint>(layerId)));
            g_signal_connect(visibility, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const auto id = static_cast<size_t>(GPOINTER_TO_UINT(
                                         g_object_get_data(G_OBJECT(button), "layer-id")));
                                 const bool shown = gtk_toggle_button_get_active(button);
                                 self->finishTextEditing();
                                 self->resetInput(true);
                                 gtk_button_set_icon_name(GTK_BUTTON(button), shown ? "view-reveal-symbolic" :
                                                                                     "view-conceal-symbolic");
                                 gtk_widget_set_tooltip_text(GTK_WIDGET(button), shown ? "Hide layer" : "Show layer");
                                 if (self->state.setLayerVisible(self->state.page, id, shown)) {
                                     if (self->selectedImageIndex &&
                                         *self->selectedImageIndex < self->state.images.size() &&
                                         self->state.images[*self->selectedImageIndex].page == self->state.page &&
                                         self->state.images[*self->selectedImageIndex].layerId == id) {
                                         self->selectedImageIndex.reset();
                                     }
                                     self->thumbnails.clear();
                                     self->update();
                                 }
                             }), this);
            auto* select = gtk_button_new_with_label(title.c_str());
            gtk_widget_set_hexpand(select, true);
            gtk_widget_set_halign(select, GTK_ALIGN_FILL);
            gtk_widget_add_css_class(select, "flat");
            if (active) {
                gtk_widget_remove_css_class(select, "flat");
                gtk_widget_add_css_class(select, "suggested-action");
            }
            gtk_widget_set_tooltip_text(select, title.c_str());
            g_object_set_data(G_OBJECT(select), "layer-id", GUINT_TO_POINTER(static_cast<guint>(layerId)));
            g_signal_connect(select, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const auto id = static_cast<size_t>(GPOINTER_TO_UINT(
                                         g_object_get_data(G_OBJECT(button), "layer-id")));
                                 self->finishTextEditing();
                                 self->resetInput(true);
                                 if (self->state.selectLayer(self->state.page, id)) {
                                     self->selectedImageIndex.reset();
                                     self->rebuildLayerStack();
                                     self->update();
                                 }
                             }), this);
            gtk_box_append(GTK_BOX(row), GTK_WIDGET(visibility));
            gtk_box_append(GTK_BOX(row), select);
            gtk_box_append(GTK_BOX(layerRows), row);
        };

        appendVisibilityRow(0, pageLayers.backgroundName.value_or("Background"), pageLayers.backgroundVisible, false);
        for (size_t layerId = pageLayers.layers.size(); layerId > 0; --layerId) {
            const auto& layer = pageLayers.layers[layerId - 1];
            const auto title = layer.name.empty() ? "Layer " + std::to_string(layerId) : layer.name;
            appendVisibilityRow(layerId, title, layer.visible, layerId == pageLayers.selectedLayer);
        }
        gtk_widget_set_sensitive(layerAddButton, bool(nativeSession) && pageLayers.layers.size() < 1000);
        gtk_widget_set_sensitive(layerDeleteButton, bool(nativeSession) && !pageLayers.layers.empty());
        gtk_widget_set_sensitive(layerMergeButton, bool(nativeSession) && pageLayers.selectedLayer > 1);
        updateLayerActionSensitivity(pageLayers);
        layerPanelPage = state.page;
        layerPanelSnapshot = pageLayers;
    }

    GtkWidget* findLayerControl(size_t layerId, bool visibility) const {
        for (auto* row = gtk_widget_get_first_child(layerRows); row; row = gtk_widget_get_next_sibling(row)) {
            for (auto* child = gtk_widget_get_first_child(row); child; child = gtk_widget_get_next_sibling(child)) {
                const auto id = static_cast<size_t>(
                        GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(child), "layer-id")));
                if (id == layerId && GTK_IS_TOGGLE_BUTTON(child) == visibility) {
                    return child;
                }
            }
        }
        return nullptr;
    }

    void updateLayerActionSensitivity(const prototype::PageLayers& pageLayers) {
        const bool hasSelection = pageLayers.selectedLayer > 0 && pageLayers.selectedLayer <= pageLayers.layers.size();
        gtk_widget_set_sensitive(layerRenameButton, bool(nativeSession) && hasSelection);
        gtk_widget_set_sensitive(layerRenameBackgroundButton, bool(nativeSession));
        gtk_widget_set_sensitive(layerDuplicateButton, bool(nativeSession) && hasSelection &&
                                                               pageLayers.layers.size() < 1000);
        gtk_widget_set_sensitive(layerMoveUpButton, bool(nativeSession) && hasSelection &&
                                                           pageLayers.selectedLayer < pageLayers.layers.size());
        gtk_widget_set_sensitive(layerMoveDownButton, bool(nativeSession) && hasSelection &&
                                                             pageLayers.selectedLayer > 1);
        gtk_widget_set_sensitive(layerShowAllButton, bool(nativeSession) && !pageLayers.layers.empty());
        gtk_widget_set_sensitive(layerHideAllButton, bool(nativeSession) && !pageLayers.layers.empty());
    }

    void refreshAfterLayerMutation() {
        selectedImageIndex.reset();
        thumbnails.clear();
        rebuildLayerStack();
        update();
    }

    void drawThumbnail(cairo_t* cr, int number, int width, int height) {
        if (!thumbnails.contains(number)) {
            if (thumbnails.size() == 16) {
                thumbnails.erase(thumbnails.begin());
            }
            auto surface = std::unique_ptr<cairo_surface_t, SurfaceDeleter>(
                    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 112, 150));
            auto* context = cairo_create(surface.get());
            const auto sz = state.pageSize(number);
            double scale = std::min(112.0 / sz.width, 150.0 / sz.height);
            cairo_set_source_rgb(context, 1, 1, 1);
            cairo_paint(context);
            if (document && state.layersForPage(number).backgroundVisible) {
                auto* source = poppler_document_get_page(document, number);
                double w, h;
                poppler_page_get_size(source, &w, &h);
                scale = std::min(112.0 / w, 150.0 / h);
                cairo_scale(context, scale, scale);
                poppler_page_render(source, context);
                g_object_unref(source);
            }
            cairo_destroy(context);
            thumbnails.emplace(number, std::make_pair(std::move(surface), scale));
        }
        cairo_save(cr);
        cairo_translate(cr, (width - 112) / 2.0, (height - 150) / 2.0);
        cairo_rectangle(cr, 0, 0, 112, 150);
        cairo_clip(cr);
        cairo_set_source_surface(cr, thumbnails.at(number).first.get(), 0, 0);
        cairo_paint(cr);
        cairo_scale(cr, thumbnails.at(number).second, thumbnails.at(number).second);
        enum class EditableKind { Ink, Text, Image };
        struct LayerItem {
            size_t order;
            EditableKind kind;
            size_t index;
        };
        const auto& pageLayers = state.layersForPage(number);
        for (size_t layerIndex = 0; layerIndex < pageLayers.layers.size(); ++layerIndex) {
            const size_t layerId = layerIndex + 1;
            const auto& layer = pageLayers.layers[layerIndex];
            if (!layer.visible) {
                continue;
            }
            for (size_t origin: layer.origins) {
                if (origin < layerDocuments.size()) {
                    if (auto* nativePage = poppler_document_get_page(layerDocuments[origin], number)) {
                        poppler_page_render(nativePage, cr);
                        g_object_unref(nativePage);
                    }
                }
                std::vector<LayerItem> items;
                items.reserve(state.ink.size() + state.texts.size());
                for (size_t index = 0; index < state.ink.size(); ++index) {
                    if (state.displayPage(index) == number && state.displayLayerId(index) == layerId &&
                        state.displayLayerOrigin(index) == origin) {
                        items.push_back({state.ink[index].layerOrder, EditableKind::Ink, index});
                    }
                }
                for (size_t index = 0; index < state.texts.size(); ++index) {
                    if (state.texts[index].page == number && state.texts[index].layerId == layerId &&
                        state.texts[index].originLayer == origin) {
                        items.push_back({state.texts[index].layerOrder, EditableKind::Text, index});
                    }
                }
                for (size_t index = 0; index < state.images.size(); ++index) {
                    const auto& image = state.images[index];
                    if (image.page == number && image.layerId == layerId && image.originLayer == origin) {
                        items.push_back({image.layerOrder, EditableKind::Image, index});
                    }
                }
                std::stable_sort(items.begin(), items.end(),
                                 [](const LayerItem& a, const LayerItem& b) { return a.order < b.order; });
                for (const auto& item: items) {
                    if (item.kind == EditableKind::Text) {
                        prototype::drawText(cr, state.texts[item.index]);
                    } else if (item.kind == EditableKind::Image) {
                        prototype::drawCanvasImage(cr, displayedImage(item.index));
                    } else {
                        prototype::drawInkPreview(cr, state, item.index);
                    }
                }
                if (state.current && state.current->page == number && state.current->layerId == layerId &&
                    state.current->originLayer == origin) {
                    prototype::drawInk(cr, *state.current);
                }
            }
        }
        cairo_restore(cr);
    }

    void activate(GtkApplication* app) {
        AdwColorScheme colorScheme = ADW_COLOR_SCHEME_DEFAULT;
        if (options.explicitDark) {
            colorScheme = ADW_COLOR_SCHEME_FORCE_DARK;
        } else {
            switch (importedConfig.settings.themeVariant) {
                case prototype::ThemeVariant::ForceDark:
                    colorScheme = ADW_COLOR_SCHEME_FORCE_DARK;
                    break;
                case prototype::ThemeVariant::ForceLight:
                    colorScheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
                    break;
                case prototype::ThemeVariant::UseSystem:
                default:
                    colorScheme = ADW_COLOR_SCHEME_DEFAULT;
                    break;
            }
        }
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), colorScheme);
        window = adw_application_window_new(app);
        gtk_window_set_title(GTK_WINDOW(window), "Xournal++ · GTK 4 canvas prototype");
        gtk_window_set_default_size(GTK_WINDOW(window), options.width, options.height);
        audioPlayback = std::make_unique<prototype::AudioPlayback>(
                options.disableAudio || importedConfig.settings.disableAudio,
                [this](const std::string& message) { showAlert("Unable to play audio recording", message); },
                [this] {
                    if (audioControls) {
                        syncAudioControls();
                        update();
                    }
                });

        auto* view = adw_toolbar_view_new();
        contentView = view;
        auto* header = adw_header_bar_new();
        heading = adw_window_title_new("Blank notebook", "");
        adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), heading);
        sidebarButton = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(sidebarButton), "view-sidebar-start-symbolic");
        gtk_widget_set_tooltip_text(sidebarButton, "Show pages");
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebarButton);
        preferencesButton = button("preferences-system-symbolic", "Preferences",
                                   G_CALLBACK(+[](GtkButton*, gpointer data) {
                                       static_cast<Prototype*>(data)->openPreferences();
                                   }));
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), preferencesButton);
        auto* pluginsManagerAction = g_simple_action_new("plugins-manager", nullptr);
        g_signal_connect(pluginsManagerAction, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
                             static_cast<Prototype*>(data)->openPluginManager();
                         }),
                         this);
        g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(pluginsManagerAction));
        g_object_unref(pluginsManagerAction);

        auto* pluginsMenu = g_menu_new();
        auto* pluginSections = g_menu_new();
        const auto pluginActions =
                pluginRuntime ? pluginRuntime->appendMenuSections(pluginSections, G_ACTION_MAP(window), app) : 0;
        if (pluginActions > 0) {
            g_menu_append_section(pluginsMenu, nullptr, G_MENU_MODEL(pluginSections));
        }
        auto* managerSection = g_menu_new();
        g_menu_append(managerSection, "Manage Plugins…", "win.plugins-manager");
        g_menu_append_section(pluginsMenu, nullptr, G_MENU_MODEL(managerSection));
        pluginsMenuButton = gtk_menu_button_new();
        gtk_menu_button_set_label(GTK_MENU_BUTTON(pluginsMenuButton), "Plugins");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(pluginsMenuButton), G_MENU_MODEL(pluginsMenu));
        gtk_widget_set_tooltip_text(pluginsMenuButton, "Plugin actions and plugin manager");
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), pluginsMenuButton);
        g_object_unref(managerSection);
        g_object_unref(pluginSections);
        g_object_unref(pluginsMenu);
        openButton = button("document-open-symbolic", "Open (Ctrl+O)", G_CALLBACK(+[](GtkButton*, gpointer data) {
            auto* self = static_cast<Prototype*>(data);
            self->protectDocument([self] { self->chooseOpen(); });
        }));
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), openButton);
        saveButton = button("document-save-symbolic", "Save (Ctrl+S)",
                            G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->save(); }));
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), saveButton);
        saveAsButton = button("document-save-as-symbolic", "Save As (Ctrl+Shift+S)",
                              G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->chooseSave(); }));
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), saveAsButton);
        exportPdfButton = button("document-send-symbolic", "Export as PDF (Ctrl+E)",
                                 G_CALLBACK(+[](GtkButton*, gpointer data) {
                                     static_cast<Prototype*>(data)->chooseExportPdf();
                                 }));
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), exportPdfButton);
        undoButton = button("edit-undo-symbolic", "Undo ink (Ctrl+Z)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                auto* self = static_cast<Prototype*>(data);
                                self->selectedImageIndex.reset();
                                self->input.cancel();
                                self->state.undo();
                                self->update();
                            }));
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), undoButton);
        redoButton = button("edit-redo-symbolic", "Redo ink (Ctrl+Shift+Z)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                auto* self = static_cast<Prototype*>(data);
                                self->selectedImageIndex.reset();
                                self->input.cancel();
                                self->state.redo();
                                self->update();
                            }));
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), redoButton);
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), button("zoom-fit-best-symbolic", "Fit page (Ctrl+0)",
                                                               G_CALLBACK(+[](GtkButton*, gpointer data) {
                                                                   auto* self = static_cast<Prototype*>(data);
                                                                   self->fitted = false;
                                                                   self->update();
                                                               })));
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

        auto* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(toolbar, "toolbar");
        gtk_widget_set_halign(toolbar, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(toolbar, true);
        const std::array<const char*, 5> toolNames = {
                "Pen", "Highlighter", "Eraser", "Smart Select", "Hand tool",
        };
        const std::array<const char*, 5> toolIcons = {
                "document-edit-symbolic", "applications-graphics-symbolic", "edit-clear-symbolic",
                "edit-select-all-symbolic", "pan-down-symbolic",
        };
        for (size_t i = 0; i < toolButtons.size(); ++i) {
            toolButtons[i] = gtk_toggle_button_new();
            gtk_button_set_child(GTK_BUTTON(toolButtons[i]), gtk_image_new_from_icon_name(toolIcons[i]));
            gtk_accessible_update_property(GTK_ACCESSIBLE(toolButtons[i]), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                           toolNames[i], -1);
            gtk_widget_set_tooltip_text(toolButtons[i], toolNames[i]);
            if (i) {
                gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(toolButtons[i]), GTK_TOGGLE_BUTTON(toolButtons[0]));
            }
            g_object_set_data(G_OBJECT(toolButtons[i]), "tool", GUINT_TO_POINTER(i));
            g_signal_connect(toolButtons[i], "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 if (gtk_toggle_button_get_active(button) && !self->syncingTools) {
                                     const auto index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "tool"));
                                     const auto tool = index == 3 ? Tool::SmartSelect :
                                                       index == 4 ? Tool::Hand : static_cast<Tool>(index);
                                     self->selectTool(tool);
                                 }
                             }),
                             this);
        }
        gtk_widget_set_tooltip_text(toolButtons[3],
                                    "Smart Select (S); open Selection mode for rectangle, lasso, object, and PDF text");
        gtk_widget_set_tooltip_text(toolButtons[4], "Drag anywhere on the canvas to pan; keep selections active");
        floatingToolboxToolButton = gtk_toggle_button_new();
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(floatingToolboxToolButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
        gtk_button_set_child(GTK_BUTTON(floatingToolboxToolButton), gtk_image_new_from_icon_name("view-grid-symbolic"));
        gtk_widget_set_tooltip_text(floatingToolboxToolButton,
                                    "Floating toolbox: click the canvas to open tools at the pointer");
        gtk_accessible_update_property(GTK_ACCESSIBLE(floatingToolboxToolButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Floating toolbox tool", -1);
        g_signal_connect(floatingToolboxToolButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (gtk_toggle_button_get_active(button) && !self->syncingTools) {
                                 self->selectTool(Tool::FloatingToolbox);
                             }
                         }),
                         this);
        textButton = gtk_toggle_button_new();
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(textButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
        gtk_button_set_child(GTK_BUTTON(textButton), gtk_image_new_from_icon_name("format-text-symbolic"));
        gtk_widget_set_tooltip_text(textButton, "Text tool (T)");
        gtk_accessible_update_property(GTK_ACCESSIBLE(textButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Text tool", -1);
        g_signal_connect(textButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingTools &&
                                 (gtk_toggle_button_get_active(button) || self->state.brush.tool == Tool::Text)) {
                                 self->selectTool(Tool::Text);
                             }
                         }),
                         this);
        latexButton = gtk_toggle_button_new();
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(latexButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
        gtk_button_set_child(GTK_BUTTON(latexButton), gtk_label_new("x²"));
        gtk_widget_set_tooltip_text(latexButton, "LaTeX formula tool");
        gtk_accessible_update_property(GTK_ACCESSIBLE(latexButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "LaTeX formula tool", -1);
        g_signal_connect(latexButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingTools &&
                                 (gtk_toggle_button_get_active(button) || self->state.brush.tool == Tool::Latex)) {
                                 self->selectTool(Tool::Latex);
                             }
                         }),
                         this);
        linkButton = gtk_toggle_button_new();
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(linkButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
        gtk_button_set_child(GTK_BUTTON(linkButton), gtk_image_new_from_icon_name("insert-link-symbolic"));
        gtk_widget_set_tooltip_text(linkButton, "Add or edit a link (L)");
        gtk_accessible_update_property(GTK_ACCESSIBLE(linkButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Link tool", -1);
        g_signal_connect(linkButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingTools &&
                                 (gtk_toggle_button_get_active(button) || self->state.brush.tool == Tool::Link)) {
                                 self->selectTool(Tool::Link);
                             }
                         }), this);
        for (size_t i = 0; i < laserButtons.size(); ++i) {
            laserButtons[i] = gtk_toggle_button_new();
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(laserButtons[i]), GTK_TOGGLE_BUTTON(toolButtons[0]));
            const char* icon = i == 0 ? "media-record-symbolic" : "applications-graphics-symbolic";
            const char* label = i == 0 ? "Laser Pointer Pen" : "Laser Pointer Highlighter";
            gtk_button_set_child(GTK_BUTTON(laserButtons[i]), gtk_image_new_from_icon_name(icon));
            gtk_widget_set_tooltip_text(laserButtons[i], label);
            gtk_accessible_update_property(GTK_ACCESSIBLE(laserButtons[i]), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
            g_object_set_data(G_OBJECT(laserButtons[i]), "laser-tool-index", GUINT_TO_POINTER(i));
            g_signal_connect(laserButtons[i], "toggled",
                             G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 if (!gtk_toggle_button_get_active(button) || self->syncingTools) {
                                     return;
                                 }
                                 const auto index = GPOINTER_TO_UINT(
                                         g_object_get_data(G_OBJECT(button), "laser-tool-index"));
                                 self->selectTool(index == 0 ? Tool::LaserPointerPen :
                                                                  Tool::LaserPointerHighlighter);
                             }), this);
        }
        playObjectButton = gtk_toggle_button_new();
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(playObjectButton), GTK_TOGGLE_BUTTON(toolButtons[0]));
        gtk_button_set_child(GTK_BUTTON(playObjectButton), gtk_image_new_from_icon_name("audio-x-generic-symbolic"));
        gtk_widget_set_tooltip_text(playObjectButton, "Play Object: click an audio-linked stroke or text object");
        gtk_accessible_update_property(GTK_ACCESSIBLE(playObjectButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Play Object tool", -1);
        g_signal_connect(playObjectButton, "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (gtk_toggle_button_get_active(button) && !self->syncingTools) {
                                 self->selectTool(Tool::PlayObject);
                             }
                         }), this);
        imageButton = button("insert-image-symbolic", "Insert Image",
                             G_CALLBACK(+[](GtkButton*, gpointer data) {
                                 static_cast<Prototype*>(data)->chooseImage();
                             }));
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), imageButton);

        audioControls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(audioControls, "linked");
        audioBackButton = button("media-seek-backward-symbolic", "Seek backward",
                                 G_CALLBACK(+[](GtkButton*, gpointer data) {
                                     auto* self = static_cast<Prototype*>(data);
                                     if (self->audioPlayback) {
                                         const auto seconds = std::min(self->importedConfig.settings.defaultSeekTimeSeconds,
                                                                       86400u);
                                         self->audioPlayback->seekBySeconds(-static_cast<int>(seconds));
                                     }
                                 }));
        audioPauseButton = button("media-playback-pause-symbolic", "Pause audio playback",
                                  G_CALLBACK(+[](GtkButton*, gpointer data) {
                                      auto* self = static_cast<Prototype*>(data);
                                      if (!self->audioPlayback) {
                                          return;
                                      }
                                      if (self->audioPlayback->isPaused()) {
                                          self->audioPlayback->resume();
                                      } else {
                                          self->audioPlayback->pause();
                                      }
                                  }));
        audioForwardButton = button("media-seek-forward-symbolic", "Seek forward",
                                    G_CALLBACK(+[](GtkButton*, gpointer data) {
                                        auto* self = static_cast<Prototype*>(data);
                                        if (self->audioPlayback) {
                                            const auto seconds = std::min(
                                                    self->importedConfig.settings.defaultSeekTimeSeconds, 86400u);
                                            self->audioPlayback->seekBySeconds(static_cast<int>(seconds));
                                        }
                                    }));
        audioStopButton = button("media-playback-stop-symbolic", "Stop audio playback",
                                 G_CALLBACK(+[](GtkButton*, gpointer data) {
                                     auto* self = static_cast<Prototype*>(data);
                                     if (self->audioPlayback) {
                                         self->audioPlayback->stop();
                                     }
                                 }));
        for (auto* control: {audioBackButton, audioPauseButton, audioForwardButton, audioStopButton}) {
            gtk_box_append(GTK_BOX(audioControls), control);
        }
        gtk_box_append(GTK_BOX(toolbar), audioControls);
        gtk_widget_set_visible(audioControls, false);

        textFontEntry = gtk_entry_new();
        gtk_entry_set_placeholder_text(GTK_ENTRY(textFontEntry), "Font");
        gtk_editable_set_width_chars(GTK_EDITABLE(textFontEntry), 10);
        gtk_editable_set_text(GTK_EDITABLE(textFontEntry), state.textFontName.c_str());
        gtk_widget_set_tooltip_text(textFontEntry, "Text font family");
        g_signal_connect(textFontEntry, "changed", G_CALLBACK(+[](GtkEditable* entry, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const char* font = gtk_editable_get_text(entry);
                             if (font && *font) {
                                 self->state.textFontName = font;
                                 if (self->textEditor && self->textEditor->active()) {
                                     const auto* item = self->textEditor->item();
                                     self->textEditor->setBaseFont(self->state.textFontName, item->fontSize);
                                     self->updateTextEditorPosition();
                                 } else if (self->state.brush.tool == Tool::Text &&
                                            !self->state.selectedTexts.empty()) {
                                     if (self->state.updateSelectedTexts([&](prototype::TextItem& item) {
                                             item.fontName = self->state.textFontName;
                                             const auto [width, height] = prototype::textNaturalSize(item);
                                             item.layoutWidth = width;
                                             item.layoutHeight = height;
                                         })) {
                                         self->thumbnails.clear();
                                         self->update();
                                     }
                                 }
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(toolbar), textFontEntry);
        textFontSize = gtk_spin_button_new_with_range(1, 512, 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(textFontSize), 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), state.textFontSize);
        gtk_widget_set_tooltip_text(textFontSize, "Text font size");
        g_signal_connect(textFontSize, "value-changed", G_CALLBACK(+[](GtkSpinButton* spin, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->state.textFontSize = gtk_spin_button_get_value(spin);
                             if (self->textEditor && self->textEditor->active()) {
                                 const auto* item = self->textEditor->item();
                                 self->textEditor->setBaseFont(item->fontName, self->state.textFontSize);
                                 self->updateTextEditorPosition();
                             } else if (self->state.brush.tool == Tool::Text &&
                                        !self->state.selectedTexts.empty()) {
                                 if (self->state.updateSelectedTexts([&](prototype::TextItem& item) {
                                         item.fontSize = self->state.textFontSize;
                                         const auto [width, height] = prototype::textNaturalSize(item);
                                         item.layoutWidth = width;
                                         item.layoutHeight = height;
                                     })) {
                                     self->thumbnails.clear();
                                     self->update();
                                 }
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(toolbar), textFontSize);
        textFormatBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
        for (size_t i = 0; i < textFormatButtons.size(); ++i) {
            static constexpr const char* labels[] = {"B", "I", "U", "S", "x²", "x₂"};
            static constexpr const char* tips[] = {"Bold (Ctrl+B)", "Italic (Ctrl+I)", "Underline (Ctrl+U)",
                                                    "Strikethrough (Ctrl+Shift+K)", "Superscript (Ctrl+.)",
                                                    "Subscript (Ctrl+,)"};
            textFormatButtons[i] = gtk_button_new_with_label(labels[i]);
            gtk_widget_set_tooltip_text(textFormatButtons[i], tips[i]);
            g_object_set_data(G_OBJECT(textFormatButtons[i]), "text-format", GUINT_TO_POINTER(i));
            g_signal_connect(textFormatButtons[i], "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const auto index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "text-format"));
                                 self->applyTextFormat(static_cast<prototype::TextEditorOverlay::Format>(index));
                             }),
                             this);
            gtk_box_append(GTK_BOX(textFormatBox), textFormatButtons[i]);
        }
        gtk_box_append(GTK_BOX(toolbar), textFormatBox);
        gtk_widget_set_visible(textFontEntry, false);
        gtk_widget_set_visible(textFontSize, false);
        gtk_widget_set_visible(textFormatBox, false);
        colorBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        auto* colorDialog = gtk_color_dialog_new();
        gtk_color_dialog_set_with_alpha(colorDialog, false);
        // The button constructor takes ownership of the dialog.
        colorButton = gtk_color_dialog_button_new(colorDialog);
        gtk_widget_set_tooltip_text(colorButton, "Ink color");
        g_signal_connect(colorButton, "notify::rgba", G_CALLBACK((+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush && self->state.brush.tool != Tool::Eraser) {
                                 const auto* color =
                                         gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(self->colorButton));
                                 const bool textContext = self->state.brush.tool == Tool::Text ||
                                         (self->textEditor && self->textEditor->active());
                                 const std::array selectedColor{static_cast<double>(color->red),
                                                                static_cast<double>(color->green),
                                                                static_cast<double>(color->blue)};
                                 if (textContext) {
                                     self->state.textColor = selectedColor;
                                     if (self->state.brush.tool == Tool::Text) {
                                         self->state.brush.color = selectedColor;
                                     }
                                     self->applyTextColor(Prototype::packTextColor(selectedColor));
                                 } else {
                                     self->state.brush.color = selectedColor;
                                 }
                                 self->input.cancel();
                                 self->update();
                             }
                         })),
                         this);
        gtk_box_append(GTK_BOX(colorBox), colorButton);
        gtk_box_append(GTK_BOX(colorBox), createPalette());
        gtk_box_append(GTK_BOX(toolbar), colorBox);

        const char* const eraserModes[] = {"Standard", "Whiteout", "Delete Stroke", nullptr};
        eraserModeDropdown = gtk_drop_down_new_from_strings(eraserModes);
        gtk_widget_set_tooltip_text(eraserModeDropdown, "Eraser mode");
        g_signal_connect(eraserModeDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
                                 const auto selected =
                                         gtk_drop_down_get_selected(GTK_DROP_DOWN(self->eraserModeDropdown));
                                 if (selected <= 2) {
                                     self->input.cancel();
                                     self->state.eraserMode = static_cast<prototype::EraserMode>(selected);
                                     self->update();
                                 }
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(toolbar), eraserModeDropdown);

        gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        const char* const sizeLabels[] = {"Very Fine", "Fine", "Medium", "Thick", "Very Thick", "Custom", nullptr};
        sizeDropdown = gtk_drop_down_new_from_strings(sizeLabels);
        gtk_widget_set_tooltip_text(sizeDropdown, "Tool size preset");
        g_signal_connect(sizeDropdown, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
                                 const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->sizeDropdown));
                                 if (selected < 5) {
                                     const auto width = prototype::widthsForTool(self->state.brush.tool)[selected];
                                     self->input.cancel();
                                     self->state.brush.width = width;
                                     if (self->state.brush.tool == Tool::Eraser) {
                                         self->eraserBrush.width = width;
                                         self->state.eraserWidth = width;
                                     }
                                     self->syncingBrush = true;
                                     gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->widthButton), width);
                                     self->syncingBrush = false;
                                     self->update();
                                 }
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(toolbar), sizeDropdown);
        widthButton = gtk_spin_button_new_with_range(0.2, 36.0, 0.1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widthButton), 2);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widthButton), true);
        gtk_editable_set_width_chars(GTK_EDITABLE(widthButton), 5);
        gtk_widget_set_tooltip_text(widthButton, "Stroke or eraser width in document points");
        g_signal_connect(widthButton, "value-changed", G_CALLBACK(+[](GtkSpinButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
                                 self->input.cancel();
                                 self->state.brush.width = gtk_spin_button_get_value(button);
                                 if (self->state.brush.tool == Tool::Eraser) {
                                     self->eraserBrush.width = self->state.brush.width;
                                     self->state.eraserWidth = self->state.brush.width;
                                 }
                                 self->syncingBrush = true;
                                 self->syncSizeDropdown();
                                 self->syncingBrush = false;
                                 self->update();
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(toolbar), widthButton);

        auto* drawingOptions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_top(drawingOptions, 10);
        gtk_widget_set_margin_bottom(drawingOptions, 10);
        gtk_widget_set_margin_start(drawingOptions, 10);
        gtk_widget_set_margin_end(drawingOptions, 10);

        const char* const drawingModes[] = {"Freehand", "Line", "Rectangle", "Ellipse", "Arrow",
                                            "Double arrow", "Coordinate axes", "Shape recognizer", "Spline", nullptr};
        drawingModeDropdown = gtk_drop_down_new_from_strings(drawingModes);
        gtk_widget_set_tooltip_text(drawingModeDropdown, "Drawing tool");
        g_signal_connect(drawingModeDropdown, "notify::selected",
                         G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
                                 const auto selected = gtk_drop_down_get_selected(
                                         GTK_DROP_DOWN(self->drawingModeDropdown));
                                 if (selected <= static_cast<guint>(prototype::DrawingMode::Spline)) {
                                     self->input.cancel();
                                     self->state.drawingMode = static_cast<prototype::DrawingMode>(selected);
                                     const bool supportsStabilizer =
                                             self->state.drawingMode == prototype::DrawingMode::Freehand ||
                                             self->state.drawingMode == prototype::DrawingMode::ShapeRecognizer;
                                     gtk_widget_set_visible(self->averagingDropdown,
                                                            supportsStabilizer);
                                     gtk_widget_set_visible(self->preprocessorDropdown,
                                                            supportsStabilizer);
                                     self->update();
                                 }
                             }
                         }), this);
        gtk_box_append(GTK_BOX(drawingOptions), gtk_label_new("Tool"));
        gtk_box_append(GTK_BOX(drawingOptions), drawingModeDropdown);

        const char* const averagingModes[] = {"No smoothing", "Arithmetic", "Velocity Gaussian", nullptr};
        averagingDropdown = gtk_drop_down_new_from_strings(averagingModes);
        gtk_widget_set_tooltip_text(averagingDropdown, "Stroke averaging; defaults to the Xournal++ no-stabilizer setting");
        g_signal_connect(averagingDropdown, "notify::selected",
                         G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
                                 const auto selected = gtk_drop_down_get_selected(
                                         GTK_DROP_DOWN(self->averagingDropdown));
                                 if (selected <= static_cast<guint>(prototype::AveragingMethod::VelocityGaussian)) {
                                     self->input.cancel();
                                     auto config = self->input.stabilizerConfig;
                                     config.averaging = static_cast<prototype::AveragingMethod>(selected);
                                     self->input.setStabilizerConfig(config);
                                 }
                             }
                         }), this);
        gtk_box_append(GTK_BOX(drawingOptions), gtk_label_new("Averaging"));
        gtk_box_append(GTK_BOX(drawingOptions), averagingDropdown);

        const char* const preprocessors[] = {"No preprocessor", "Deadzone", "Inertia", nullptr};
        preprocessorDropdown = gtk_drop_down_new_from_strings(preprocessors);
        gtk_widget_set_tooltip_text(preprocessorDropdown, "Stroke preprocessor");
        g_signal_connect(preprocessorDropdown, "notify::selected",
                         G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
                                 const auto selected = gtk_drop_down_get_selected(
                                         GTK_DROP_DOWN(self->preprocessorDropdown));
                                 if (selected <= static_cast<guint>(prototype::StrokePreprocessor::Inertia)) {
                                     self->input.cancel();
                                     auto config = self->input.stabilizerConfig;
                                     config.preprocessor = static_cast<prototype::StrokePreprocessor>(selected);
                                     self->input.setStabilizerConfig(config);
                                 }
                             }
                         }), this);
        gtk_box_append(GTK_BOX(drawingOptions), gtk_label_new("Preprocessor"));
        gtk_box_append(GTK_BOX(drawingOptions), preprocessorDropdown);
        drawingOptionsExpander = gtk_expander_new("Drawing tools and stabilization");
        gtk_expander_set_child(GTK_EXPANDER(drawingOptionsExpander), drawingOptions);
        gtk_box_append(GTK_BOX(paletteContent), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
        gtk_box_append(GTK_BOX(paletteContent), drawingOptionsExpander);
        gtk_widget_set_tooltip_text(paletteButton, "Quick colors and drawing tools");

        const char* selectionModes[] = {"Rectangle", "Lasso", "Smart", "Rectangle: Visible layers",
                                        "Lasso: Visible layers", "Object", "PDF text: Linear",
                                        "PDF text: Rectangle", "Vertical space", nullptr};
        selectionModeDropdown = gtk_drop_down_new_from_strings(selectionModes);
        gtk_widget_set_tooltip_text(selectionModeDropdown,
                                    "Object selects the nearest item in the active layer. Visible-layer modes "
                                    "search from the top visible layer downward and select editable objects on "
                                    "the first layer with matches. Vertical space shifts objects wholly above or "
                                    "below a dragged boundary; hold Control to switch sides.");
        g_signal_connect(
                selectionModeDropdown, "notify::selected",
                G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
                    const auto tool = selected == 8 ? Tool::VerticalSpace :
                                      selected == 1 || selected == 4 ? Tool::SelectLasso :
                                      selected == 2 ? Tool::SmartSelect :
                                      selected == 5 ? Tool::SelectObject :
                                      selected == 6 ? Tool::SelectPdfTextLinear :
                                      selected == 7 ? Tool::SelectPdfTextRect : Tool::SelectRectangle;
                    const bool acrossLayers = selected == 3 || selected == 4;
                    if (tool == Tool::VerticalSpace) {
                        self->selectTool(tool);
                        return;
                    }
                    if (prototype::isPdfTextSelectionTool(self->state.brush.tool) &&
                        prototype::isPdfTextSelectionTool(tool)) {
                        if (self->state.brush.tool != tool) {
                            self->switchPdfTextSelectionStyle();
                        }
                    } else if ((prototype::isSelectionTool(self->state.brush.tool) ||
                                self->state.brush.tool == Tool::VerticalSpace) &&
                        (tool != self->state.brush.tool || acrossLayers != self->state.selectionAcrossLayers)) {
                        self->activateSelection(tool, acrossLayers);
                    }
                }),
                this);
        gtk_box_append(GTK_BOX(toolbar), selectionModeDropdown);
        selectionActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(selectionActions, "linked");
        gtk_box_append(GTK_BOX(selectionActions), button("edit-copy-symbolic", "Copy selection (Ctrl+C)",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->copySelection(); })));
        gtk_box_append(GTK_BOX(selectionActions), button("edit-cut-symbolic", "Cut selection (Ctrl+X)",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->copySelection(true); })));
        gtk_box_append(GTK_BOX(selectionActions), button("edit-paste-symbolic", "Paste selection or image (Ctrl+V)",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->pasteSelection(); })));
        gtk_box_append(GTK_BOX(selectionActions), button("edit-duplicate-symbolic", "Duplicate selection",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->duplicateSelection(); })));
        gtk_box_append(GTK_BOX(toolbar), selectionActions);
        selectionLabel = gtk_label_new("Drag to select");
        gtk_widget_set_tooltip_text(selectionLabel, "Drag to move, drag a square to resize, or drag the circle to rotate");
        gtk_box_append(GTK_BOX(toolbar), selectionLabel);
        gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        auto* navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_add_css_class(navigation, "linked");
        prevPageButton = button("go-previous-symbolic", "Previous page (Page Up / Shift+K)",
                                G_CALLBACK(+[](GtkButton*, gpointer data) {
                                    auto* self = static_cast<Prototype*>(data);
                                    self->navigateToPage(self->state.page - 1);
                                }));
        gtk_box_append(GTK_BOX(navigation), prevPageButton);
        pageButton = gtk_button_new();
        gtk_widget_add_css_class(pageButton, "flat");
        gtk_widget_set_tooltip_text(pageButton, "Go to page (Ctrl+G or :)");
        pageLabel = gtk_label_new("");
        gtk_button_set_child(GTK_BUTTON(pageButton), pageLabel);
        g_signal_connect(pageButton, "clicked",
                         G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<Prototype*>(data)->openGotoPageDialog();
                         }),
                         this);
        gtk_box_append(GTK_BOX(navigation), pageButton);
        nextPageButton = button("go-next-symbolic", "Next page (Page Down / Shift+J)",
                                G_CALLBACK(+[](GtkButton*, gpointer data) {
                                    auto* self = static_cast<Prototype*>(data);
                                    self->navigateToPage(self->state.page + 1);
                                }));
        gtk_box_append(GTK_BOX(navigation), nextPageButton);
        gtk_box_append(GTK_BOX(navigation), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        insertPageButton = button("list-add-symbolic", "Insert page after (Ctrl+D)",
                                  G_CALLBACK(+[](GtkButton*, gpointer data) {
                                      static_cast<Prototype*>(data)->insertPageAfterCurrent();
                                  }));
        gtk_box_append(GTK_BOX(navigation), insertPageButton);
        deletePageButton = button("list-remove-symbolic", "Delete current page (Ctrl+Shift+Delete)",
                                  G_CALLBACK(+[](GtkButton*, gpointer data) {
                                      static_cast<Prototype*>(data)->deleteCurrentPage();
                                  }));
        gtk_box_append(GTK_BOX(navigation), deletePageButton);
        zoomLabel = gtk_label_new("");
        gtk_widget_set_size_request(zoomLabel, 60, -1);
        gtk_box_append(GTK_BOX(navigation), zoomLabel);
        gtk_box_append(GTK_BOX(toolbar), navigation);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), toolbar);
        mainToolRailWidget = createMainToolRail();
        selectTool(importedConfig.settings.activeTool);

        canvas = gtk_drawing_area_new();
        gtk_widget_set_focusable(canvas, true);
        gtk_widget_set_hexpand(canvas, true);
        gtk_widget_set_vexpand(canvas, true);
        gtk_widget_set_tooltip_text(canvas, "Draw with pen or mouse. Touch or middle-drag to pan. Pinch or Ctrl+wheel "
                                            "to zoom. Escape cancels ink.");
        canvasOverlay = gtk_overlay_new();
        gtk_overlay_set_child(GTK_OVERLAY(canvasOverlay), canvas);
        textEditor = std::make_unique<prototype::TextEditorOverlay>(GTK_OVERLAY(canvasOverlay));
        textEditor->setWrap(false);
        createPdfActionsPopover();
        createLinkInfoPopover();
        createFloatingToolbox();
        gtk_drawing_area_set_draw_func(
                GTK_DRAWING_AREA(canvas),
                +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
                    auto* self = static_cast<Prototype*>(data);
                    try {
                        self->draw(cr, width, height);
                    } catch (const std::exception& error) {
                        std::cerr << error.what() << '\n';
                        self->failed = true;
                        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                    }
                },
                this, nullptr);
        g_signal_connect(canvas, "resize", G_CALLBACK(+[](GtkDrawingArea*, int width, int height, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->fitMode || !self->fitted) {
                                 self->input.cancel();
                                 self->state.fit(width, height);
                                 self->fitted = true;
                                 self->fitMode = true;
                                 self->lastWidth = width;
                                 self->lastHeight = height;
                                 self->update();
                             }
                             if (gtk_widget_get_visible(self->floatingToolbox)) {
                                 self->positionFloatingToolbox(self->floatingToolboxAnchor.x,
                                                               self->floatingToolboxAnchor.y);
                             }
                         }),
                         this);
        splitView = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
        adw_overlay_split_view_set_content(splitView, canvasOverlay);
        adw_overlay_split_view_set_sidebar(splitView, createSidebar());
        adw_overlay_split_view_set_min_sidebar_width(splitView, 160);
        adw_overlay_split_view_set_max_sidebar_width(splitView, 180);
        // Sidebar edge swipes could steal drawing gestures. Its button is explicit.
        adw_overlay_split_view_set_enable_show_gesture(splitView, false);
        adw_overlay_split_view_set_enable_hide_gesture(splitView, false);
        adw_overlay_split_view_set_show_sidebar(splitView, importedConfig.settings.showSidebar);
        g_object_bind_property(splitView, "show-sidebar", sidebarButton, "active",
                               GBindingFlags(G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE));
        auto* adaptive = adw_breakpoint_bin_new();
        gtk_widget_set_size_request(adaptive, 320, 240);
        auto* workspace = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_append(GTK_BOX(workspace), mainToolRailWidget);
        gtk_box_append(GTK_BOX(workspace), GTK_WIDGET(splitView));
        adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(adaptive), workspace);
        auto* breakpoint = adw_breakpoint_new(
                adw_breakpoint_condition_new_length(ADW_BREAKPOINT_CONDITION_MAX_WIDTH, 720, ADW_LENGTH_UNIT_PX));
        adw_breakpoint_add_setters(breakpoint, G_OBJECT(splitView), "collapsed", TRUE, nullptr);
        adw_breakpoint_bin_add_breakpoint(ADW_BREAKPOINT_BIN(adaptive), breakpoint);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), adaptive);
        adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), view);
        connectInput();
        g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
            auto* self = static_cast<Prototype*>(data);
            self->finishTextEditing();
            if (self->busy || self->choosingFile || self->messageDialog) {
                return true;
            }
            if (self->autosaving) {
                self->waitForAutosave([self] { self->protectDocument([self] { self->closeWindow(); }); });
                return true;
            }
            self->protectDocument([self] { self->closeWindow(); });
            return true;
        }), this);
        g_signal_connect(window, "notify::is-active", G_CALLBACK(+[](GObject* window, GParamSpec*, gpointer data) {
                             if (!gtk_window_is_active(GTK_WINDOW(window))) {
                                 auto* self = static_cast<Prototype*>(data);
                                 self->resetInput(true);
                                 self->update();
                             }
                         }),
                         this);
        g_signal_connect(
                canvas, "unmap",
                G_CALLBACK(+[](GtkWidget*, gpointer data) { static_cast<Prototype*>(data)->resetInput(true); }), this);
        update();
        windowPaintable = gtk_widget_paintable_new(window);
        gtk_window_present(GTK_WINDOW(window));
        paintClock = GDK_FRAME_CLOCK(g_object_ref(gtk_widget_get_frame_clock(window)));
        g_signal_connect(paintClock, "after-paint",
                         G_CALLBACK(+[](GdkFrameClock*, gpointer data) { ++static_cast<Prototype*>(data)->paints; }),
                         this);
        gtk_widget_grab_focus(canvas);
        if (!options.worker.empty()) {
            scheduleAutosave(static_cast<guint>(std::max(0, effectiveAutosaveSeconds)));
            requestOpenDocument(sourcePath, true);
        }
        if (options.verifyPlugins || options.verifyBeamer || options.verifyLayerActions || options.verifyLatex) {
            g_timeout_add(
                    200,
                    +[](gpointer data) -> gboolean {
                        auto* self = static_cast<Prototype*>(data);
                        if (self->busy || (self->options.worker.size() && !self->nativeSession)) {
                            return G_SOURCE_CONTINUE;
                        }
                        try {
                            if (self->options.verifyPlugins) {
                                self->verifyPluginIntegration();
                            } else if (self->options.verifyBeamer) {
                                self->verifyBeamerIntegration();
                            } else if (self->options.verifyLayerActions) {
                                self->verifyLayerActionsIntegration();
                            } else {
                                self->verifyLatexIntegration();
                            }
                        } catch (const std::exception& error) {
                            std::cerr << (self->options.verifyLatex ? "LATEX" : "PLUGIN")
                                      << " VERIFY FAILED: " << error.what() << '\n';
                            self->failed = true;
                        }
                        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                        return G_SOURCE_REMOVE;
                    },
                    this);
        }
        if (const char* marker = g_getenv("XOURNALPP_CRASH_AFTER_AUTOSAVE_MARKER"); marker && *marker) {
            crashMarkerPath = marker;
            crashHelperSource = g_timeout_add(
                    50,
                    +[](gpointer data) -> gboolean {
                        auto* self = static_cast<Prototype*>(data);
                        try {
                            return self->stepCrashHelper();
                        } catch (const std::exception& error) {
                            std::cerr << "CRASH HELPER FAILED: " << error.what() << '\n';
                            self->failed = true;
                            g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                            self->crashHelperSource = 0;
                            return G_SOURCE_REMOVE;
                        }
                    },
                    this);
        }
        if (const char* action = g_getenv("XOURNALPP_RECOVERY_ACTION"); action && *action) {
            recoveryActionSpec = action;
            recoveryVerifySource = g_timeout_add(
                    80,
                    +[](gpointer data) -> gboolean {
                        auto* self = static_cast<Prototype*>(data);
                        try {
                            return self->stepRecoveryVerify();
                        } catch (const std::exception& error) {
                            std::cerr << "RECOVERY VERIFY FAILED: " << error.what() << '\n';
                            self->failed = true;
                            g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                            self->recoveryVerifySource = 0;
                            return G_SOURCE_REMOVE;
                        }
                    },
                    this);
        }
        if (!options.smokeDirectory.empty() && !options.verifyPlugins && !options.verifyBeamer &&
            !options.verifyLayerActions && !options.verifyLatex) {
            if (const char* verifyMode = g_getenv("XOURNALPP_SETTINGS_VERIFY_MODE"); verifyMode && *verifyMode) {
                settingsVerifyMode = verifyMode;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_PREFERENCES_VERIFY_MODE"); verifyMode && *verifyMode) {
                preferencesVerifyMode = verifyMode;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_LAYER_VERIFY_MODE"); verifyMode && *verifyMode) {
                layerVerifyMode = verifyMode;
                layerVerifyStep = 0;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_IMAGE_VERIFY_MODE"); verifyMode && *verifyMode) {
                imageVerifyMode = verifyMode;
                imageVerifySource = g_getenv("XOURNALPP_IMAGE_VERIFY_SOURCE") ?
                                            g_getenv("XOURNALPP_IMAGE_VERIFY_SOURCE") : "";
                imageVerifyStep = 0;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_SELECTION_VERIFY_MODE"); verifyMode && *verifyMode) {
                selectionVerifyMode = verifyMode;
                selectionVerifyStep = 0;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_PDF_TEXT_SELECTION_VERIFY_MODE");
                verifyMode && *verifyMode) {
                pdfTextSelectionVerifyMode = verifyMode;
                pdfTextSelectionVerifyStep = 0;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_LINK_VERIFY_MODE"); verifyMode && *verifyMode) {
                linkVerifyMode = verifyMode;
                linkVerifyStep = 0;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_LASER_VERIFY_MODE"); verifyMode && *verifyMode) {
                laserVerifyMode = verifyMode;
                laserVerifyStep = 0;
            }
            if (const char* verifyMode = g_getenv("XOURNALPP_PLAY_OBJECT_VERIFY_MODE"); verifyMode && *verifyMode) {
                playObjectVerifyMode = verifyMode;
                playObjectVerifyStep = 0;
            }
            heartbeatSource = g_timeout_add(10, +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                if (self->busy) {
                    ++self->busyHeartbeats;
                }
                if (self->autosaving) {
                    ++self->autosaveHeartbeats;
                }
                return G_SOURCE_CONTINUE;
            }, this);
            g_timeout_add(
                    150,
                    +[](gpointer data) -> gboolean {
                        auto* self = static_cast<Prototype*>(data);
                        try {
                            return self->smoke();
                        } catch (const SnapshotPending&) {
                            if (++self->snapshotWaits < 60) {
                                if (!self->settingsVerifyMode.empty()) {
                                    --self->settingsVerifyStep;
                                } else if (!self->preferencesVerifyMode.empty()) {
                                    --self->preferencesVerifyStep;
                                } else if (!self->imageVerifyMode.empty()) {
                                    --self->imageVerifyStep;
                                } else if (!self->layerVerifyMode.empty()) {
                                    --self->layerVerifyStep;
                                } else if (!self->selectionVerifyMode.empty()) {
                                    --self->selectionVerifyStep;
                                } else if (!self->pdfTextSelectionVerifyMode.empty()) {
                                    --self->pdfTextSelectionVerifyStep;
                                } else if (!self->laserVerifyMode.empty()) {
                                    --self->laserVerifyStep;
                                } else if (!self->playObjectVerifyMode.empty()) {
                                    --self->playObjectVerifyStep;
                                } else if (self->documentSmokeStep >= 0) {
                                    --self->documentSmokeStep;
                                } else {
                                    --self->smokeStep;
                                }
                                self->lastSmokePaint = self->paints;
                                gtk_widget_queue_draw(self->window);
                                return G_SOURCE_CONTINUE;
                            }
                            std::cerr << "SMOKE FAILED: window snapshot never became ready\n";
                            self->failed = true;
                            g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                            return G_SOURCE_REMOVE;
                        } catch (const std::exception& error) {
                            std::cerr << "SMOKE FAILED: " << error.what() << '\n';
                            self->failed = true;
                            g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self->window))));
                            return G_SOURCE_REMOVE;
                        }
                    },
                    this);
        }
    }

    void update() {
        if (closed) {
            return;
        }
        const auto& pageLayers = state.layersForPage(state.page);
        if (layerRows && (layerPanelPage != state.page || !layerPanelSnapshot ||
                          *layerPanelSnapshot != pageLayers)) {
            thumbnails.clear();
            rebuildLayerStack();
        }
        updateTextEditorPosition();
        updateTitle();
        if (selectionLabel) {
            std::string selectionStatus = state.hasSelection() ?
                                                  std::to_string(state.selectionCount()) + " selected" :
                                          pdfTextSelection && pdfTextSelection->finalized ?
                                                  std::to_string(g_utf8_strlen(pdfTextSelection->text.c_str(), -1)) +
                                                          " PDF characters selected" :
                                          pdfTextSelection && pdfTextSelection->dragging ? "Selecting PDF text" :
                                          state.brush.tool == Tool::SelectObject ? "Click to select" :
                                                                                   "Drag to select";
            gtk_label_set_text(GTK_LABEL(selectionLabel), selectionStatus.c_str());
        }
        if (pdfTextSelection && pdfTextSelection->finalized && pdfActionsPopover &&
            gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover))) {
            updatePdfActionsPosition(false);
        }
        gtk_widget_set_sensitive(openButton, !options.worker.empty());
        gtk_widget_set_sensitive(saveButton, bool(nativeSession));
        gtk_widget_set_sensitive(saveAsButton, bool(nativeSession));
        gtk_widget_set_sensitive(exportPdfButton, bool(nativeSession));
        gtk_widget_set_sensitive(textButton, bool(nativeSession));
        gtk_widget_set_sensitive(latexButton, bool(nativeSession));
        gtk_widget_set_sensitive(playObjectButton, bool(nativeSession));
        gtk_widget_set_sensitive(imageButton, bool(nativeSession));
        gtk_widget_set_sensitive(prevPageButton, state.page > 0);
        gtk_widget_set_sensitive(nextPageButton, state.page + 1 < pageCount);
        gtk_widget_set_sensitive(insertPageButton, bool(nativeSession));
        gtk_widget_set_sensitive(deletePageButton, bool(nativeSession) && pageCount > 1);
        const auto label = std::to_string(state.page + 1) + " / " + std::to_string(pageCount);
        gtk_label_set_text(GTK_LABEL(pageLabel), label.c_str());
        gtk_label_set_text(GTK_LABEL(zoomLabel), (std::to_string(static_cast<int>(state.zoom * 100)) + "%").c_str());
        gtk_widget_set_sensitive(undoButton, state.canUndo());
        gtk_widget_set_sensitive(redoButton, state.canRedo());
        if (pageList) {
            syncingPage = true;
            gtk_list_box_select_row(pageList, gtk_list_box_get_row_at_index(pageList, state.page));
            syncingPage = false;
        }
        for (auto* thumbnail: thumbnailWidgets) {
            if (gtk_widget_get_mapped(thumbnail)) {
                gtk_widget_queue_draw(thumbnail);
            }
        }
        gtk_widget_queue_draw(canvas);
        syncAudioControls();
        ensureLaserFadeTimer();
    }

    void ensureLaserFadeTimer() {
        if (laserFadeSource || !state.hasLaserFades()) {
            return;
        }
        laserFadeSource = g_timeout_add(
                prototype::behavior::laserPointerFadeStepMs,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    if (!self->state.hasLaserFades()) {
                        self->laserFadeSource = 0;
                        return G_SOURCE_REMOVE;
                    }
                    if (self->state.advanceLaserFades()) {
                        self->update();
                    }
                    if (!self->state.hasLaserFades()) {
                        self->laserFadeSource = 0;
                        return G_SOURCE_REMOVE;
                    }
                    return G_SOURCE_CONTINUE;
                },
                this);
    }

    void zoom(double value, double x, double y) {
        if (input.active()) {
            return;
        }
        state.zoomAt(value, x, y);
        fitMode = false;
        fitted = true;
        update();
    }

    static bool mouseEvent(GtkEventController* controller) {
        auto* event = gtk_event_controller_get_current_event(controller);
        if (!event) {
            return true;  // Signal-level integration tests have no backend event.
        }
        auto* device = gdk_event_get_device(event);
        return !gdk_event_get_device_tool(event) && device && gdk_device_get_source(device) != GDK_SOURCE_TOUCHSCREEN;
    }

    std::optional<prototype::Point> surfacePoint(double x, double y) const {
        auto* native = gtk_widget_get_native(canvas);
        double tx, ty;
        gtk_native_get_surface_transform(native, &tx, &ty);
        const graphene_point_t from{static_cast<float>(x + tx), static_cast<float>(y + ty)};
        graphene_point_t to;
        if (!gtk_widget_compute_point(GTK_WIDGET(native), canvas, &from, &to)) {
            return std::nullopt;
        }
        return prototype::Point{to.x, to.y};
    }

    prototype::Point scrollAnchor(GtkEventController* controller) const {
        double x, y;
        auto* event = gtk_event_controller_get_current_event(controller);
        if (event && gdk_event_get_position(event, &x, &y)) {
            if (const auto point = surfacePoint(x, y)) {
                return *point;
            }
        }
        return {gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0};
    }

    prototype::CanvasImage displayedImage(size_t index) const {
        if (imageGesture && imageGesture->index == index) {
            return imageGesture->preview;
        }
        return state.displayImage(index);
    }

    std::optional<size_t> imageHandleAt(const prototype::CanvasImage& image, const prototype::Point& point) const {
        const auto corners = prototype::canvasImageCorners(image);
        const double radius = 8.0 / std::max(0.1, state.zoom);
        for (size_t index = 0; index < corners.size(); ++index) {
            if (std::hypot(corners[index].x - point.x, corners[index].y - point.y) <= radius) {
                return index;
            }
        }
        const auto rotationHandle = prototype::canvasImageRotationHandle(image, state.zoom);
        if (std::hypot(rotationHandle.x - point.x, rotationHandle.y - point.y) <= radius) {
            return corners.size();
        }
        return std::nullopt;
    }

    bool beginImageGesture(Device device, double x, double y) {
        if (state.hasSelection() || state.brush.tool != Tool::SmartSelect || input.spaceHeld || input.shiftHeld ||
            input.panning() ||
            (device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
            return false;
        }
        const auto hit = state.hitPage(x, y);
        if (!hit) {
            return false;
        }

        std::optional<size_t> picked = state.pickImage(hit->page, hit->local);
        std::optional<size_t> handle;
        if (selectedImageIndex && *selectedImageIndex < state.images.size()) {
            const auto& selectedImage = state.images[*selectedImageIndex];
            if (selectedImage.page == hit->page && selectedImage.layerId == state.activeLayerId(hit->page) &&
                state.layerVisible(hit->page, selectedImage.layerId)) {
                handle = imageHandleAt(selectedImage, hit->local);
                if (handle) {
                    picked = selectedImageIndex;
                }
            }
        }
        if (!picked) {
            selectedImageIndex.reset();
            return false;
        }

        if (!handle) {
            const auto pickedStroke = state.pickStroke(hit->page, hit->local);
            const auto pickedText = state.pickText(hit->page, hit->local);
            const auto imageOrder = state.images[*picked].layerOrder;
            if ((pickedStroke && state.ink[*pickedStroke].layerOrder > imageOrder) ||
                (pickedText && state.texts[*pickedText].layerOrder > imageOrder)) {
                return false;
            }
        }

        const auto& image = state.images[*picked];
        if (image.page != hit->page || image.layerId != state.activeLayerId(hit->page) ||
            !state.layerVisible(hit->page, image.layerId)) {
            return false;
        }
        if (*picked != selectedImageIndex.value_or(state.images.size())) {
            state.clearSelection();
        }
        state.selectActivePage(hit->page);
        selectedImageIndex = *picked;
        ImageGesture gesture;
        gesture.owner = device;
        gesture.index = *picked;
        gesture.mode = !handle ? ImageGesture::Mode::Move :
                       *handle == 4 ? ImageGesture::Mode::Rotate : ImageGesture::Mode::Resize;
        gesture.corner = handle.value_or(0);
        gesture.start = hit->local;
        gesture.center = prototype::canvasImageCenter(image);
        gesture.startAngle = std::atan2(hit->local.y - gesture.center.y, hit->local.x - gesture.center.x);
        gesture.original = image;
        gesture.preview = image;
        imageGesture = std::move(gesture);
        return true;
    }

    void updateImageGesture(double x, double y) {
        if (!imageGesture) {
            return;
        }
        auto& gesture = *imageGesture;
        const auto point = state.pageLocalPoint(gesture.original.page, x, y);
        if (gesture.mode == ImageGesture::Mode::Move) {
            gesture.preview.transform.x = gesture.original.transform.x + point.x - gesture.start.x;
            gesture.preview.transform.y = gesture.original.transform.y + point.y - gesture.start.y;
        } else if (gesture.mode == ImageGesture::Mode::Rotate) {
            const double currentAngle = std::atan2(point.y - gesture.center.y, point.x - gesture.center.x);
            const double angle = std::remainder(currentAngle - gesture.startAngle, 2 * G_PI);
            gesture.rotationDelta = angle;
            prototype::rotateCanvasImage(gesture.preview, angle);
        } else {
            constexpr std::array<prototype::Point, 4> localCorners = {
                    prototype::Point{0, 0}, prototype::Point{1, 0}, prototype::Point{1, 1},
                    prototype::Point{0, 1}};
            const auto corners = prototype::canvasImageCorners(gesture.original);
            const size_t opposite = (gesture.corner + 2) % corners.size();
            const auto anchor = corners[opposite];
            const double diagonalX = corners[gesture.corner].x - anchor.x;
            const double diagonalY = corners[gesture.corner].y - anchor.y;
            const double denominator = diagonalX * diagonalX + diagonalY * diagonalY;
            if (denominator > 1e-8) {
                const double factor = std::clamp(((point.x - anchor.x) * diagonalX +
                                                  (point.y - anchor.y) * diagonalY) / denominator,
                                                 0.05, 10.0);
                const auto& source = gesture.original.transform;
                auto& target = gesture.preview.transform;
                target.xx = source.xx * factor;
                target.yx = source.yx * factor;
                target.xy = source.xy * factor;
                target.yy = source.yy * factor;
                const double anchorX = localCorners[opposite].x * prototype::canvasImageWidth(gesture.original);
                const double anchorY = localCorners[opposite].y * prototype::canvasImageHeight(gesture.original);
                target.x = anchor.x - target.xx * anchorX - target.xy * anchorY;
                target.y = anchor.y - target.yx * anchorX - target.yy * anchorY;
            }
        }
        const auto& before = gesture.original.transform;
        const auto& after = gesture.preview.transform;
        gesture.changed = before.xx != after.xx || before.yx != after.yx || before.xy != after.xy ||
                          before.yy != after.yy || before.x != after.x || before.y != after.y;
    }

    void finishImageGesture() {
        if (!imageGesture) {
            return;
        }
        auto gesture = std::move(*imageGesture);
        imageGesture.reset();
        if (gesture.mode == ImageGesture::Mode::Rotate) {
            gesture.preview = gesture.original;
            const double angle = prototype::canvasImageRotationDelta(
                    gesture.startAngle, gesture.startAngle + gesture.rotationDelta,
                    state.snapRotationEnabled != input.altHeld, state.snapRotationTolerance);
            prototype::rotateCanvasImage(gesture.preview, angle);
            const auto& before = gesture.original.transform;
            const auto& after = gesture.preview.transform;
            gesture.changed = before.xx != after.xx || before.yx != after.yx || before.xy != after.xy ||
                              before.yy != after.yy || before.x != after.x || before.y != after.y;
        }
        if (gesture.changed && state.replaceImage(gesture.index, std::move(gesture.preview))) {
            selectedImageIndex = gesture.index;
            thumbnails.clear();
        }
        update();
    }

    LatexTarget latexTargetAt(int page, const prototype::Point& point, bool includeText = true) const {
        LatexTarget target;
        const auto activeLayer = state.activeLayerId(page);
        if (!state.layerVisible(page, activeLayer)) {
            return target;
        }

        size_t bestOrder = 0;
        bool found = false;
        const auto contains = [&point](double xx, double yx, double xy, double yy, double x, double y,
                                       double width, double height) {
            const double determinant = xx * yy - xy * yx;
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-10) {
                return false;
            }
            const double dx = point.x - x;
            const double dy = point.y - y;
            const double localX = (yy * dx - xy * dy) / determinant;
            const double localY = (-yx * dx + xx * dy) / determinant;
            return localX >= 0 && localX <= width && localY >= 0 && localY <= height;
        };

        for (size_t index = 0; index < state.images.size(); ++index) {
            const auto& image = state.images[index];
            if (image.page != page || image.layerId != activeLayer ||
                image.kind != prototype::CanvasImageKind::Latex || !prototype::validCanvasImage(image) ||
                !contains(image.transform.xx, image.transform.yx, image.transform.xy, image.transform.yy,
                          image.transform.x, image.transform.y, prototype::canvasImageWidth(image),
                          prototype::canvasImageHeight(image))) {
                continue;
            }
            if (!found || image.layerOrder >= bestOrder) {
                target = {index, std::nullopt};
                bestOrder = image.layerOrder;
                found = true;
            }
        }

        if (includeText) {
            for (size_t index = 0; index < state.texts.size(); ++index) {
                const auto& item = state.texts[index];
                if (item.page != page || item.layerId != activeLayer || item.text.empty() ||
                    !contains(item.xx, item.yx, item.xy, item.yy, item.x, item.y,
                              item.layoutWidth, item.layoutHeight)) {
                    continue;
                }
                if (!found || item.layerOrder >= bestOrder) {
                    target = {std::nullopt, index};
                    bestOrder = item.layerOrder;
                    found = true;
                }
            }
        }
        return target;
    }

    prototype::Point textTopLeft(const prototype::TextItem& item) const {
        double width = item.layoutWidth;
        double height = item.layoutHeight;
        if (width <= 0 || height <= 0) {
            const auto natural = prototype::textNaturalSize(item);
            width = std::max(width, natural.first);
            height = std::max(height, natural.second);
        }
        const std::array<prototype::Point, 4> corners = {
                prototype::Point{item.x, item.y},
                prototype::Point{item.x + item.xx * width, item.y + item.yx * width},
                prototype::Point{item.x + item.xy * height, item.y + item.yy * height},
                prototype::Point{item.x + item.xx * width + item.xy * height,
                                 item.y + item.yx * width + item.yy * height},
        };
        prototype::Point topLeft{corners[0].x, corners[0].y};
        for (const auto& corner: corners) {
            topLeft.x = std::min(topLeft.x, corner.x);
            topLeft.y = std::min(topLeft.y, corner.y);
        }
        return topLeft;
    }

    void openLatexEditorAt(double x, double y, bool existingLatexOnly = false) {
        if (!nativeSession || !window || busy || choosingFile || messageDialog || latexEditor) {
            return;
        }

        finishTextEditing();
        const auto hit = state.hitPage(x, y);
        if (!hit) {
            return;
        }
        const auto target = latexTargetAt(hit->page, hit->local, !existingLatexOnly);
        if (existingLatexOnly && !target.imageIndex) {
            return;
        }

        const int page = hit->page;
        auto position = hit->local;
        std::string source = importedConfig.settings.latexDefaultText;
        std::optional<prototype::CanvasImage> originalImage;
        std::optional<prototype::TextItem> originalText;
        if (target.imageIndex) {
            originalImage = state.images[*target.imageIndex];
            source = originalImage->latexSource;
        } else if (target.textIndex) {
            originalText = state.texts[*target.textIndex];
            source = "\\text{" + originalText->text + "}";
            position = textTopLeft(*originalText);
        }

        resetInput(true);
        state.selectActivePage(page);
        const auto documentSession = nativeSession;
        const auto textIndex = target.textIndex;
        const auto imageIndex = target.imageIndex;
        const auto insertedAt = position;
        const bool selectInitialSource = !originalImage && !originalText;
        const auto& latexSettings = importedConfig.settings;
        prototype::LatexEditorPreferences editorPreferences;
        editorPreferences.sourceViewThemeId = latexSettings.latexSourceViewThemeId;
        editorPreferences.sourceViewAutoIndent = latexSettings.latexSourceViewAutoIndent;
        editorPreferences.sourceViewSyntaxHighlight = latexSettings.latexSourceViewSyntaxHighlight;
        editorPreferences.sourceViewShowLineNumbers = latexSettings.latexSourceViewShowLineNumbers;
        editorPreferences.editorFont = latexSettings.latexEditorFont;
        editorPreferences.useCustomEditorFont = latexSettings.latexUseCustomEditorFont;
        editorPreferences.editorWordWrap = latexSettings.latexEditorWordWrap;
        editorPreferences.useExternalEditor = latexSettings.latexUseExternalEditor;
        editorPreferences.externalEditorAutoConfirm = latexSettings.latexExternalEditorAutoConfirm;
        editorPreferences.externalEditorCommand = latexSettings.latexExternalEditorCommand;
        editorPreferences.temporaryFileExtension = latexSettings.latexTemporaryFileExtension;
        latexEditor = prototype::LatexEditor::create(
                GTK_WINDOW(window), latexTemplatePath, importedConfig.settings.latexGeneratorCommand,
                std::move(source), state.brush.color, originalImage, selectInitialSource,
                std::move(editorPreferences),
                [this, documentSession, page, textIndex, imageIndex, originalImage, originalText,
                 insertedAt](std::optional<prototype::CanvasImage> result) {
                    latexEditor.reset();
                    if (!result) {
                        update();
                        return;
                    }
                    if (nativeSession != documentSession || !nativeSession || page < 0 || page >= state.pageCount()) {
                        return;
                    }

                    result->page = page;
                    size_t insertedIndex = state.images.size();
                    bool changed = false;
                    if (imageIndex && originalImage) {
                        result->transform = originalImage->transform;
                        result->layerId = originalImage->layerId;
                        result->layerOrder = originalImage->layerOrder;
                        result->originLayer = originalImage->originLayer;
                        changed = state.replaceImage(*imageIndex, std::move(*result));
                        insertedIndex = *imageIndex;
                    } else if (textIndex && originalText) {
                        const auto topLeft = textTopLeft(*originalText);
                        result->transform = {1, 0, 0, 1, topLeft.x, topLeft.y};
                        changed = state.replaceTextWithImage(*textIndex, std::move(*result));
                        insertedIndex = state.images.empty() ? 0 : state.images.size() - 1;
                    } else {
                        result->transform = {1, 0, 0, 1, insertedAt.x, insertedAt.y};
                        changed = state.insertImage(std::move(*result));
                        insertedIndex = state.images.empty() ? 0 : state.images.size() - 1;
                    }

                    if (!changed) {
                        showError("The LaTeX formula could not be added to the active page.");
                        update();
                        return;
                    }
                    thumbnails.clear();
                    activateSelection(Tool::SmartSelect);
                    selectedImageIndex = insertedIndex;
                    update();
                });
        latexEditor->present();
    }

    void insertImageFromPath(const std::string& path) {
        if (!nativeSession) {
            return;
        }
        try {
            auto image = prototype::readCanvasImageFile(path);
            const auto page = state.pageSize(state.page);
            image.page = state.page;
            const double scale = std::min(page.width * 0.6 / image.pixelWidth,
                                          page.height * 0.6 / image.pixelHeight);
            const double width = image.pixelWidth * scale;
            const double height = image.pixelHeight * scale;
            image.transform = {scale, 0, 0, scale, (page.width - width) / 2, (page.height - height) / 2};
            selectedImageIndex.reset();
            selectTool(Tool::SmartSelect);
            if (!state.insertImage(std::move(image))) {
                showError("The image could not be inserted into the active page.");
                return;
            }
            thumbnails.clear();
            update();
        } catch (const std::exception& error) {
            showError(std::string("Cannot insert image: ") + error.what());
        }
    }

    void chooseImage() {
        if (!nativeSession || busy || choosingFile || messageDialog) {
            return;
        }
        if (autosaving) {
            waitForAutosave([this] { chooseImage(); });
            return;
        }
        finishTextEditing();
        resetInput(true);
        choosingFile = true;
        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Insert Image");
        auto* filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "Images");
        for (const char* mime: {"image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp", "image/tiff"}) {
            gtk_file_filter_add_mime_type(filter, mime);
        }
        gtk_file_dialog_set_default_filter(dialog, filter);
        g_object_unref(filter);
        gtk_file_dialog_open(dialog, GTK_WINDOW(window), nullptr,
                             +[](GObject* source, GAsyncResult* result, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 self->choosingFile = false;
                                 GError* error = nullptr;
                                 auto* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
                                 self->finishFileChoice(file, error, [self](const std::string& path) {
                                     self->insertImageFromPath(path);
                                 });
                             },
                             this);
        g_object_unref(dialog);
    }

    void scheduleSelectionEdgePan(double x, double y) {
        if (!state.selectionCanEdgePan()) {
            stopSelectionEdgePan();
            return;
        }
        selectionEdgePanPointer = prototype::Point{x, y};
        if (selectionEdgePanSource) {
            return;
        }
        selectionEdgePanSource = g_timeout_add(
                1000 / 30,
                +[](gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    if (!self->selectionEdgePanPointer || !self->state.selectionCanEdgePan() || !self->canvas) {
                        self->selectionEdgePanSource = 0;
                        return G_SOURCE_REMOVE;
                    }
                    const auto width = gtk_widget_get_width(self->canvas);
                    const auto height = gtk_widget_get_height(self->canvas);
                    const auto pointer = *self->selectionEdgePanPointer;
                    if (!self->state.edgePanSelectionAtPointer(pointer.x, pointer.y, width, height,
                                                               self->input.altHeld)) {
                        self->selectionEdgePanSource = 0;
                        return G_SOURCE_REMOVE;
                    }
                    self->fitted = true;
                    self->fitMode = false;
                    self->state.syncActivePage(width, height);
                    self->update();
                    return G_SOURCE_CONTINUE;
                },
                this);
    }

    void stopSelectionEdgePan() {
        if (selectionEdgePanSource) {
            g_source_remove(selectionEdgePanSource);
            selectionEdgePanSource = 0;
        }
        selectionEdgePanPointer.reset();
    }

    void resetInput(bool interrupted = false) {
        stopSelectionEdgePan();
        imageGesture.reset();
        smartSelectPdfGesture.reset();
        state.cancelVerticalSpace();
        if (textSelectionOwner) {
            state.cancelSelection();
        }
        textSelectionOwner.reset();
        pendingStylusTextTap.reset();
        textDragHandled = false;
        if (interrupted) {
            input.interrupt();
        } else {
            input.reset();
        }
        for (auto* pan: panGestures) {
            static_cast<PanState*>(g_object_get_data(G_OBJECT(pan), "pan-state"))->enabled = false;
        }
        if (canvas) {
            gtk_widget_set_cursor_from_name(canvas, state.brush.tool == Tool::Hand ? "grab" : nullptr);
        }
    }

    void playObjectAt(double screenX, double screenY) {
        constexpr double actionRadius = 15.0;
        const auto hit = state.hitPage(screenX, screenY);
        if (!hit) {
            return;
        }
        state.selectActivePage(hit->page);
        const auto& pageLayers = state.layersForPage(hit->page);
        if (pageLayers.selectedLayer == 0 || pageLayers.selectedLayer > pageLayers.layers.size()) {
            return;
        }

        const auto& activeOrigins = pageLayers.layers[pageLayers.selectedLayer - 1].origins;
        std::optional<std::string> playbackError;
        for (size_t origin: activeOrigins) {
            for (const auto& target: audioTargets) {
                if (target.page != hit->page || target.originLayer != origin ||
                    !prototype::audioTargetIntersectsArea(target, hit->local.x - actionRadius,
                                                          hit->local.y - actionRadius,
                                                          2 * actionRadius, 2 * actionRadius) ||
                    prototype::audioTargetDistance(target, hit->local.x, hit->local.y) >= actionRadius) {
                    continue;
                }
                std::string error;
                const auto filename = target.playbackFilename.empty() ? target.filename : target.playbackFilename;
                if (!audioPlayback || !audioPlayback->start(filename, target.timestampMs, error)) {
                    playbackError = error.empty() ? "Audio playback is unavailable" : std::move(error);
                } else {
                    playbackError.reset();
                }
            }
        }
        if (playbackError) {
            showAlert("Unable to play audio recording", *playbackError);
        }
        update();
    }

    void syncAudioControls() {
        if (!audioControls || !audioPlayback) {
            return;
        }
        const bool active = audioPlayback->isActive();
        gtk_widget_set_visible(audioControls, active);
        if (audioPauseButton) {
            const bool paused = audioPlayback->isPaused();
            gtk_button_set_icon_name(GTK_BUTTON(audioPauseButton),
                                     paused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic");
            gtk_widget_set_tooltip_text(audioPauseButton, paused ? "Resume audio playback" : "Pause audio playback");
        }
        for (auto* control: {audioBackButton, audioPauseButton, audioForwardButton, audioStopButton}) {
            if (control) {
                gtk_widget_set_sensitive(control, active);
            }
        }
    }

    void trackPointerPosition(double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) {
            return;
        }
        if (pointerPosition && pointerPosition->x == x && pointerPosition->y == y) {
            return;
        }
        pointerPosition = prototype::Point{x, y};
        if (importedConfig.settings.highlightPosition && canvas) {
            gtk_widget_queue_draw(canvas);
        }
    }

    void begin(Device device, double x, double y, std::optional<double> pressure = std::nullopt,
               bool eraserEnd = false, uint32_t timeMs = 0) {
        trackPointerPosition(x, y);
        if (state.brush.tool == Tool::Latex) {
            gtk_widget_grab_focus(canvas);
            if (device == Device::Mouse) {
                return;
            }
            if (device != Device::Stylus || (!eraserEnd && activeStylusAction == prototype::ButtonToolAction::None)) {
                openLatexEditorAt(x, y);
                return;
            }
        }
        if (state.brush.tool == Tool::FloatingToolbox ||
            (device == Device::Stylus && !eraserEnd &&
             activeStylusAction == prototype::ButtonToolAction::FloatingToolbox)) {
            showFloatingToolbox(x, y);
            gtk_widget_grab_focus(canvas);
            return;
        }
        if (state.brush.tool == Tool::PlayObject) {
            gtk_widget_grab_focus(canvas);
            playObjectAt(x, y);
            return;
        }
        if (state.brush.tool == Tool::VerticalSpace &&
            !(device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
            state.beginVerticalSpace(x, y, input.controlHeld);
            gtk_widget_grab_focus(canvas);
            update();
            return;
        }
        if (state.brush.tool == Tool::Link) {
            gtk_widget_grab_focus(canvas);
            return;
        }
        if (state.brush.tool == Tool::Text && device == Device::Stylus && !eraserEnd &&
            activeStylusAction == prototype::ButtonToolAction::None && !(textEditor && textEditor->active())) {
            pendingStylusTextTap = prototype::Point{x, y};
            gtk_widget_grab_focus(canvas);
            return;
        }
        if (beginTextTransform(device, x, y)) {
            gtk_widget_grab_focus(canvas);
            update();
            return;
        }
        if (prototype::isPdfTextSelectionTool(state.brush.tool)) {
            gtk_widget_grab_focus(canvas);
            beginPdfTextSelection(device, x, y);
            return;
        }
        if (beginSmartSelectPdfGesture(device, x, y)) {
            gtk_widget_grab_focus(canvas);
            return;
        }
        if (pdfTextSelection) {
            clearPdfTextSelection(false);
        }
        if (beginImageGesture(device, x, y)) {
            gtk_widget_grab_focus(canvas);
            update();
            return;
        }
        const bool preserveSelection = state.brush.tool == Tool::Hand || input.spaceHeld ||
                                       (device == Device::Stylus && !eraserEnd &&
                                        activeStylusAction == prototype::ButtonToolAction::Hand) ||
                                       !state.hitPage(x, y);
        if (!preserveSelection) {
            selectedImageIndex.reset();
        }
        gtk_widget_grab_focus(canvas);
        const auto toolbarBrush = state.brush;
        const bool previousSpaceHeld = input.spaceHeld;
        if (device == Device::Stylus && !eraserEnd) {
            switch (activeStylusAction) {
                case prototype::ButtonToolAction::Pen:
                    state.brush = penBrush;
                    break;
                case prototype::ButtonToolAction::Highlighter:
                    state.brush = highlightBrush;
                    break;
                case prototype::ButtonToolAction::Eraser:
                    state.brush = eraserBrush;
                    break;
                case prototype::ButtonToolAction::Hand:
                    input.spaceHeld = true;
                    break;
                case prototype::ButtonToolAction::FloatingToolbox:
                    break;
                case prototype::ButtonToolAction::None:
                    break;
            }
        }
        input.begin(device, {x, y, pressure, timeMs}, eraserEnd);
        state.brush = toolbarBrush;
        input.spaceHeld = previousSpaceHeld;
        if (input.panning()) {
            gtk_widget_set_cursor_from_name(canvas, "grabbing");
        } else if (state.brush.tool == Tool::Hand || input.spaceHeld) {
            gtk_widget_set_cursor_from_name(canvas, "grab");
        }
        update();
    }

    void motion(Device device, double x, double y, std::optional<double> pressure = std::nullopt,
                uint32_t timeMs = 0) {
        trackPointerPosition(x, y);
        if (state.brush.tool == Tool::Link) {
            updateLinkHover(x, y);
            return;
        }
        if (smartSelectPdfGesture && smartSelectPdfGesture->owner == device) {
            smartSelectPdfGesture->latest = {x, y};
            const auto& start = smartSelectPdfGesture->start;
            if (std::max(std::abs(x - start.x), std::abs(y - start.y)) < 10.0) {
                return;
            }
            const auto gesture = *smartSelectPdfGesture;
            smartSelectPdfGesture.reset();
            beginPdfTextSelection(device, gesture.start.x, gesture.start.y);
            updatePdfTextSelection(device, x, y);
            return;
        }
        if (state.verticalSpacePreview()) {
            state.moveVerticalSpace(x, y, input.controlHeld);
            update();
            return;
        }
        if (device == Device::Stylus && pendingStylusTextTap) {
            const auto start = *pendingStylusTextTap;
            if (std::hypot(x - start.x, y - start.y) < 3.0) {
                return;
            }
            pendingStylusTextTap.reset();
            if (beginTextTransform(device, start.x, start.y)) {
                state.moveSelectionPointer(x, y, input.altHeld);
                scheduleSelectionEdgePan(x, y);
                update();
            }
            return;
        }
        if (textSelectionOwner && *textSelectionOwner == device) {
            state.moveSelectionPointer(x, y, input.altHeld);
            scheduleSelectionEdgePan(x, y);
            update();
            return;
        }
        if (pdfTextSelection && pdfTextSelection->dragging && pdfTextSelection->owner == device) {
            updatePdfTextSelection(device, x, y);
            return;
        }
        if (imageGesture && imageGesture->owner == device) {
            updateImageGesture(x, y);
            update();
            return;
        }
        input.motion(device, {x, y, pressure, timeMs});
        if (input.panning()) {
            fitted = true;
            fitMode = false;
            state.syncActivePage(gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
        }
        update();
    }

    void finish(Device device, std::optional<double> releasePressure = std::nullopt,
                std::optional<prototype::Point> releasePoint = std::nullopt) {
        if (releasePoint) {
            trackPointerPosition(releasePoint->x, releasePoint->y);
        }
        if (smartSelectPdfGesture && smartSelectPdfGesture->owner == device) {
            if (releasePoint) {
                smartSelectPdfGesture->latest = *releasePoint;
            }
            const auto gesture = *smartSelectPdfGesture;
            smartSelectPdfGesture.reset();
            const auto& end = gesture.latest;
            const bool exceededThreshold = std::max(std::abs(end.x - gesture.start.x),
                                                    std::abs(end.y - gesture.start.y)) >= 10.0;
            if (exceededThreshold) {
                beginPdfTextSelection(device, gesture.start.x, gesture.start.y);
                updatePdfTextSelection(device, end.x, end.y);
                finishPdfTextSelection(device);
            } else {
                state.clearSelection();
                selectedImageIndex.reset();
                clearPdfTextSelection(false);
                update();
            }
            return;
        }
        if (device == Device::Stylus && state.brush.tool == Tool::Link && releasePoint) {
            handleLinkClick(releasePoint->x, releasePoint->y, 1, input.controlHeld);
            update();
            return;
        }
        if (state.verticalSpacePreview()) {
            if (state.finishVerticalSpace()) {
                thumbnails.clear();
            }
            update();
            return;
        }
        if (textSelectionOwner && *textSelectionOwner == device) {
            stopSelectionEdgePan();
            if (releasePoint) {
                state.moveSelectionPointer(releasePoint->x, releasePoint->y, input.altHeld);
            }
            state.finishSelection();
            textSelectionOwner.reset();
            gtk_widget_set_cursor_from_name(canvas, nullptr);
            update();
            return;
        }
        if (device == Device::Stylus && pendingStylusTextTap) {
            const auto point = *pendingStylusTextTap;
            pendingStylusTextTap.reset();
            startTextEditing(point.x, point.y);
            update();
            return;
        }
        if (pdfTextSelection && pdfTextSelection->dragging && pdfTextSelection->owner == device) {
            finishPdfTextSelection(device);
            return;
        }
        if (imageGesture && imageGesture->owner == device) {
            finishImageGesture();
            return;
        }
        input.finish(device, releasePressure);
        if (!input.active()) {
            gtk_widget_set_cursor_from_name(canvas,
                                            input.spaceHeld || state.brush.tool == Tool::Hand ? "grab" : nullptr);
        }
        update();
    }

    static std::optional<double> pressure(GtkGestureStylus* stylus) {
        double value;
        if (gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(stylus)) &&
            gtk_gesture_stylus_get_axis(stylus, GDK_AXIS_PRESSURE, &value) && std::isfinite(value)) {
            return value;
        }
        return std::nullopt;
    }

    void consumeBacklog(std::span<const GdkTimeCoord> samples) {
        for (const auto& sample: samples) {
            if (const auto point = prototype::historySample(sample)) {
                if (pendingStylusTextTap ||
                    (smartSelectPdfGesture && smartSelectPdfGesture->owner == Device::Stylus) ||
                    (textSelectionOwner && *textSelectionOwner == Device::Stylus)) {
                    motion(Device::Stylus, point->x, point->y, point->pressure, point->timeMs);
                } else {
                    input.motion(Device::Stylus, *point);
                }
            }
        }
    }

    void stylusMotion(GtkGestureStylus* gesture, double x, double y) {
        if (gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture))) {
            GdkTimeCoord* samples = nullptr;
            guint count = 0;
            if (gtk_gesture_stylus_get_backlog(gesture, &samples, &count)) {
                consumeBacklog({samples, count});
            }
            g_free(samples);
        }
        auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
        motion(Device::Stylus, x, y, pressure(gesture), event ? gdk_event_get_time(event) : 0);
    }

    struct PanState {
        double x = 0;
        double y = 0;
        bool enabled = false;
    };

    void connectInput() {
        mouse = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mouse), GDK_BUTTON_PRIMARY);
        g_signal_connect(mouse, "drag-begin",
                         G_CALLBACK(+[](GtkGestureDrag* gesture, double x, double y, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (mouseEvent(GTK_EVENT_CONTROLLER(gesture)) &&
                                 self->state.drawingMode != prototype::DrawingMode::Spline) {
                                 self->dragX = x;
                                 self->dragY = y;
                                 auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                                 self->begin(Device::Mouse, x, y, std::nullopt, false,
                                             event ? gdk_event_get_time(event) : 0);
                             }
                         }),
                         this);
        g_signal_connect(mouse, "drag-update",
                         G_CALLBACK(+[](GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (mouseEvent(GTK_EVENT_CONTROLLER(gesture)) &&
                                 self->state.drawingMode != prototype::DrawingMode::Spline) {
                                 auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                                 self->motion(Device::Mouse, self->dragX + dx, self->dragY + dy, std::nullopt,
                                              event ? gdk_event_get_time(event) : 0);
                             }
                         }),
                         this);
        g_signal_connect(mouse, "drag-end", G_CALLBACK(+[](GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (mouseEvent(GTK_EVENT_CONTROLLER(gesture)) &&
                                 self->state.drawingMode != prototype::DrawingMode::Spline) {
                                 self->finish(Device::Mouse, std::nullopt,
                                              prototype::Point{self->dragX + dx, self->dragY + dy});
                                 self->update();
                             }
                         }),
                         this);
        addGesture(mouse, Device::Mouse);

        textClick = GTK_GESTURE_CLICK(gtk_gesture_click_new());
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(textClick), GDK_BUTTON_PRIMARY);
        g_signal_connect(textClick, "pressed",
                         G_CALLBACK(+[](GtkGestureClick* gesture, int presses, double x, double y, gpointer data) {
                             if (!mouseEvent(GTK_EVENT_CONTROLLER(gesture))) return;
                             auto* self = static_cast<Prototype*>(data);
                             self->textDragHandled = false;
                             if (self->state.drawingMode == prototype::DrawingMode::Spline &&
                                 (self->state.brush.tool == Tool::Pen || self->state.brush.tool == Tool::Highlighter)) {
                                 self->input.splinePress(Device::Mouse, {x, y, std::nullopt}, presses >= 2);
                                 self->update();
                             }
                         }), this);
        g_signal_connect(textClick, "released",
                         G_CALLBACK(+[](GtkGestureClick* gesture, int presses, double x, double y, gpointer data) {
                             if (!mouseEvent(GTK_EVENT_CONTROLLER(gesture))) return;
                             auto* self = static_cast<Prototype*>(data);
                             if (self->state.drawingMode == prototype::DrawingMode::Spline &&
                                 (self->state.brush.tool == Tool::Pen || self->state.brush.tool == Tool::Highlighter)) {
                                 self->input.finish(Device::Mouse);
                                 self->update();
                                 return;
                             }
                             if (self->textDragHandled) {
                                 self->textDragHandled = false;
                                 return;
                             }
                             const auto tool = self->state.brush.tool;
                             if (presses > 1 && tool != Tool::Link) {
                                 const auto hit = self->state.hitPage(x, y);
                                 if (hit) {
                                     if (const auto index = self->state.pickLink(hit->page, hit->local)) {
                                         self->openLinkEditor(hit->page,
                                                              {self->state.texts[*index].x,
                                                               self->state.texts[*index].y}, index);
                                         self->update();
                                         return;
                                     }
                                 }
                             }
                             if (tool == Tool::Link) {
                                 const auto modifiers = gtk_event_controller_get_current_event_state(
                                         GTK_EVENT_CONTROLLER(gesture));
                                 self->handleLinkClick(x, y, presses, (modifiers & GDK_CONTROL_MASK) != 0);
                                 self->update();
                                 return;
                             }
                             if (prototype::isPdfTextSelectionTool(tool) && (presses == 2 || presses == 3)) {
                                 self->selectPdfTextAtPoint(
                                         x, y, presses == 2 ? prototype::PdfTextSelectionStyle::Word :
                                                             prototype::PdfTextSelectionStyle::Line);
                                 return;
                             }
                             if (tool == Tool::Latex) {
                                 if (presses == 1) {
                                     self->openLatexEditorAt(x, y);
                                 }
                                 return;
                             }
                             if (tool == Tool::SmartSelect && presses == 2) {
                                 const auto hit = self->state.hitPage(x, y);
                                 if (hit) {
                                     const auto object = self->state.pickSelectableObject(hit->page, hit->local);
                                     if (object && object->kind == prototype::SelectableObjectKind::Image &&
                                         self->state.images[object->index].kind == prototype::CanvasImageKind::Latex) {
                                         self->selectTool(Tool::Latex);
                                         self->openLatexEditorAt(x, y, true);
                                         self->update();
                                         return;
                                     }
                                 }
                             }
                             if (presses != 1) return;
                             if (tool == Tool::Text) {
                                 self->startTextEditing(x, y);
                             } else if (tool == Tool::SmartSelect) {
                                 const auto hit = self->state.hitPage(x, y);
                                 if (hit && self->textAt(hit->page, hit->local)) {
                                     self->startTextEditing(x, y);
                                 }
                             }
                         }),
                         this);
        g_signal_connect(textClick, "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (self->state.drawingMode == prototype::DrawingMode::Spline && self->input.active()) {
                                 self->input.finish(Device::Mouse);
                                 self->update();
                             }
                         }), this);
        addGesture(GTK_GESTURE(textClick));

        pointerMotion = gtk_event_controller_motion_new();
        g_signal_connect(pointerMotion, "motion", G_CALLBACK(+[](GtkEventControllerMotion* controller,
                                                                   double x, double y, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->trackPointerPosition(x, y);
                             if (self->state.brush.tool == Tool::Link) {
                                 self->updateLinkHover(x, y);
                                 return;
                             }
                             if (self->state.drawingMode != prototype::DrawingMode::Spline ||
                                 (self->state.brush.tool != Tool::Pen && self->state.brush.tool != Tool::Highlighter)) {
                                 return;
                             }
                             if (self->input.active()) {
                                 auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
                                 self->input.motion(Device::Mouse, {x, y, std::nullopt,
                                                                    event ? gdk_event_get_time(event) : 0});
                             } else {
                                 self->input.splineHover(x, y);
                             }
                             self->update();
                         }), this);
        g_signal_connect(pointerMotion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const bool hadPointer = self->pointerPosition.has_value();
                             self->pointerPosition.reset();
                             if (hadPointer && self->importedConfig.settings.highlightPosition) {
                                 gtk_widget_queue_draw(self->canvas);
                             }
                             self->scheduleFloatingToolboxHide();
                             if (self->state.brush.tool == Tool::Link) {
                                 self->linkInfoIndex.reset();
                                 if (self->linkInfoPopover) {
                                     gtk_popover_popdown(GTK_POPOVER(self->linkInfoPopover));
                                 }
                                 gtk_widget_set_cursor_from_name(self->canvas, "pointer");
                             }
                         }), this);
        gtk_widget_add_controller(canvas, pointerMotion);

        stylus = gtk_gesture_stylus_new();
        gtk_gesture_stylus_set_stylus_only(GTK_GESTURE_STYLUS(stylus), true);
        g_signal_connect(stylus, "down", G_CALLBACK(+[](GtkGestureStylus* gesture, double x, double y, gpointer data) {
                             auto* tool = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture)) ?
                                                  gtk_gesture_stylus_get_device_tool(gesture) :
                                                  nullptr;
                             const bool eraser = tool && prototype::isEraserEnd(gdk_device_tool_get_tool_type(tool));
                             auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                             static_cast<Prototype*>(data)->begin(Device::Stylus, x, y, pressure(gesture), eraser,
                                                                   event ? gdk_event_get_time(event) : 0);
                         }),
                         this);
        g_signal_connect(stylus, "motion",
                         G_CALLBACK(+[](GtkGestureStylus* gesture, double x, double y, gpointer data) {
                             static_cast<Prototype*>(data)->stylusMotion(gesture, x, y);
                         }),
                         this);
        g_signal_connect(stylus, "up", G_CALLBACK(+[](GtkGestureStylus* gesture, double x, double y, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->finish(Device::Stylus, pressure(gesture), prototype::Point{x, y});
                             self->update();
                         }),
                         this);
        addGesture(stylus, Device::Stylus);

        stylusButtonClick = GTK_GESTURE_CLICK(gtk_gesture_click_new());
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(stylusButtonClick), 0);
        g_signal_connect(stylusButtonClick, "pressed",
                         G_CALLBACK(+[](GtkGestureClick* gesture, int, double x, double y, gpointer data) {
                             auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                             if (!event || !gdk_event_get_device_tool(event)) {
                                 return;
                             }
                             const auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
                             auto* self = static_cast<Prototype*>(data);
                             const auto next = button == GDK_BUTTON_MIDDLE ?
                                                       self->importedConfig.settings.stylusButtonOne :
                                               button == GDK_BUTTON_SECONDARY ?
                                                       self->importedConfig.settings.stylusButtonTwo :
                                                                               prototype::ButtonToolAction::None;
                             if (button != GDK_BUTTON_MIDDLE && button != GDK_BUTTON_SECONDARY) {
                                 return;
                             }
                             if (next == self->activeStylusAction && self->activeStylusButton == button) {
                                 return;
                             }
                             const bool restart = self->input.active(Device::Stylus) &&
                                                  (self->activeStylusAction != prototype::ButtonToolAction::None ||
                                                   next != prototype::ButtonToolAction::None);
                             if (restart) {
                                 self->input.finish(Device::Stylus);
                             }
                             self->activeStylusButton = button;
                             self->activeStylusAction = next;
                             if (next == prototype::ButtonToolAction::FloatingToolbox) {
                                 self->showFloatingToolbox(x, y);
                             } else if (restart) {
                                 self->begin(Device::Stylus, x, y);
                             } else {
                                 self->update();
                             }
                         }),
                         this);
        g_signal_connect(stylusButtonClick, "released",
                         G_CALLBACK(+[](GtkGestureClick* gesture, int, double x, double y, gpointer data) {
                             auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                             if (!event || !gdk_event_get_device_tool(event)) {
                                 return;
                             }
                             auto* self = static_cast<Prototype*>(data);
                             const auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
                             if (button != self->activeStylusButton) {
                                 return;
                             }
                             const bool restart = self->activeStylusAction != prototype::ButtonToolAction::None &&
                                                  self->input.active(Device::Stylus);
                             if (restart) {
                                 self->input.finish(Device::Stylus);
                             }
                             self->activeStylusButton = 0;
                             self->activeStylusAction = prototype::ButtonToolAction::None;
                             if (restart) {
                                 self->begin(Device::Stylus, x, y);
                             } else {
                                 self->update();
                             }
                         }),
                         this);
        addGesture(GTK_GESTURE(stylusButtonClick));

        // Each pan sequence keeps its own incremental offsets. A pinch or pen
        // sequence permanently suppresses that pan until a new drag begins.
        for (bool touch: {false, true}) {
            auto* pan = gtk_gesture_drag_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan), touch ? GDK_BUTTON_PRIMARY : GDK_BUTTON_MIDDLE);
            gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(pan), touch);
            g_object_set_data_full(
                    G_OBJECT(pan), "pan-state", new PanState,
                    +[](gpointer data) { delete static_cast<PanState*>(data); });
            g_object_set_data(G_OBJECT(pan), "touch-pan", GINT_TO_POINTER(touch));
            g_signal_connect(pan, "drag-begin", G_CALLBACK(+[](GtkGestureDrag* gesture, double, double, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 auto* pan = static_cast<PanState*>(g_object_get_data(G_OBJECT(gesture), "pan-state"));
                                 const bool touch = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "touch-pan"));
                                 pan->x = 0;
                                 pan->y = 0;
                                 pan->enabled = (touch || mouseEvent(GTK_EVENT_CONTROLLER(gesture))) &&
                                                !self->input.active() &&
                                                !self->state.blocksScroll(false, g_get_monotonic_time());
                             }),
                             this);
            g_signal_connect(pan, "drag-update",
                             G_CALLBACK(+[](GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 auto* pan = static_cast<PanState*>(g_object_get_data(G_OBJECT(gesture), "pan-state"));
                                 if (self->input.active() || self->state.blocksScroll(false, g_get_monotonic_time())) {
                                     pan->enabled = false;
                                 }
                                 if (pan->enabled) {
                                     self->state.offsetX += dx - pan->x;
                                     self->state.offsetY += dy - pan->y;
                                     self->fitted = true;
                                     self->fitMode = false;
                                     self->state.syncActivePage(gtk_widget_get_width(self->canvas),
                                                                gtk_widget_get_height(self->canvas));
                                     self->update();
                                 }
                                 pan->x = dx;
                                 pan->y = dy;
                             }),
                             this);
            g_signal_connect(pan, "cancel", G_CALLBACK(+[](GtkGesture* gesture, GdkEventSequence*, gpointer) {
                                 static_cast<PanState*>(g_object_get_data(G_OBJECT(gesture), "pan-state"))->enabled =
                                         false;
                             }),
                             nullptr);
            addGesture(pan);
            panGestures.push_back(pan);
        }

        pinch = gtk_gesture_zoom_new();
        g_signal_connect(pinch, "begin", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->smartSelectPdfGesture.reset();
                             self->input.cancel();
                             self->state.beginPinch();
                             self->pinchZoom = self->state.zoom;
                             self->pinchX = gtk_widget_get_width(self->canvas) / 2.0;
                             self->pinchY = gtk_widget_get_height(self->canvas) / 2.0;
                             if (gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(self->pinch))) {
                                 gtk_gesture_get_bounding_box_center(self->pinch, &self->pinchX, &self->pinchY);
                             }
                         }),
                         this);
        g_signal_connect(pinch, "scale-changed", G_CALLBACK(+[](GtkGestureZoom*, double scale, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->zoom(self->pinchZoom * scale, self->pinchX, self->pinchY);
                         }),
                         this);
        g_signal_connect(pinch, "end", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
                             static_cast<Prototype*>(data)->state.endPinch(g_get_monotonic_time());
                         }),
                         this);
        addGesture(pinch);

        auto* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
        g_signal_connect(
                scroll, "scroll",
                G_CALLBACK(+[](GtkEventControllerScroll* controller, double dx, double dy, gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
                    const bool discrete = event && gdk_scroll_event_get_unit(event) == GDK_SCROLL_UNIT_WHEEL;
                    const auto modifiers =
                            gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)) &
                            gtk_accelerator_get_default_mod_mask();
                    if (modifiers & ~(GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
                        return true;
                    }
                    if (self->input.active() || self->state.blocksScroll(discrete, g_get_monotonic_time())) {
                        return true;
                    }
                    if (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)) &
                        GDK_CONTROL_MASK) {
                        if (dy != 0) {
                            const auto anchor = self->scrollAnchor(GTK_EVENT_CONTROLLER(controller));
                            self->zoom(self->state.steppedZoom(dy < 0, true), anchor.x, anchor.y);
                        }
                    } else {
                        self->state.offsetX -= dx * 35;
                        self->state.offsetY -= dy * 35;
                        self->fitted = true;
                        self->fitMode = false;
                        self->state.syncActivePage(gtk_widget_get_width(self->canvas),
                                                   gtk_widget_get_height(self->canvas));
                        self->update();
                    }
                    return true;
                }),
                this);
        gtk_widget_add_controller(canvas, scroll);

        keys = gtk_event_controller_key_new();
        g_signal_connect(
                keys, "key-pressed",
                G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType modifiers,
                               gpointer data) -> gboolean {
                    auto* self = static_cast<Prototype*>(data);
                    if (self->busy || self->choosingFile || self->messageDialog) {
                        return false;
                    }
                    bool editingText = self->textEditor && self->textEditor->active();
                    if (editingText) {
                        const bool control = (modifiers & GDK_CONTROL_MASK) != 0;
                        const bool shift = (modifiers & GDK_SHIFT_MASK) != 0;
                        if (control && (key == GDK_KEY_b || key == GDK_KEY_B)) {
                            self->applyTextFormat(prototype::TextEditorOverlay::Format::Bold);
                            return true;
                        }
                        if (control && (key == GDK_KEY_i || key == GDK_KEY_I)) {
                            self->applyTextFormat(prototype::TextEditorOverlay::Format::Italic);
                            return true;
                        }
                        if (control && (key == GDK_KEY_u || key == GDK_KEY_U)) {
                            self->applyTextFormat(prototype::TextEditorOverlay::Format::Underline);
                            return true;
                        }
                        if (control && shift && (key == GDK_KEY_k || key == GDK_KEY_K)) {
                            self->applyTextFormat(prototype::TextEditorOverlay::Format::Strikethrough);
                            return true;
                        }
                        if (control && key == GDK_KEY_period) {
                            self->applyTextFormat(prototype::TextEditorOverlay::Format::Superscript);
                            return true;
                        }
                        if (control && key == GDK_KEY_comma) {
                            self->applyTextFormat(prototype::TextEditorOverlay::Format::Subscript);
                            return true;
                        }
                        if (key == GDK_KEY_Escape || (control && key == GDK_KEY_Return)) {
                            self->finishTextEditing();
                            return true;
                        }
                        const bool documentAction =
                                (control && (key == GDK_KEY_o || key == GDK_KEY_O || key == GDK_KEY_s ||
                                             key == GDK_KEY_S || key == GDK_KEY_e || key == GDK_KEY_E ||
                                             key == GDK_KEY_g || key == GDK_KEY_G || key == GDK_KEY_d ||
                                             key == GDK_KEY_D || key == GDK_KEY_0 || key == GDK_KEY_plus ||
                                             key == GDK_KEY_equal || key == GDK_KEY_minus || key == GDK_KEY_Home ||
                                             key == GDK_KEY_End)) ||
                                (control && shift && (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete ||
                                                      key == GDK_KEY_g || key == GDK_KEY_G || key == GDK_KEY_r ||
                                                      key == GDK_KEY_R)) ||
                                key == GDK_KEY_Page_Down || key == GDK_KEY_Page_Up ||
                                ((modifiers & GDK_SHIFT_MASK) && (key == GDK_KEY_j || key == GDK_KEY_J ||
                                                                  key == GDK_KEY_k || key == GDK_KEY_K));
                        if (documentAction) {
                            self->finishTextEditing();
                            editingText = false;
                        } else {
                            return false;
                        }
                    }
                    if (!editingText && !GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(self->window))) &&
                        self->pluginRuntime && self->pluginRuntime->handleKeyPress(key, modifiers, false)) {
                        self->update();
                        return true;
                    }
                    self->input.setModifiers((modifiers & GDK_SHIFT_MASK) || key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R,
                                             (modifiers & GDK_CONTROL_MASK) || key == GDK_KEY_Control_L || key == GDK_KEY_Control_R,
                                             (modifiers & GDK_ALT_MASK) || key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R);
                    if (self->state.verticalSpacePreview() &&
                        (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R)) {
                        self->state.setVerticalSpaceDirection(self->input.controlHeld);
                        self->update();
                        return true;
                    }
                    if (self->input.splineInProgress && !editingText &&
                        !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
                        if (key == GDK_KEY_BackSpace) {
                            self->input.removeSplineKnot();
                            self->update();
                            return true;
                        }
                        if (key == GDK_KEY_Left || key == GDK_KEY_Right || key == GDK_KEY_Up || key == GDK_KEY_Down) {
                            const double delta = key == GDK_KEY_Left ? -1 : key == GDK_KEY_Right ? 1 : 0;
                            const double vertical = key == GDK_KEY_Up ? -1 : key == GDK_KEY_Down ? 1 : 0;
                            self->input.moveLastSplineKnot(delta, vertical);
                            self->update();
                            return true;
                        }
                        if (key == GDK_KEY_r || key == GDK_KEY_R) {
                            self->input.rotateLastSplineTangent(key == GDK_KEY_R ? -5 : 5);
                            self->update();
                            return true;
                        }
                        if (key == GDK_KEY_s || key == GDK_KEY_S) {
                            self->input.scaleLastSplineTangent(key == GDK_KEY_S ? 1.0 / 1.05 : 1.05);
                            self->update();
                            return true;
                        }
                    }
                    if (!editingText && !GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(self->window)))) {
                        if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_c || key == GDK_KEY_C)) {
                            if (self->pdfTextSelection && self->pdfTextSelection->finalized) {
                                self->copyPdfTextSelection();
                            } else {
                                self->copySelection();
                            }
                            return true;
                        }
                        if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_x || key == GDK_KEY_X)) {
                            if (self->pdfTextSelection && self->pdfTextSelection->finalized) {
                                self->copyPdfTextSelection();
                            } else {
                                self->copySelection(true);
                            }
                            return true;
                        }
                        if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_v || key == GDK_KEY_V)) {
                            self->pasteSelection(); return true;
                        }
                        if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) && (key == GDK_KEY_g || key == GDK_KEY_G)) {
                            self->activateSelection(Tool::SelectLasso); return true;
                        }
                        if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) && (key == GDK_KEY_s || key == GDK_KEY_S)) {
                            self->activateSelection(Tool::SmartSelect); return true;
                        }
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_o || key == GDK_KEY_O)) {
                        if (!self->options.worker.empty()) {
                            self->protectDocument([self] { self->chooseOpen(); });
                        }
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_s || key == GDK_KEY_S)) {
                        if (modifiers & GDK_SHIFT_MASK) {
                            self->chooseSave();
                        } else {
                            self->save();
                        }
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && !(modifiers & GDK_SHIFT_MASK) &&
                        (key == GDK_KEY_e || key == GDK_KEY_E)) {
                        self->chooseExportPdf();
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && !(modifiers & GDK_SHIFT_MASK) &&
                        (key == GDK_KEY_g || key == GDK_KEY_G)) {
                        self->openGotoPageDialog();
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && !(modifiers & GDK_SHIFT_MASK) &&
                        (key == GDK_KEY_d || key == GDK_KEY_D)) {
                        self->insertPageAfterCurrent();
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
                        (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete)) {
                        self->deleteCurrentPage();
                        return true;
                    }
                    if (key == GDK_KEY_Escape) {
                        if (self->pdfTextSelection) {
                            self->clearPdfTextSelection(false);
                        } else if (self->imageGesture) {
                            self->resetInput();
                            self->selectedImageIndex.reset();
                        } else if (self->selectedImageIndex) {
                            self->selectedImageIndex.reset();
                        } else if (self->input.splineInProgress) {
                            self->input.finalizeSpline();
                        } else {
                            self->resetInput();
                            self->state.clearSelection();
                        }
                    } else if (editingText || GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(self->window)))) {
                        return false;
                    } else if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
                               (key == GDK_KEY_r || key == GDK_KEY_R)) {
                        self->activateSelection(Tool::SelectRectangle);
                    } else if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_a || key == GDK_KEY_A)) {
                        self->selectedImageIndex.reset();
                        self->input.cancel();
                        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toolButtons[3]), true);
                        self->state.selectAllOnPage();
                    } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                               (key == GDK_KEY_Delete || key == GDK_KEY_BackSpace) && self->selectedImageIndex) {
                        self->resetInput();
                        self->state.deleteImage(*self->selectedImageIndex);
                        self->selectedImageIndex.reset();
                        self->thumbnails.clear();
                    } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                               (key == GDK_KEY_Delete || key == GDK_KEY_BackSpace) && self->state.hasSelection()) {
                        self->input.cancel();
                        self->state.deleteSelection();
                    } else if (key == GDK_KEY_space && !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK))) {
                        self->input.spaceHeld = true;
                        gtk_widget_set_cursor_from_name(self->canvas, "grab");
                    } else if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_z || key == GDK_KEY_Z)) {
                        self->selectedImageIndex.reset();
                        if (modifiers & GDK_SHIFT_MASK) {
                            self->input.cancel();
                            self->state.redo();
                        } else {
                            self->input.cancel();
                            self->state.undo();
                        }
                    } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_y) {
                        self->selectedImageIndex.reset();
                        self->input.cancel();
                        self->state.redo();
                    } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_0) {
                        self->fitted = false;
                    } else if ((modifiers & GDK_CONTROL_MASK) &&
                               (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_minus)) {
                        self->zoom(self->state.steppedZoom(key != GDK_KEY_minus, false),
                                   gtk_widget_get_width(self->canvas) / 2.0, gtk_widget_get_height(self->canvas) / 2.0);
                    } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_Home) {
                        self->navigateToPage(0);
                    } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_End) {
                        self->navigateToPage(self->pageCount - 1);
                    } else if (key == GDK_KEY_Page_Down || key == GDK_KEY_Page_Up) {
                        self->navigateToPage(self->state.page + (key == GDK_KEY_Page_Down ? 1 : -1));
                    } else if ((modifiers & GDK_SHIFT_MASK) &&
                               !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                               (key == GDK_KEY_J || key == GDK_KEY_j || key == GDK_KEY_K || key == GDK_KEY_k)) {
                        self->navigateToPage(self->state.page + ((key == GDK_KEY_J || key == GDK_KEY_j) ? 1 : -1));
                    } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                               key == GDK_KEY_colon) {
                        self->openGotoPageDialog();
                        return true;
                    } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                               (key == GDK_KEY_p || key == GDK_KEY_h || key == GDK_KEY_e ||
                                ((key == GDK_KEY_t || key == GDK_KEY_l) && self->nativeSession))) {
                        if (key == GDK_KEY_t) {
                            self->selectTool(Tool::Text);
                            return true;
                        }
                        if (key == GDK_KEY_l) {
                            self->selectTool(Tool::Link);
                            return true;
                        }
                        const auto tool = key == GDK_KEY_p ? 0 : key == GDK_KEY_h ? 1 : 2;
                        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toolButtons[tool]), true);
                    } else {
                        return false;
                    }
                    self->update();
                    return true;
                }),
                this);
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-released",
                         G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType modifiers, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             bool shift = modifiers & GDK_SHIFT_MASK;
                             bool control = modifiers & GDK_CONTROL_MASK;
                             bool alt = modifiers & GDK_ALT_MASK;
                             if (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R) shift = false;
                             if (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R) control = false;
                             if (key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R) alt = false;
                             self->input.setModifiers(shift, control, alt);
                             if (self->state.verticalSpacePreview() &&
                                 (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R)) {
                                 self->state.setVerticalSpaceDirection(self->input.controlHeld);
                             }
                             if (key == GDK_KEY_space) {
                                 self->input.spaceHeld = false;
                                 gtk_widget_set_cursor_from_name(self->canvas,
                                                                 self->input.panning() ? "grabbing" :
                                                                 self->state.brush.tool == Tool::Hand ? "grab" :
                                                                 nullptr);
                             }
                             self->update();
                         }),
                         this);
        gtk_widget_add_controller(window, keys);
    }

    void addGesture(GtkGesture* gesture, std::optional<Device> inkOwner = std::nullopt) {
        if (inkOwner) {
            g_object_set_data(G_OBJECT(gesture), "ink-owner", GUINT_TO_POINTER(*inkOwner == Device::Mouse ? 1 : 2));
        }
        g_signal_connect(gesture, "cancel", G_CALLBACK(+[](GtkGesture* gesture, GdkEventSequence*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             const auto owner = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(gesture), "ink-owner"));
                             if (owner && !(owner == 1 && self->state.drawingMode == prototype::DrawingMode::Spline)) {
                                 auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                                 if (event && gdk_event_get_event_type(event) == GDK_GRAB_BROKEN) {
                                     self->finish(owner == 1 ? Device::Mouse : Device::Stylus);
                                 } else {
                                     self->input.cancel(owner == 1 ? Device::Mouse : Device::Stylus);
                                 }
                             }
                             if (gesture == self->pinch && self->state.pinching) {
                                 self->state.endPinch(g_get_monotonic_time());
                             }
                             self->update();
                         }),
                         this);
        gtk_widget_add_controller(canvas, GTK_EVENT_CONTROLLER(gesture));
    }

    static constexpr size_t maxCachedPages = 4;

    const PageCacheEntry& ensurePageCache(int pageIndex, int deviceScale) {
        const auto sz = state.pageSize(pageIndex);
        // Bound both dimensions and memory per page, even when zoomed into a huge PDF page.
        const auto scale = std::min({state.zoom * deviceScale, 4096.0 / sz.width, 4096.0 / sz.height});
        for (auto& entry: pageCache) {
            if (entry.pageIndex == pageIndex && entry.originLayer == prototype::newLayerOrigin &&
                std::abs(entry.scale - scale) < 0.0001) {
                entry.stamp = ++cacheClock;
                return entry;
            }
        }
        const int width = std::max(1, static_cast<int>(std::ceil(sz.width * scale)));
        const int height = std::max(1, static_cast<int>(std::ceil(sz.height * scale)));
        std::unique_ptr<cairo_surface_t, SurfaceDeleter> surface(
                cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
            throw std::runtime_error("Cannot allocate page surface");
        }
        auto* cr = cairo_create(surface.get());
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, scale, scale);
        if (document && pageIndex >= 0 && pageIndex < pageCount) {
            if (auto* pdfPage = poppler_document_get_page(document, pageIndex)) {
                poppler_page_render(pdfPage, cr);
                g_object_unref(pdfPage);
            }
        }
        cairo_destroy(cr);
        ++rasterizations;
        PageCacheEntry* slot = nullptr;
        for (auto& entry: pageCache) {
            if (entry.pageIndex == pageIndex && entry.originLayer == prototype::newLayerOrigin) {
                slot = &entry;
                break;
            }
        }
        if (!slot) {
            if (pageCache.size() < maxCachedPages) {
                pageCache.push_back({});
                slot = &pageCache.back();
            } else {
                slot = &*std::min_element(pageCache.begin(), pageCache.end(),
                                          [](const PageCacheEntry& a, const PageCacheEntry& b) {
                                              return a.stamp < b.stamp;
                                          });
            }
        }
        slot->pageIndex = pageIndex;
        slot->originLayer = prototype::newLayerOrigin;
        slot->scale = scale;
        slot->stamp = ++cacheClock;
        slot->surface = std::move(surface);
        return *slot;
    }

    const PageCacheEntry& ensureLayerPageCache(int pageIndex, size_t originLayer, int deviceScale) {
        const auto sz = state.pageSize(pageIndex);
        const auto scale = std::min({state.zoom * deviceScale, 4096.0 / sz.width, 4096.0 / sz.height});
        const bool audioMode = state.brush.tool == Tool::PlayObject;
        for (auto& entry: layerPageCache) {
            if (entry.pageIndex == pageIndex && entry.originLayer == originLayer &&
                entry.audioMode == audioMode && std::abs(entry.scale - scale) < 0.0001) {
                entry.stamp = ++cacheClock;
                return entry;
            }
        }
        const int width = std::max(1, static_cast<int>(std::ceil(sz.width * scale)));
        const int height = std::max(1, static_cast<int>(std::ceil(sz.height * scale)));
        std::unique_ptr<cairo_surface_t, SurfaceDeleter> surface(
                cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
            throw std::runtime_error("Cannot allocate native layer surface");
        }
        auto* cr = cairo_create(surface.get());
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_paint(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_scale(cr, scale, scale);
        const auto& documents = audioMode ? audioLayerDocuments : layerDocuments;
        if (originLayer < documents.size() && pageIndex >= 0 && pageIndex < pageCount) {
            if (auto* pdfPage = poppler_document_get_page(documents[originLayer], pageIndex)) {
                poppler_page_render(pdfPage, cr);
                g_object_unref(pdfPage);
            }
        }
        cairo_destroy(cr);
        ++rasterizations;
        PageCacheEntry* slot = nullptr;
        for (auto& entry: layerPageCache) {
            if (entry.pageIndex == pageIndex && entry.originLayer == originLayer && entry.audioMode == audioMode) {
                slot = &entry;
                break;
            }
        }
        if (!slot) {
            constexpr size_t maxCachedLayerPages = 16;
            if (layerPageCache.size() < maxCachedLayerPages) {
                layerPageCache.push_back({});
                slot = &layerPageCache.back();
            } else {
                slot = &*std::min_element(layerPageCache.begin(), layerPageCache.end(),
                                          [](const PageCacheEntry& a, const PageCacheEntry& b) {
                                              return a.stamp < b.stamp;
                                          });
            }
        }
        slot->pageIndex = pageIndex;
        slot->originLayer = originLayer;
        slot->scale = scale;
        slot->audioMode = audioMode;
        slot->stamp = ++cacheClock;
        slot->surface = std::move(surface);
        return *slot;
    }

    const PageCacheEntry& ensureCache(int deviceScale) { return ensurePageCache(state.page, deviceScale); }

    size_t totalCacheBytes() const {
        size_t total = 0;
        for (const auto& entry: pageCache) {
            if (entry.surface) {
                total += static_cast<size_t>(cairo_image_surface_get_stride(entry.surface.get())) *
                         static_cast<size_t>(cairo_image_surface_get_height(entry.surface.get()));
            }
        }
        for (const auto& entry: layerPageCache) {
            if (entry.surface) {
                total += static_cast<size_t>(cairo_image_surface_get_stride(entry.surface.get())) *
                         static_cast<size_t>(cairo_image_surface_get_height(entry.surface.get()));
            }
        }
        return total;
    }

    void draw(cairo_t* cr, int width, int height) {
        if (!fitted || (fitMode && (width != lastWidth || height != lastHeight))) {
            input.cancel();
            state.fit(width, height);
            fitted = true;
            fitMode = true;
        }
        lastWidth = width;
        lastHeight = height;
        const int deviceScale = gtk_widget_get_scale_factor(canvas);
        const auto theme = importedConfig.settings.themeVariant;
        const bool darkCanvas = options.explicitDark || theme == prototype::ThemeVariant::ForceDark ||
                (theme == prototype::ThemeVariant::UseSystem &&
                 adw_style_manager_get_dark(adw_style_manager_get_default()));
        if (darkCanvas) {
            cairo_set_source_rgb(cr, 0.13, 0.15, 0.19);
        } else {
            cairo_set_source_rgb(cr, 0.88, 0.88, 0.88);
        }
        cairo_paint(cr);
        if (darkCanvas) {
            cairo_new_path(cr);
            for (int y = 12; y < height; y += 26) {
                for (int x = 12; x < width; x += 26) {
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, x, y, 1.25, 0, 2 * G_PI);
                }
            }
            cairo_set_source_rgba(cr, 0.25, 0.28, 0.33, 0.55);
            cairo_fill(cr);
        }
        bool drewAny = false;
        for (int i = 0; i < state.pageCount(); ++i) {
            const auto sz = state.pageSize(i);
            const double sx0 = state.offsetX + state.pageLeft(i) * state.zoom;
            const double sy0 = state.offsetY + state.pageTop(i) * state.zoom;
            const double sx1 = sx0 + sz.width * state.zoom;
            const double sy1 = sy0 + sz.height * state.zoom;
            if (sx1 <= 0 || sy1 <= 0 || sx0 >= width || sy0 >= height) {
                continue;
            }
            drewAny = true;
            const auto& cached = ensurePageCache(i, deviceScale);
            cairo_save(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, darkCanvas ? 0.45 : 0.18);
            cairo_rectangle(cr, sx0 - 0.5, sy0 - 0.5, (sx1 - sx0) + 1.0, (sy1 - sy0) + 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_restore(cr);

            cairo_save(cr);
            cairo_translate(cr, sx0, sy0);
            cairo_scale(cr, state.zoom, state.zoom);
            cairo_rectangle(cr, 0, 0, sz.width, sz.height);
            cairo_clip(cr);
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_paint(cr);
            if (state.layerVisible(i, 0)) {
                cairo_save(cr);
                cairo_scale(cr, 1.0 / cached.scale, 1.0 / cached.scale);
                cairo_set_source_surface(cr, cached.surface.get(), 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                cairo_paint(cr);
                cairo_restore(cr);
            }
            enum class EditableKind { Ink, Text, Image };
            struct LayerItem {
                size_t order;
                EditableKind kind;
                size_t index;
            };
            const bool fadeEditableObjects = state.brush.tool == Tool::PlayObject;
            const auto drawEditableObject = [&](auto&& drawObject) {
                if (!fadeEditableObjects) {
                    drawObject();
                    return;
                }
                cairo_push_group(cr);
                drawObject();
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, 0.3);
            };
            for (size_t layerIndex = 0; layerIndex < state.layersForPage(i).layers.size(); ++layerIndex) {
                const size_t layerId = layerIndex + 1;
                const auto& layer = state.layersForPage(i).layers[layerIndex];
                if (!layer.visible) {
                    continue;
                }
                for (size_t origin: layer.origins) {
                    if (origin < layerDocuments.size()) {
                        const auto& nativeLayer = ensureLayerPageCache(i, origin, deviceScale);
                        cairo_save(cr);
                        cairo_scale(cr, 1.0 / nativeLayer.scale, 1.0 / nativeLayer.scale);
                        cairo_set_source_surface(cr, nativeLayer.surface.get(), 0, 0);
                        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                        cairo_paint(cr);
                        cairo_restore(cr);
                    }
                    std::vector<LayerItem> layerItems;
                    layerItems.reserve(state.ink.size() + state.texts.size());
                    for (size_t index = 0; index < state.ink.size(); ++index) {
                        if (state.displayPage(index) == i && state.displayLayerId(index) == layerId &&
                            state.displayLayerOrigin(index) == origin && !state.verticalSpaceAffectsInk(index)) {
                            layerItems.push_back({state.ink[index].layerOrder, EditableKind::Ink, index});
                        }
                    }
                    for (size_t index = 0; index < state.texts.size(); ++index) {
                        if (state.verticalSpaceAffectsText(index)) {
                            continue;
                        }
                        const auto text = state.displayText(index);
                        if (text.page == i && text.layerId == layerId && text.originLayer == origin) {
                            layerItems.push_back({text.layerOrder, EditableKind::Text, index});
                        }
                    }
                    for (size_t index = 0; index < state.images.size(); ++index) {
                        if (state.verticalSpaceAffectsImage(index)) {
                            continue;
                        }
                        const auto image = displayedImage(index);
                        if (image.page == i && image.layerId == layerId && image.originLayer == origin) {
                            layerItems.push_back({image.layerOrder, EditableKind::Image, index});
                        }
                    }
                    std::stable_sort(layerItems.begin(), layerItems.end(),
                                     [](const LayerItem& a, const LayerItem& b) { return a.order < b.order; });
                    for (const auto& item: layerItems) {
                        if (item.kind == EditableKind::Text) {
                            const auto text = state.displayText(item.index);
                            drawEditableObject([&] { prototype::drawText(cr, text); });
                        } else if (item.kind == EditableKind::Image) {
                            drawEditableObject([&] { prototype::drawCanvasImage(cr, displayedImage(item.index)); });
                        } else {
                            drawEditableObject([&] { prototype::drawInkPreview(cr, state, item.index); });
                        }
                    }
                    if (state.current && state.current->page == i && state.current->layerId == layerId &&
                        state.current->originLayer == origin) {
                        prototype::drawInk(cr, *state.current);
                    }
                }
            }
            prototype::drawLaserPointer(cr, state, i);
            prototype::drawVerticalSpacePreview(cr, state, i);
            prototype::drawSelection(cr, state, i);
            if (state.brush.tool != Tool::Hand && selectedImageIndex && *selectedImageIndex < state.images.size() &&
                state.images[*selectedImageIndex].page == i &&
                state.layerVisible(i, state.images[*selectedImageIndex].layerId)) {
                prototype::drawCanvasImageHandles(cr, displayedImage(*selectedImageIndex), state.zoom);
            }
            if (pdfTextSelection && pdfTextSelection->page == i && pdfTextSelection->region) {
                const int rectangleCount = cairo_region_num_rectangles(pdfTextSelection->region.get());
                if (rectangleCount > 0) {
                    cairo_save(cr);
                    cairo_set_source_rgba(cr, 0.22, 0.50, 0.86, 0.30);
                    for (int rectangleIndex = 0; rectangleIndex < rectangleCount; ++rectangleIndex) {
                        cairo_rectangle_int_t rectangle{};
                        cairo_region_get_rectangle(pdfTextSelection->region.get(), rectangleIndex, &rectangle);
                        cairo_rectangle(cr, rectangle.x, rectangle.y, rectangle.width, rectangle.height);
                    }
                    cairo_fill(cr);
                    if (pdfTextSelection->dragging && pdfTextSelection->tool == Tool::SelectPdfTextRect) {
                        const auto& bounds = pdfTextSelection->bounds;
                        cairo_set_source_rgba(cr, 0.16, 0.39, 0.68, 0.95);
                        cairo_set_line_width(cr, 1.0 / state.zoom);
                        cairo_rectangle(cr, std::min(bounds.x1, bounds.x2), std::min(bounds.y1, bounds.y2),
                                        std::abs(bounds.x2 - bounds.x1), std::abs(bounds.y2 - bounds.y1));
                        cairo_stroke(cr);
                    }
                    cairo_restore(cr);
                }
            }
            cairo_restore(cr);
        }
        if (!drewAny) {
            ensurePageCache(state.page, deviceScale);
        }
        if (importedConfig.settings.highlightPosition && pointerPosition && pointerPosition->x >= 0 &&
            pointerPosition->y >= 0 && pointerPosition->x < width && pointerPosition->y < height) {
            const auto fill = unpackArgbColor(importedConfig.settings.cursorHighlightColor);
            cairo_save(cr);
            cairo_arc(cr, pointerPosition->x, pointerPosition->y, importedConfig.settings.cursorHighlightRadius, 0,
                      2 * G_PI);
            cairo_set_source_rgba(cr, fill.red, fill.green, fill.blue, fill.alpha);
            cairo_fill_preserve(cr);
            if (importedConfig.settings.cursorHighlightBorderWidth > 0) {
                const auto border = unpackArgbColor(importedConfig.settings.cursorHighlightBorderColor);
                cairo_set_source_rgba(cr, border.red, border.green, border.blue, border.alpha);
                cairo_set_line_width(cr, importedConfig.settings.cursorHighlightBorderWidth);
                cairo_stroke(cr);
            } else {
                cairo_new_path(cr);
            }
            cairo_restore(cr);
        }
        ++frames;
    }

    void capture(const std::string& name, GtkWidget* target = nullptr, bool captureTargetDirectly = false) {
        waitForNextWindowFrame();
        graphene_rect_t targetBounds;
        if (target && (captureTargetDirectly || !gtk_widget_compute_bounds(target, window, &targetBounds))) {
            const int targetWidth = gtk_widget_get_width(target);
            const int targetHeight = gtk_widget_get_height(target);
            if (targetWidth <= 0 || targetHeight <= 0) {
                throw SnapshotPending{};
            }
            auto* targetPaintable = gtk_widget_paintable_new(target);
            auto* targetSnapshot = gtk_snapshot_new();
            gdk_paintable_snapshot(GDK_PAINTABLE(targetPaintable), GDK_SNAPSHOT(targetSnapshot),
                                   targetWidth, targetHeight);
            auto* targetNode = gtk_snapshot_free_to_node(targetSnapshot);
            g_object_unref(targetPaintable);
            if (!targetNode) {
                throw SnapshotPending{};
            }
            const graphene_rect_t targetRenderBounds{
                    {0, 0}, {static_cast<float>(targetWidth), static_cast<float>(targetHeight)}};
            auto* targetTexture = gsk_renderer_render_texture(
                    gtk_native_get_renderer(GTK_NATIVE(window)), targetNode, &targetRenderBounds);
            const auto targetPath = options.smokeDirectory + "/" + name + ".png";
            const bool targetSaved = gdk_texture_save_to_png(targetTexture, targetPath.c_str());
            g_object_unref(targetTexture);
            gsk_render_node_unref(targetNode);
            if (!targetSaved) {
                throw std::runtime_error("Cannot write screenshot");
            }
            return;
        }
        // Snapshot the window so a crop retains ancestor backgrounds and styling.
        auto* paintable = GDK_PAINTABLE(g_object_ref(windowPaintable));
        auto* snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(window),
                               gtk_widget_get_height(window));
        // A window paintable may lag a just-queued drawing-area frame. Overlay a
        // fresh canvas snapshot so smoke captures include the state being tested.
        const bool overlayDialogVisible = messageDialog ||
                                          (preferencesDialog &&
                                           gtk_widget_get_visible(GTK_WIDGET(preferencesDialog)));
        if (!target && canvas && !overlayDialogVisible) {
            graphene_rect_t canvasBounds;
            if (gtk_widget_compute_bounds(canvas, window, &canvasBounds)) {
                auto* canvasPaintable = gtk_widget_paintable_new(canvas);
                const graphene_point_t offset{canvasBounds.origin.x, canvasBounds.origin.y};
                gtk_snapshot_save(snapshot);
                gtk_snapshot_translate(snapshot, &offset);
                gdk_paintable_snapshot(GDK_PAINTABLE(canvasPaintable), GDK_SNAPSHOT(snapshot),
                                       gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
                gtk_snapshot_restore(snapshot);
                g_object_unref(canvasPaintable);
            }
        }
        // Wayland GtkPopover renders into a child popup surface; composite it when open.
        if (!target && palettePopover && gtk_widget_get_visible(palettePopover) && paletteButton) {
            const int popW = gtk_widget_get_width(palettePopover);
            const int popH = gtk_widget_get_height(palettePopover);
            graphene_rect_t anchorBounds;
            if (popW > 0 && popH > 0 && gtk_widget_compute_bounds(paletteButton, window, &anchorBounds)) {
                auto* popPaintable = gtk_widget_paintable_new(palettePopover);
                const float px = std::clamp(anchorBounds.origin.x + 0.5f * (anchorBounds.size.width - popW), 4.0f,
                                            std::max(4.0f, static_cast<float>(gtk_widget_get_width(window) - popW - 4)));
                const float py = anchorBounds.origin.y + anchorBounds.size.height + 2.0f;
                const graphene_point_t offset{px, py};
                gtk_snapshot_save(snapshot);
                gtk_snapshot_translate(snapshot, &offset);
                gdk_paintable_snapshot(popPaintable, GDK_SNAPSHOT(snapshot), popW, popH);
                gtk_snapshot_restore(snapshot);
                g_object_unref(popPaintable);
            }
        }
        if (!target && pluginsMenuButton) {
            auto* popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(pluginsMenuButton));
            const int popW = popover ? gtk_widget_get_width(GTK_WIDGET(popover)) : 0;
            const int popH = popover ? gtk_widget_get_height(GTK_WIDGET(popover)) : 0;
            graphene_rect_t anchorBounds;
            if (popover && gtk_widget_get_visible(GTK_WIDGET(popover)) && popW > 0 && popH > 0 &&
                gtk_widget_compute_bounds(pluginsMenuButton, window, &anchorBounds)) {
                auto* popPaintable = gtk_widget_paintable_new(GTK_WIDGET(popover));
                const float px =
                        std::clamp(anchorBounds.origin.x + 0.5f * (anchorBounds.size.width - popW), 4.0f,
                                   std::max(4.0f, static_cast<float>(gtk_widget_get_width(window) - popW - 4)));
                const float py = anchorBounds.origin.y + anchorBounds.size.height + 2.0f;
                const graphene_point_t offset{px, py};
                gtk_snapshot_save(snapshot);
                gtk_snapshot_translate(snapshot, &offset);
                gdk_paintable_snapshot(GDK_PAINTABLE(popPaintable), GDK_SNAPSHOT(snapshot), popW, popH);
                gtk_snapshot_restore(snapshot);
                g_object_unref(popPaintable);
            }
        }
        auto* node = gtk_snapshot_free_to_node(snapshot);
        if (!node) {
            g_object_unref(paintable);
            throw SnapshotPending{};
        }
        graphene_rect_t bounds;
        const bool crop = target && gtk_widget_compute_bounds(target, window, &bounds);
        auto* texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node,
                                                    crop ? &bounds : nullptr);
        const auto path = options.smokeDirectory + "/" + name + ".png";
        const bool saved = gdk_texture_save_to_png(texture, path.c_str());
        g_object_unref(texture);
        gsk_render_node_unref(node);
        g_object_unref(paintable);
        if (!saved) {
            throw std::runtime_error("Cannot write screenshot");
        }
    }

    void waitForNextPaint() {
        const int previousPaints = paints;
        gtk_widget_queue_draw(canvas);
        const gint64 deadline = g_get_monotonic_time() + 500000;
        while (paints == previousPaints && g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(paints > previousPaints, "Canvas did not paint the requested demo state");
    }

    void waitForNextWindowFrame() {
        auto* frameClock = gtk_widget_get_frame_clock(window);
        require(frameClock != nullptr, "Window does not have a frame clock");
        const auto previousFrame = gdk_frame_clock_get_frame_counter(frameClock);
        gtk_widget_queue_draw(window);
        const gint64 deadline = g_get_monotonic_time() + 500000;
        while (gdk_frame_clock_get_frame_counter(frameClock) <= previousFrame &&
               g_get_monotonic_time() < deadline) {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        require(gdk_frame_clock_get_frame_counter(frameClock) > previousFrame,
                "Window did not render the requested screenshot frame");
    }

    static void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    void checkInputParity(double x, double y) {
        const auto count = state.ink.size();
        begin(Device::Stylus, x, y + 100, 0.2);
        std::array<GdkTimeCoord, 2> history{};
        for (size_t i = 0; i < history.size(); ++i) {
            history[i].flags = GdkAxisFlags(GDK_AXIS_FLAG_X | GDK_AXIS_FLAG_Y);
            history[i].axes[GDK_AXIS_X] = x + 20 + 20 * i;
            history[i].axes[GDK_AXIS_Y] = y + 100;
        }
        history[0].flags = GdkAxisFlags(history[0].flags | GDK_AXIS_FLAG_PRESSURE);
        history[0].axes[GDK_AXIS_PRESSURE] = 0.6;
        consumeBacklog(history);
        g_signal_emit_by_name(stylus, "motion", x + 60, y + 100);
        g_signal_emit_by_name(stylus, "up", x + 1000, y + 1000);
        require(state.ink.size() == count + 1 && state.ink.back().points.size() == 4 &&
                        state.ink.back().points.back().pressure == 0.6,
                "Stylus backlog/release lost samples or added a hover tail");
        state.undo();

        begin(Device::Stylus, x + 50, y + 15, 0.5, true);
        g_signal_emit_by_name(stylus, "up", x + 50, y + 15);
        require(state.ink.size() == count + 1 && state.brush.tool == Tool::Pen,
                "Stylus eraser end failed to split stroke or changed toolbar selection");
        state.undo();

        state.eraserMode = prototype::EraserMode::Whiteout;
        begin(Device::Stylus, x + 20, y + 15, 0.5, true);
        g_signal_emit_by_name(stylus, "motion", x + 60, y + 15);
        g_signal_emit_by_name(stylus, "up", x + 60, y + 15);
        require(state.ink.size() == count + 1 && state.ink.back().brush.tool == Tool::Eraser &&
                        state.brush.tool == Tool::Pen,
                "Stylus eraser end in whiteout mode failed or changed toolbar selection");
        state.undo();

        state.eraserMode = prototype::EraserMode::DeleteStroke;
        begin(Device::Stylus, x, y, 0.5, true);
        g_signal_emit_by_name(stylus, "up", x, y);
        require(state.ink.size() == count - 1 && state.brush.tool == Tool::Pen,
                "Stylus eraser end in delete-stroke mode failed or changed toolbar selection");
        state.undo();
        state.eraserMode = prototype::EraserMode::Standard;

        const auto oldX = state.offsetX;
        const auto oldY = state.offsetY;
        gboolean handled = false;
        g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_space, 0u, GdkModifierType(0), &handled);
        require(handled && input.spaceHeld, "Space key did not enable temporary hand tool");
        g_signal_emit_by_name(mouse, "drag-begin", x, y);
        g_signal_emit_by_name(mouse, "drag-update", 10.0, 5.0);
        g_signal_emit_by_name(keys, "key-released", GDK_KEY_space, 0u, GdkModifierType(0));
        g_signal_emit_by_name(mouse, "drag-update", 20.0, 10.0);
        g_signal_emit_by_name(mouse, "drag-end", 400.0, 300.0);
        require(!input.active() && !input.spaceHeld && state.ink.size() == count &&
                        std::abs(state.offsetX - oldX - 20) < 0.001 && std::abs(state.offsetY - oldY - 10) < 0.001,
                "Space drag drew ink, jumped on release, or left the hand tool stuck");
        state.offsetX = oldX;
        state.offsetY = oldY;
        g_signal_emit_by_name(mouse, "drag-begin", 2.0, 2.0);
        require(input.panning(), "A drag in the page margin must use the hand tool");
        g_signal_emit_by_name(mouse, "drag-update", 8.0, 6.0);
        g_signal_emit_by_name(mouse, "drag-end", 100.0, 100.0);
        require(state.ink.size() == count && std::abs(state.offsetX - oldX - 8) < 0.001,
                "Margin drag unexpectedly drew or jumped on release");
        state.offsetX = oldX;
        state.offsetY = oldY;
        update();
    }

    void smokeDrawingTools() {
        const auto before = state.ink.size();
        selectTool(Tool::Pen);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                                   static_cast<guint>(prototype::DrawingMode::Freehand));
        gtk_menu_button_popup(GTK_MENU_BUTTON(paletteButton));
        gtk_expander_set_expanded(GTK_EXPANDER(drawingOptionsExpander), true);
        waitForNextPaint();
        require(gtk_widget_get_visible(palettePopover), "Drawing controls popover did not open");
        capture("drawing-controls");
        gtk_popover_popdown(GTK_POPOVER(palettePopover));

        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                                   static_cast<guint>(prototype::DrawingMode::Rectangle));
        const auto start = state.pageScreenPoint(0, 190, 230);
        g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
        gboolean handled = false;
        g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Shift_L, 0u, GdkModifierType(0), &handled);
        g_signal_emit_by_name(mouse, "drag-update", 60.0 * state.zoom, 35.0 * state.zoom);
        require(state.current && state.current->points.size() == 5 &&
                        std::abs(std::abs(state.current->points[2].x - state.current->points[0].x) -
                                 std::abs(state.current->points[2].y - state.current->points[0].y)) < 1e-6,
                "Toolbar rectangle and live Shift constraint did not update the preview");
        g_signal_emit_by_name(mouse, "drag-end", 60.0 * state.zoom, 35.0 * state.zoom);
        g_signal_emit_by_name(keys, "key-released", GDK_KEY_Shift_L, 0u, GdkModifierType(0));
        require(state.ink.size() == before + 1 && state.ink.back().points.size() == 5,
                "Constrained rectangle did not commit through the canvas input path");
        waitForNextPaint();
        capture("shape-tools");
        state.undo();

        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                                   static_cast<guint>(prototype::DrawingMode::ShapeRecognizer));
        const double centerX = 300, centerY = 330, radius = 54;
        const auto first = state.pageScreenPoint(0, centerX + radius, centerY);
        begin(Device::Mouse, first.x, first.y);
        for (int i = 1; i <= 48; ++i) {
            const double angle = 2.0 * M_PI * i / 48.0;
            const auto next = state.pageScreenPoint(0, centerX + radius * std::cos(angle),
                                                    centerY + radius * std::sin(angle));
            input.motion(Device::Mouse, {next.x, next.y, std::nullopt});
        }
        finish(Device::Mouse);
        require(state.ink.size() == before + 1 && state.ink.back().points.size() > 20,
                "Shape recognizer did not replace the circular stroke in the GTK smoke path");
        waitForNextPaint();
        capture("shape-recognized");
        state.undo();
        require(state.ink.size() == before, "Shape tool smoke operations did not undo cleanly");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                                   static_cast<guint>(prototype::DrawingMode::Spline));
        auto firstKnot = state.pageScreenPoint(0, 400, 260);
        g_signal_emit_by_name(textClick, "pressed", 1, firstKnot.x, firstKnot.y);
        g_signal_emit_by_name(textClick, "released", 1, firstKnot.x, firstKnot.y);
        auto secondKnot = state.pageScreenPoint(0, 460, 275);
        g_signal_emit_by_name(textClick, "pressed", 1, secondKnot.x, secondKnot.y);
        auto tangentPoint = state.pageScreenPoint(0, 475, 305);
        g_signal_emit_by_name(pointerMotion, "motion", tangentPoint.x, tangentPoint.y);
        g_signal_emit_by_name(textClick, "released", 1, tangentPoint.x, tangentPoint.y);
        require(input.splineInProgress && state.current && state.current->points.size() > 2,
                "Toolbar spline clicks and tangent drag did not update the live path");
        g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Escape, 0u, GdkModifierType(0), &handled);
        require(!input.splineInProgress && state.ink.size() == before + 1 && state.ink.back().points.size() > 2,
                "Escape did not finalize the interactive spline through the GTK key path");
        waitForNextPaint();
        capture("spline-tools");
        state.undo();
        require(state.ink.size() == before, "Interactive spline should commit as a single undo action");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drawingModeDropdown),
                                   static_cast<guint>(prototype::DrawingMode::Freehand));
        update();
    }

    gboolean stepCrashHelper() {
        require(++crashHelperTicks < 240, "Crash helper timed out waiting for autosave");
        if (busy || !frames || pageCache.empty() || !nativeSession) {
            return G_SOURCE_CONTINUE;
        }
        if (crashHelperStep == 0) {
            const auto x = state.offsetX + 90 * state.zoom;
            const auto y = state.offsetY + 120 * state.zoom;
            begin(Device::Stylus, x, y, 0.25);
            motion(Device::Stylus, x + 50 * state.zoom, y + 25 * state.zoom, 0.75);
            finish(Device::Stylus, 0.75);
            require(modified() && state.ink.size() == 1 && state.ink[0].pressureSensitive,
                    "Crash helper did not record pressure-sensitive stroke");
            crashHelperStep = 1;
            return G_SOURCE_CONTINUE;
        }
        if (autosaving || autosavedRevision != state.revision() || activeAutosavePath.empty()) {
            return G_SOURCE_CONTINUE;
        }
        require(std::filesystem::is_regular_file(activeAutosavePath), "Crash helper autosave file missing");
        const auto temporary = crashMarkerPath + ".tmp";
        {
            std::ofstream marker(temporary, std::ios::binary);
            marker << activeAutosavePath << '\n';
            marker.close();
            require(bool(marker), "Cannot write crash helper marker");
        }
        std::filesystem::rename(temporary, crashMarkerPath);
        crashHelperSource = 0;
        return G_SOURCE_REMOVE;
    }

    gboolean stepRecoveryVerify() {
        require(++recoveryVerifyTicks < 240, "Recovery verifier timed out");
        if (busy || deferredAction) {
            return G_SOURCE_CONTINUE;
        }
        if (recoveryVerifyStep == 0) {
            if (!messageDialog) {
                return G_SOURCE_CONTINUE;
            }
            if (recoveryActionSpec.starts_with("restore:")) {
                answerDialog("restore");
            } else if (recoveryActionSpec == "delete") {
                answerDialog("delete");
            } else if (recoveryActionSpec == "recover") {
                answerDialog("recover");
            } else if (recoveryActionSpec == "discard") {
                answerDialog("discard");
            } else {
                throw std::runtime_error("Unknown XOURNALPP_RECOVERY_ACTION");
            }
            recoveryVerifyStep = 1;
            return G_SOURCE_CONTINUE;
        }
        if (recoveryVerifyStep == 1) {
            if (!frames || pageCache.empty() || g_get_monotonic_time() - lastDocumentCompletion < 150000) {
                return G_SOURCE_CONTINUE;
            }
            if (recoveryActionSpec == "delete" || recoveryActionSpec == "discard") {
                require(!modified() && activeAutosavePath.empty(),
                        "Expected clean state after deleting/discarding recovery file");
                closeWindow();
                recoveryVerifySource = 0;
                return G_SOURCE_REMOVE;
            }
            if (recoveryActionSpec.starts_with("restore:")) {
                require(modified() && recoveredChanges && saveTarget.empty() && !activeAutosavePath.empty() &&
                                std::filesystem::is_regular_file(activeAutosavePath),
                        "Expected unnamed dirty restored document with adopted cache autosave");
                smokeSaveChoice = recoveryActionSpec.substr(std::string_view("restore:").size());
                save([this] { closeWindow(); });
            } else {
                require(modified() && recoveredChanges && !saveTarget.empty() && !activeAutosavePath.empty() &&
                                std::filesystem::is_regular_file(activeAutosavePath),
                        "Expected associated dirty recovered document");
                save([this] { closeWindow(); });
            }
            recoveryVerifyStep = 2;
            return G_SOURCE_CONTINUE;
        }
        if (!closed) {
            return G_SOURCE_CONTINUE;
        }
        recoveryVerifySource = 0;
        return G_SOURCE_REMOVE;
    }

    gboolean smokeLayers() {
        switch (layerVerifyStep++) {
            case 0: {
                require(nativeSession && pageCount == 2 && state.layersForPage(0).layers.size() == 2,
                        "Layer verification requires a native two-layer document");
                layerVerifyInitialInk = state.ink.size();
                require(state.layersForPage(0).layers[0].name == "Lower" &&
                                state.layersForPage(0).layers[1].name == "Upper",
                        "Native layer names were not loaded into the sidebar model");
                capture("layer-stack");
                auto* upper = findLayerControl(2, false);
                require(upper, "Upper layer is missing from the layer sidebar");
                g_signal_emit_by_name(upper, "clicked");
                require(state.activeLayerId(0) == 2, "Clicking a layer row did not select it");
                g_signal_emit_by_name(layerAddButton, "clicked");
                require(state.layersForPage(0).layers.size() == 3 && state.activeLayerId(0) == 3 &&
                                state.activeLayerOrigin(0) == 2,
                        "Add layer did not insert above the selected native layer");
                break;
            }
            case 1: {
                capture("layer-added");
                require(gtk_widget_get_sensitive(layerMergeButton), "Merge down was unavailable for the top layer");
                g_signal_emit_by_name(layerMergeButton, "clicked");
                require(state.layersForPage(0).layers.size() == 2 && state.activeLayerId(0) == 2 &&
                                state.layersForPage(0).layers[1].origins == std::vector<size_t>({1, 2}),
                        "Merge down did not preserve source origins in bottom-to-top order");
                break;
            }
            case 2: {
                capture("layer-merged");
                auto* upperVisibility = findLayerControl(2, true);
                require(upperVisibility, "Upper layer visibility control is missing");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(upperVisibility), false);
                require(!state.layerVisible(0, 2), "Visibility control did not hide the selected layer");
                break;
            }
            case 3: {
                capture("layer-hidden");
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.layerVisible(0, 2), "Undo did not restore layer visibility");
                const auto start = state.pageScreenPoint(0, 120, 120);
                const auto end = state.pageScreenPoint(0, 170, 120);
                g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
                require(state.ink.size() == layerVerifyInitialInk + 1 && state.ink.back().layerId == 2 &&
                                state.ink.back().originLayer == 2,
                        "New ink did not follow the active merged layer origin");
                g_signal_emit_by_name(layerRenameBackgroundButton, "clicked");
                require(messageDialog && layerRenameEntry, "Rename Background did not open its dialog");
                break;
            }
            case 4: {
                gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), "Paper");
                answerDialog("rename");
                require(state.layersForPage(0).backgroundName == "Paper",
                        "Renaming the background did not update its explicit name");
                break;
            }
            case 5: {
                capture("layer-background-renamed");
                g_signal_emit_by_name(layerRenameButton, "clicked");
                require(messageDialog && layerRenameEntry, "Rename Layer did not open its dialog");
                break;
            }
            case 6: {
                gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), "Upper renamed");
                answerDialog("rename");
                require(state.layersForPage(0).layers[1].name == "Upper renamed",
                        "Renaming the selected layer did not update its name");
                break;
            }
            case 7: {
                capture("layer-renamed");
                g_signal_emit_by_name(layerDuplicateButton, "clicked");
                layerVerifyDuplicateInkStart = state.ink.size() - 2;
                require(state.layersForPage(0).layers.size() == 3 && state.activeLayerId(0) == 3 &&
                                state.layersForPage(0).layers[1].origins == std::vector<size_t>({1, 2}) &&
                                state.layersForPage(0).layers[2].origins == std::vector<size_t>({1, 2}) &&
                                state.ink.size() == layerVerifyInitialInk + 3 &&
                                state.ink[layerVerifyDuplicateInkStart].layerId == 3 &&
                                state.ink[layerVerifyDuplicateInkStart + 1].layerId == 3,
                        "Duplicating a merged layer did not copy its origins and editable ink");
                g_signal_emit_by_name(layerRenameButton, "clicked");
                require(messageDialog && layerRenameEntry, "Renaming a duplicate did not open its dialog");
                break;
            }
            case 8: {
                gtk_editable_set_text(GTK_EDITABLE(layerRenameEntry), "Duplicate");
                answerDialog("rename");
                break;
            }
            case 9: {
                capture("layer-duplicated");
                g_signal_emit_by_name(layerMoveDownButton, "clicked");
                require(state.activeLayerId(0) == 2 && state.layersForPage(0).layers[1].name == "Duplicate" &&
                                state.ink[layerVerifyDuplicateInkStart].layerId == 2 &&
                                state.ink[layerVerifyDuplicateInkStart + 1].layerId == 2,
                        "Moving the duplicate down did not update stack order and object IDs");
                break;
            }
            case 10: {
                capture("layer-moved-down");
                g_signal_emit_by_name(layerMoveUpButton, "clicked");
                require(state.activeLayerId(0) == 3 && state.layersForPage(0).layers[2].name == "Duplicate" &&
                                state.ink[layerVerifyDuplicateInkStart].layerId == 3 &&
                                state.ink[layerVerifyDuplicateInkStart + 1].layerId == 3,
                        "Moving the duplicate up did not restore stack order and object IDs");
                break;
            }
            case 11: {
                g_signal_emit_by_name(layerHideAllButton, "clicked");
                require(!state.layerVisible(0, 1) && !state.layerVisible(0, 2) && !state.layerVisible(0, 3),
                        "Hide All Layers did not hide every foreground layer");
                break;
            }
            case 12: {
                capture("layers-hidden-all");
                g_signal_emit_by_name(layerShowAllButton, "clicked");
                require(state.layerVisible(0, 1) && state.layerVisible(0, 2) && state.layerVisible(0, 3),
                        "Show All Layers did not show every foreground layer");
                break;
            }
            case 13: {
                g_signal_emit_by_name(undoButton, "clicked");
                require(!state.layerVisible(0, 1) && !state.layerVisible(0, 2) && !state.layerVisible(0, 3),
                        "Undo did not restore the prior all-hidden state");
                g_signal_emit_by_name(redoButton, "clicked");
                require(state.layerVisible(0, 1) && state.layerVisible(0, 2) && state.layerVisible(0, 3),
                        "Redo did not restore the all-visible state");
                break;
            }
            case 14: {
                capture("layers-shown-all");
                const auto saved = (std::filesystem::path(options.smokeDirectory) / "layer-saved.xopp").string();
                saveCopyTo(saved);
                require(busy, "Saving the layer verification document did not start");
                break;
            }
            default: {
                const auto saved = std::filesystem::path(options.smokeDirectory) / "layer-saved.xopp";
                require(!modified() && savedCopyPath == saved.string() && std::filesystem::is_regular_file(saved),
                        "Layer-aware save did not complete cleanly");
                capture("layer-saved");
                std::cout << "PASS: GTK layer selection, add, rename, duplicate, reorder, visibility, merge, undo and save\n";
                closeWindow();
                return G_SOURCE_REMOVE;
            }
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    gboolean smokeSelectionModes() {
        switch (selectionVerifyStep++) {
            case 0: {
                require(nativeSession && pageCount == 1 && state.layersForPage(0).layers.size() == 2 &&
                                state.ink.size() == 4 && state.texts.size() == 1 &&
                                state.texts[0].layoutWidth > 0 && state.texts[0].layoutHeight > 0 &&
                                state.layersForPage(0).layers[0].name == "Lower" &&
                                state.layersForPage(0).layers[1].name == "Upper",
                        "Visible-layer selection verification requires its native two-layer fixture");
                require(g_list_model_get_n_items(gtk_drop_down_get_model(GTK_DROP_DOWN(selectionModeDropdown))) == 9,
                        "Selection mode menu does not expose visible-layer, object, PDF text and vertical-space modes");
                activateSelection(Tool::SelectRectangle);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
                require(state.brush.tool == Tool::SelectRectangle && state.selectionAcrossLayers,
                        "Visible-layer rectangle mode did not activate multilayer selection");
                state.clearSelection();
                {
                    const auto start = state.pageScreenPoint(0, 45, 95);
                    const auto end = state.pageScreenPoint(0, 270, 230);
                    g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                    g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                    g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
                }
                require(state.selected == std::vector<size_t>({2, 3}),
                        "Visible-layer rectangle did not select all matching strokes from the upper layer only");
                require(state.selectedTexts == std::vector<size_t>{0},
                        "Visible-layer rectangle did not include native text from the matching upper layer");
                break;
            }
            case 1: {
                capture("selection-visible-layer-rectangle");
                state.clearSelection();
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 4);
                require(state.brush.tool == Tool::SelectLasso && state.selectionAcrossLayers,
                        "Visible-layer lasso mode did not activate multilayer selection");
                const auto start = state.pageScreenPoint(0, 45, 95);
                const std::array<prototype::Point, 4> polygon{{{270, 95}, {270, 230}, {45, 230}, {45, 95}}};
                const auto end = state.pageScreenPoint(0, polygon.back().x, polygon.back().y);
                g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                for (const auto& point: polygon) {
                    const auto screen = state.pageScreenPoint(0, point.x, point.y);
                    g_signal_emit_by_name(mouse, "drag-update", screen.x - start.x, screen.y - start.y);
                }
                g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
                require(state.selected == std::vector<size_t>({2, 3}),
                        "Visible-layer lasso did not select all matching strokes from the upper layer only");
                require(state.selectedTexts == std::vector<size_t>{0},
                        "Visible-layer lasso did not include native text from the matching upper layer");
                break;
            }
            case 2: {
                capture("selection-visible-layer-lasso");
                state.clearSelection();
                auto* upperVisibility = findLayerControl(2, true);
                require(upperVisibility, "Upper-layer visibility control is missing");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(upperVisibility), false);
                require(!state.layerVisible(0, 2), "Upper layer did not become hidden before fallback selection");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
                const auto start = state.pageScreenPoint(0, 45, 95);
                const auto end = state.pageScreenPoint(0, 270, 230);
                g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
                require(state.selected == std::vector<size_t>({0, 1}),
                        "Visible-layer rectangle did not fall back to the next visible matching layer");
                require(state.selectedTexts.empty(),
                        "Hidden upper-layer text leaked into the visible-layer fallback selection");
                break;
            }
            case 3: {
                capture("selection-hidden-layer-fallback");
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.layerVisible(0, 2), "Undo did not restore the upper layer visibility");
                state.clearSelection();
                {
                    const auto start = state.pageScreenPoint(0, 68, 206);
                    const auto end = state.pageScreenPoint(0, 270, 230);
                    g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                    g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                    g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
                }
                require(state.selected.empty() && state.selectedTexts == std::vector<size_t>{0},
                        "Rectangle selection did not isolate the native text object");
                selectionVerifyTextX = state.texts[0].x;
                break;
            }
            case 4: {
                capture("selection-text-object");
                {
                    const auto start = state.pageScreenPoint(0, 95, 213);
                    const auto end = state.pageScreenPoint(0, 113, 223);
                    g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
                    g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
                    g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
                }
                require(std::abs(state.texts[0].x - (selectionVerifyTextX + 18)) < 1e-6 &&
                                std::abs(state.texts[0].y - 218) < 1e-6 &&
                                state.selectedTexts == std::vector<size_t>{0},
                        "Dragging selected native text did not move it while preserving selection");
                break;
            }
            case 5: {
                capture("selection-text-moved");
                g_signal_emit_by_name(undoButton, "clicked");
                require(std::abs(state.texts[0].x - selectionVerifyTextX) < 1e-6 &&
                                std::abs(state.texts[0].y - 208) < 1e-6 &&
                                state.selectedTexts == std::vector<size_t>{0},
                        "Undo did not restore the native text position and selection");

                auto* upperLayer = findLayerControl(2, false);
                require(upperLayer, "Upper layer selector is missing for Text-tool verification");
                g_signal_emit_by_name(upperLayer, "clicked");

                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 2);
                require(state.brush.tool == Tool::SmartSelect && !state.selectionAcrossLayers,
                        "Smart Select was not available for native hit testing");
                state.clearSelection();
                const bool oldSnapGridEnabled = state.snapGridEnabled;
                const double oldSnapGridSize = state.snapGridSize;
                const double oldSnapGridTolerance = state.snapGridTolerance;
                state.snapGridEnabled = false;
                const auto edgeTextBefore = state.texts[0];
                const auto edgeStart = state.pageScreenPoint(
                        0, edgeTextBefore.x + edgeTextBefore.layoutWidth + 0.25,
                        edgeTextBefore.y + edgeTextBefore.layoutHeight / 2);
                begin(Device::Mouse, edgeStart.x, edgeStart.y);
                require(state.movingSelection() && state.selectedTexts == std::vector<size_t>{0},
                        "Smart Select missed native text within the 0.5 point edge hit radius");
                motion(Device::Mouse, edgeStart.x + 10 * state.zoom, edgeStart.y);
                finish(Device::Mouse, std::nullopt,
                       prototype::Point{edgeStart.x + 10 * state.zoom, edgeStart.y});
                require(std::abs(state.texts[0].x - edgeTextBefore.x - 10) < 1e-6 &&
                                state.selectedTexts == std::vector<size_t>{0},
                        "Smart Select edge hit did not move the native text object");
                waitForNextPaint();
                capture("smart-select-edge-hit");
                state.undo();
                require(std::abs(state.texts[0].x - edgeTextBefore.x) < 1e-6,
                        "Undo did not restore the Smart Select edge-hit move");

                state.snapGridEnabled = true;
                state.snapGridSize = 10;
                state.snapGridTolerance = 0.5;
                const auto selectionBounds = state.selectionBox();
                require(selectionBounds && selectionBounds->page == 0,
                        "Smart Select grid-snap verification lost the text selection");
                const auto oldGridXOffset = state.pageLayers[0].gridXOffset;
                const auto oldGridYOffset = state.pageLayers[0].gridYOffset;
                const auto oldGridXStep = state.pageLayers[0].gridXStep;
                const auto oldGridYStep = state.pageLayers[0].gridYStep;
                const auto gridStart = state.pageScreenPoint(
                        0, selectionBounds->x1 - 7, selectionBounds->y0 +
                                   (selectionBounds->y1 - selectionBounds->y0) / 4);
                const auto gridAnchorX = selectionBounds->x1;
                const auto gridAnchorY = selectionBounds->y0;
                state.pageLayers[0].gridXOffset = gridAnchorX + 10;
                state.pageLayers[0].gridYOffset = gridAnchorY + 10;
                state.pageLayers[0].gridXStep = 0;
                state.pageLayers[0].gridYStep = 0;
                begin(Device::Mouse, gridStart.x, gridStart.y);
                motion(Device::Mouse, gridStart.x + 11 * state.zoom, gridStart.y + 11 * state.zoom);
                finish(Device::Mouse, std::nullopt,
                       prototype::Point{gridStart.x + 11 * state.zoom, gridStart.y + 11 * state.zoom});
                require(std::abs(state.texts[0].x - edgeTextBefore.x - 10) < 1e-6 &&
                                std::abs(state.texts[0].y - edgeTextBefore.y - 10) < 1e-6,
                        "Smart Select move did not snap its nearest selection corner to the page grid");
                waitForNextPaint();
                capture("smart-select-grid-snapped");
                state.undo();
                state.pageLayers[0].gridXOffset = oldGridXOffset;
                state.pageLayers[0].gridYOffset = oldGridYOffset;
                state.pageLayers[0].gridXStep = oldGridXStep;
                state.pageLayers[0].gridYStep = oldGridYStep;
                state.snapGridEnabled = oldSnapGridEnabled;
                state.snapGridSize = oldSnapGridSize;
                state.snapGridTolerance = oldSnapGridTolerance;
                require(std::abs(state.texts[0].x - edgeTextBefore.x) < 1e-6 &&
                                std::abs(state.texts[0].y - edgeTextBefore.y) < 1e-6,
                        "Undo did not restore the Smart Select grid-snapped move");

                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
                require(state.brush.tool == Tool::SelectRectangle && state.selectionAcrossLayers,
                        "Restoring the remembered visible-layer selection mode failed");
                selectTool(Tool::Text);
                require(state.brush.tool == Tool::Text &&
                                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(textButton)) &&
                                std::none_of(toolButtons.begin(), toolButtons.end(), [](GtkWidget* button) {
                                    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
                                }) &&
                                !gtk_widget_get_visible(selectionModeDropdown),
                        "Selecting the Text tool left another tool selected in the toolbar");
                const auto originalText = state.texts[0];
                const auto editPoint = state.pageScreenPoint(0, originalText.x + originalText.layoutWidth / 2,
                                                             originalText.y + originalText.layoutHeight / 2);
                g_signal_emit_by_name(textClick, "released", 1, editPoint.x, editPoint.y);
                require(textEditor->active() && gtk_widget_get_visible(textFontEntry) &&
                                gtk_widget_get_visible(textFormatBox) && gtk_widget_get_visible(colorBox),
                        "Text editing did not expose the quick-format toolbar controls");
                const GdkRGBA textColor{208.0f / 255, 32.0f / 255, 16.0f / 255, 1};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &textColor);
                finishTextEditing();
                require(state.texts[0].color == 0xffd02010U && state.selectedTexts == std::vector<size_t>{0},
                        "Text color editing did not persist or leave the edited text selected");
                waitForNextPaint();
                capture("text-tool-edited");

                const auto beforeMove = state.texts[0];
                const auto moveStart = state.pageScreenPoint(0, beforeMove.x + beforeMove.layoutWidth / 2,
                                                             beforeMove.y + beforeMove.layoutHeight / 2);
                g_signal_emit_by_name(textClick, "pressed", 1, moveStart.x, moveStart.y);
                g_signal_emit_by_name(mouse, "drag-begin", moveStart.x, moveStart.y);
                require(state.movingSelection() && state.selectedTexts == std::vector<size_t>{0},
                        "Text-tool drag did not begin moving the edited text");
                g_signal_emit_by_name(mouse, "drag-update", 18.0 * state.zoom, 10.0 * state.zoom);
                g_signal_emit_by_name(mouse, "drag-end", 18.0 * state.zoom, 10.0 * state.zoom);
                g_signal_emit_by_name(textClick, "released", 1, moveStart.x, moveStart.y);
                require(!textEditor->active() && std::abs(state.texts[0].x - beforeMove.x - 18) < 1e-6 &&
                                std::abs(state.texts[0].y - beforeMove.y - 10) < 1e-6 &&
                                state.selectedTexts == std::vector<size_t>{0},
                        "Text-tool drag did not move the object or suppressed click-to-edit incorrectly");
                waitForNextPaint();
                capture("text-tool-moved");

                const auto beforeResize = state.texts[0];
                const auto handle = state.selectionHandles()[4];
                const auto resizeStart = state.pageScreenPoint(0, handle.x, handle.y);
                g_signal_emit_by_name(textClick, "pressed", 1, resizeStart.x, resizeStart.y);
                g_signal_emit_by_name(mouse, "drag-begin", resizeStart.x, resizeStart.y);
                require(state.movingSelection(), "Text-tool corner drag did not start a resize");
                g_signal_emit_by_name(mouse, "drag-update", 18.0 * state.zoom, 18.0 * state.zoom);
                g_signal_emit_by_name(mouse, "drag-end", 18.0 * state.zoom, 18.0 * state.zoom);
                g_signal_emit_by_name(textClick, "released", 1, resizeStart.x, resizeStart.y);
                require(std::abs(state.texts[0].xx - beforeResize.xx) > 1e-5 &&
                                state.selectedTexts == std::vector<size_t>{0},
                        "Text-tool corner drag did not resize text while keeping the selection active");
                waitForNextPaint();
                capture("text-tool-resized");

                const auto stylusTap = state.pageScreenPoint(0, state.texts[0].x + state.texts[0].layoutWidth / 2,
                                                             state.texts[0].y + state.texts[0].layoutHeight / 2);
                begin(Device::Stylus, stylusTap.x, stylusTap.y);
                require(pendingStylusTextTap.has_value(), "A stylus tap did not wait for click-versus-drag routing");
                finish(Device::Stylus, std::nullopt, stylusTap);
                require(textEditor->active(), "A stylus tap on text did not open the inline editor");
                const auto tappedText = textEditor->finish();
                require(tappedText && tappedText->existingIndex == std::optional<size_t>{0},
                        "Stylus text editing lost the existing object identity");
                updateTextToolbarVisibility();

                const auto beforeStylusMove = state.texts[0];
                const auto stylusStart = state.pageScreenPoint(0, state.texts[0].x + state.texts[0].layoutWidth / 2,
                                                               state.texts[0].y + state.texts[0].layoutHeight / 2);
                begin(Device::Stylus, stylusStart.x, stylusStart.y);
                motion(Device::Stylus, stylusStart.x + 14.0 * state.zoom, stylusStart.y + 8.0 * state.zoom);
                require(state.movingSelection(), "A stylus drag did not begin moving text with the Text tool");
                finish(Device::Stylus, std::nullopt,
                       prototype::Point{stylusStart.x + 14.0 * state.zoom, stylusStart.y + 8.0 * state.zoom});
                require(std::abs(state.texts[0].x - beforeStylusMove.x - 14) < 1e-6 &&
                                std::abs(state.texts[0].y - beforeStylusMove.y - 8) < 1e-6 &&
                                state.selectedTexts == std::vector<size_t>{0},
                        "Stylus text drag did not commit with the text selected");
                waitForNextPaint();
                capture("text-tool-stylus-moved");

                gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), 18);
                require(state.texts[0].fontSize == 18,
                        "The font-size toolbar control did not update selected text");
                g_signal_emit_by_name(textFormatButtons[0], "clicked");
                require(!state.texts[0].styleRuns.empty(),
                        "The style toolbar did not format selected text outside the inline editor");
                const GdkRGBA selectedColor{32.0f / 255, 80.0f / 255, 210.0f / 255, 1};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &selectedColor);
                require(state.texts[0].color == 0xff2050d2U,
                        "The color toolbar did not recolor a selected text object");
                state.undo();
                state.undo();
                state.undo();
                state.undo();
                state.undo();
                state.undo();
                state.undo();
                require(state.texts[0].color == originalText.color && state.texts[0].fontSize == originalText.fontSize &&
                                std::abs(state.texts[0].x - originalText.x) < 1e-6 &&
                                std::abs(state.texts[0].y - originalText.y) < 1e-6 &&
                                std::abs(state.texts[0].xx - originalText.xx) < 1e-6 &&
                                state.texts[0].styleRuns == originalText.styleRuns,
                        "Undo did not restore the edited text, move, resize and toolbar formatting sequence");

                selectTool(Tool::Pen);
                require(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(textButton)),
                        "Switching away from Text left the Text tool selected in the header");
                require(state.selectionAcrossLayers, "Changing drawing tools discarded the remembered selection mode");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
                require(state.brush.tool == Tool::SmartSelect,
                        "The main selection tool button did not reopen Smart Select");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 3);
                require(state.brush.tool == Tool::SelectRectangle && state.selectionAcrossLayers &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 3,
                        "The selection mode menu did not restore the visible-layer rectangle mode");
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_r, 0u,
                                      GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
                require(handled && state.brush.tool == Tool::SelectRectangle && !state.selectionAcrossLayers &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 0,
                        "The rectangle keyboard shortcut did not select the regular single-layer tool");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 4);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 2);
                require(state.brush.tool == Tool::SmartSelect && !state.selectionAcrossLayers &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 2,
                        "Smart Select incorrectly retained the multilayer scope");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 5);
                require(state.brush.tool == Tool::SelectObject && !state.selectionAcrossLayers &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 5,
                        "Select Object mode did not activate as a distinct tool");
                selectTool(Tool::Pen);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
                require(state.brush.tool == Tool::SmartSelect &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(selectionModeDropdown)) == 2,
                        "The primary selection button did not return to Smart Select");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 5);
                std::vector<size_t> activeStrokeIndices;
                for (size_t index = 0; index < state.ink.size(); ++index) {
                    if (state.ink[index].page == 0 && state.ink[index].layerId == state.activeLayerId(0) &&
                        state.layerVisible(0, state.ink[index].layerId)) {
                        activeStrokeIndices.push_back(index);
                    }
                }
                require(activeStrokeIndices.size() >= 2,
                        "Select Object click verification needs two strokes on the active layer");
                const auto firstStroke = activeStrokeIndices[0];
                const auto secondStroke = activeStrokeIndices[1];
                const auto firstBefore = state.ink[firstStroke].points;
                const auto clickStroke = [&](size_t index) {
                    const auto& stroke = state.ink[index];
                    const auto& anchor = stroke.points[stroke.points.size() / 2];
                    const auto screen = state.pageScreenPoint(stroke.page, anchor.x, anchor.y);
                    g_signal_emit_by_name(mouse, "drag-begin", screen.x, screen.y);
                    g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
                };
                state.clearSelection();
                clickStroke(firstStroke);
                require(state.selected == std::vector<size_t>{firstStroke} &&
                                state.ink[firstStroke].points == firstBefore,
                        "Select Object did not select on click without moving the object");
                input.shiftHeld = true;
                clickStroke(secondStroke);
                input.shiftHeld = false;
                require(state.selected == std::vector<size_t>({firstStroke, secondStroke}),
                        "Shift-click Select Object did not add a second object");
                const auto handSelection = state.selected;
                const auto handInkCount = state.ink.size();
                const double handOffsetX = state.offsetX;
                const double handOffsetY = state.offsetY;
                selectTool(Tool::Hand);
                require(state.brush.tool == Tool::Hand && state.selected == handSelection &&
                                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toolButtons[4])),
                        "Selecting Hand should preserve the current object selection");
                const auto panStart = state.pageScreenPoint(0, 240, 240);
                g_signal_emit_by_name(mouse, "drag-begin", panStart.x, panStart.y);
                g_signal_emit_by_name(mouse, "drag-update", 24.0, -18.0);
                g_signal_emit_by_name(mouse, "drag-end", 24.0, -18.0);
                require(state.ink.size() == handInkCount && state.selected == handSelection &&
                                state.offsetX != handOffsetX && state.offsetY != handOffsetY && !input.active(),
                        "Hand drag did not pan the canvas while retaining its selected objects");
                const auto lowerFirstBefore = state.ink[0].points;
                const auto lowerSecondBefore = state.ink[1].points;
                const auto upperFirstBefore = state.ink[2].points;
                const auto upperSecondBefore = state.ink[3].points;
                const auto textBeforeVerticalSpace = state.texts[0];
                activateSelection(Tool::SelectRectangle);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), 8);
                require(state.brush.tool == Tool::VerticalSpace &&
                                gtk_widget_get_visible(selectionModeDropdown) &&
                                !gtk_widget_get_visible(selectionActions),
                        "Vertical space did not activate from the selection mode menu");
                input.setModifiers(false, false, false);
                const auto verticalStart = state.pageScreenPoint(0, 40, 240);
                g_signal_emit_by_name(mouse, "drag-begin", verticalStart.x, verticalStart.y);
                require(state.verticalSpacePreview() && !state.verticalSpacePreview()->moveAbove &&
                                state.verticalSpacePreview()->inkIndices.empty(),
                        "Vertical-space drag should start in below-the-line mode with no lower objects selected");
                gboolean controlHandled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Control_L, 0u,
                                      GdkModifierType(GDK_CONTROL_MASK), &controlHandled);
                require(controlHandled && state.verticalSpacePreview()->moveAbove &&
                                state.verticalSpacePreview()->inkIndices == std::vector<size_t>({2, 3}) &&
                                state.verticalSpacePreview()->textIndices == std::vector<size_t>{0},
                        "Holding Control during the drag did not switch the preview above the boundary");
                g_signal_emit_by_name(mouse, "drag-update", 0.0, 20.0 * state.zoom);
                const double verticalShift = state.verticalSpacePreview()->endY -
                                             state.verticalSpacePreview()->startY;
                require(verticalShift > 0, "Vertical-space boundary did not move down");
                waitForNextPaint();
                capture("vertical-space-preview");
                g_signal_emit_by_name(keys, "key-released", GDK_KEY_Control_L, 0u, GdkModifierType(0));
                require(!state.verticalSpacePreview()->moveAbove && state.verticalSpacePreview()->inkIndices.empty(),
                        "Releasing Control during the drag did not switch the preview below the boundary");
                controlHandled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Control_L, 0u,
                                      GdkModifierType(GDK_CONTROL_MASK), &controlHandled);
                require(controlHandled && state.verticalSpacePreview()->inkIndices == std::vector<size_t>({2, 3}),
                        "Pressing Control again did not restore the above-the-line preview");
                g_signal_emit_by_name(mouse, "drag-end", 0.0, 20.0 * state.zoom);
                g_signal_emit_by_name(keys, "key-released", GDK_KEY_Control_L, 0u, GdkModifierType(0));
                require(!state.verticalSpacePreview() && state.ink[0].points == lowerFirstBefore &&
                                state.ink[1].points == lowerSecondBefore &&
                                std::abs(state.ink[2].points.front().y - upperFirstBefore.front().y - verticalShift) < 1e-6 &&
                                std::abs(state.ink[3].points.front().y - upperSecondBefore.front().y - verticalShift) < 1e-6 &&
                                std::abs(state.texts[0].y - textBeforeVerticalSpace.y - verticalShift) < 1e-6,
                        "Vertical space did not move only the upper-layer objects selected by its preview");
                const auto verticalRevision = state.revision();
                state.undo();
                require(state.revision() != verticalRevision &&
                                state.ink[2].points.front().y == upperFirstBefore.front().y &&
                                state.texts[0].y == textBeforeVerticalSpace.y,
                        "Undo did not restore the vertical-space edit as one step");
                state.redo();
                require(state.revision() == verticalRevision &&
                                std::abs(state.ink[2].points.front().y - upperFirstBefore.front().y - verticalShift) < 1e-6,
                        "Redo did not reapply the vertical-space edit");
                waitForNextPaint();
                capture("vertical-space-applied");
                activateSelection(Tool::SmartSelect);
                state.clearSelection();
                state.snapGridEnabled = false;
                prototype::Ink rotationStroke{0, {{120, 300}, {220, 340}},
                                               {Tool::Pen, {0.8, 0.1, 0.1}, 3.0}};
                rotationStroke.layerId = state.activeLayerId(0);
                rotationStroke.originLayer = state.activeLayerOrigin(0);
                rotationStroke.layerOrder = state.ink.size();
                state.ink.push_back(rotationStroke);
                const auto rotationStrokeIndex = state.ink.size() - 1;
                state.selected = {rotationStrokeIndex};
                const auto originalFrame = *state.selectionBox();
                const auto rotationHandle = state.selectionHandles()[8];
                const prototype::Point rotationCenter{(originalFrame.x0 + originalFrame.x1) / 2,
                                                       (originalFrame.y0 + originalFrame.y1) / 2};
                const auto radius = std::hypot(rotationHandle.x - rotationCenter.x,
                                               rotationHandle.y - rotationCenter.y);
                const auto rotationStart = state.pageScreenPoint(0, rotationHandle.x, rotationHandle.y);
                const auto rotationEnd = state.pageScreenPoint(0, rotationCenter.x, rotationCenter.y + radius);
                g_signal_emit_by_name(mouse, "drag-begin", rotationStart.x, rotationStart.y);
                g_signal_emit_by_name(mouse, "drag-update", rotationEnd.x - rotationStart.x,
                                      rotationEnd.y - rotationStart.y);
                g_signal_emit_by_name(mouse, "drag-end", rotationEnd.x - rotationStart.x,
                                      rotationEnd.y - rotationStart.y);
                require(state.selectionBox() &&
                                std::abs(state.selectionBox()->x0 - originalFrame.x0) < 1e-6 &&
                                std::abs(state.selectionBox()->y0 - originalFrame.y0) < 1e-6 &&
                                std::abs(state.selectionBox()->x1 - originalFrame.x1) < 1e-6 &&
                                std::abs(state.selectionBox()->y1 - originalFrame.y1) < 1e-6,
                        "Smart Select rotation did not retain the native axis-aligned handle frame");
                waitForNextPaint();
                capture("smart-select-rotated-frame");
                const auto rightHandle = state.selectionHandles()[3];
                const auto rotationResizeStart = state.pageScreenPoint(0, rightHandle.x, rightHandle.y);
                const auto rotationResizeEnd = state.pageScreenPoint(0, rightHandle.x + 40, rightHandle.y);
                g_signal_emit_by_name(mouse, "drag-begin", rotationResizeStart.x, rotationResizeStart.y);
                g_signal_emit_by_name(mouse, "drag-update", rotationResizeEnd.x - rotationResizeStart.x,
                                      rotationResizeEnd.y - rotationResizeStart.y);
                g_signal_emit_by_name(mouse, "drag-end", rotationResizeEnd.x - rotationResizeStart.x,
                                      rotationResizeEnd.y - rotationResizeStart.y);
                require(state.selectionBox() && std::abs(state.selectionBox()->x1 - 260) < 1e-6 &&
                                std::abs(state.ink[rotationStrokeIndex].points[0].x - 190) < 1e-6 &&
                                std::abs(state.ink[rotationStrokeIndex].points[0].y - 270) < 1e-6 &&
                                std::abs(state.ink[rotationStrokeIndex].points[1].x - 150) < 1e-6 &&
                                std::abs(state.ink[rotationStrokeIndex].points[1].y - 410) < 1e-6,
                        "Smart Select resize did not apply scaling in the accumulated rotated coordinates");
                waitForNextPaint();
                capture("smart-select-rotated-resize");
                state.undo();
                require(state.selectionBox() && std::abs(state.selectionBox()->x1 - originalFrame.x1) < 1e-6 &&
                                std::abs(state.ink[rotationStrokeIndex].points[1].y - 370) < 1e-6,
                        "Undo did not restore the rotated handle frame and stroke geometry");
                state.undo();
                require(std::abs(state.ink[rotationStrokeIndex].points[0].x - 120) < 1e-6 &&
                                std::abs(state.ink[rotationStrokeIndex].points[0].y - 300) < 1e-6 &&
                                std::abs(state.selectionBox()->x1 - originalFrame.x1) < 1e-6,
                        "Undo rotation did not restore the original Smart Select state");
                state.redo();
                state.redo();
                require(std::abs(state.ink[rotationStrokeIndex].points[1].y - 410) < 1e-6 &&
                                std::abs(state.selectionBox()->x1 - 260) < 1e-6,
                        "Redo did not restore the rotated Smart Select resize");
                state.undo();
                state.undo();
                state.clearSelection();
                state.ink.pop_back();

                const auto viewportWidth = gtk_widget_get_width(canvas);
                const auto viewportHeight = gtk_widget_get_height(canvas);
                require(viewportWidth > 0 && viewportHeight > 0, "Edge-pan verification has no allocated canvas");
                state.clearSelection();
                prototype::TextItem edgePanText;
                edgePanText.page = 0;
                edgePanText.layerId = 1;
                edgePanText.originLayer = 0;
                edgePanText.text = "edge pan";
                edgePanText.layoutWidth = 40;
                edgePanText.layoutHeight = 40;
                edgePanText.x = 80;
                edgePanText.y = 100;
                state.texts.push_back(edgePanText);
                const auto edgePanTextIndex = state.texts.size() - 1;
                state.selectedTexts = {edgePanTextIndex};
                state.brush.tool = Tool::SmartSelect;
                state.snapGridEnabled = false;
                state.edgePanSpeed = 20;
                state.edgePanMaxMultiplier = 5;
                state.zoom = std::max(2.0, (viewportWidth + 50.0) / 500.0);
                state.offsetX = 0;
                state.offsetY = 0;
                const auto edgePanStart = state.pageScreenPoint(0, 100, 120);
                const auto edgePanPointer = prototype::Point{viewportWidth - 1.0, edgePanStart.y};
                require(state.beginSelection(edgePanStart.x, edgePanStart.y),
                        "Smart Select edge-pan move did not begin");
                state.moveSelectionPointer(edgePanPointer.x, edgePanPointer.y);
                require(state.selectionCanEdgePan(), "Smart Select move did not enter edge-pan mode");
                const auto edgePanBounds = state.selectionBox();
                require(edgePanBounds && edgePanBounds->x1 + state.pageLeft(0) >
                                                  (viewportWidth - state.offsetX) / state.zoom,
                        "Edge-pan fixture does not extend past the canvas viewport");
                selectionVerifyEdgePanOffsetX = state.offsetX;
                selectionVerifyEdgePanScreenX =
                        state.offsetX + (state.pageLeft(0) + edgePanBounds->x0) * state.zoom;
                scheduleSelectionEdgePan(edgePanPointer.x, edgePanPointer.y);
                update();
                return G_SOURCE_CONTINUE;
            }
            case 6: {
                if (state.offsetX >= selectionVerifyEdgePanOffsetX) {
                    --selectionVerifyStep;
                    return G_SOURCE_CONTINUE;
                }
                const auto edgePanBounds = state.selectionBox();
                require(edgePanBounds && state.offsetX < selectionVerifyEdgePanOffsetX &&
                                std::abs(state.offsetX + (state.pageLeft(0) + edgePanBounds->x0) * state.zoom -
                                         selectionVerifyEdgePanScreenX) < 1e-6,
                        "The edge-pan timer did not scroll while keeping the selection under the stationary pointer");
                stopSelectionEdgePan();
                state.finishSelection();
                update();
                waitForNextPaint();
                capture("smart-select-edge-pan");
                std::cout << "PASS: GTK visible-layer rectangle/lasso selection, Smart Select edge hit, grid snap, "
                             "resize snap, rotation-aware resize and edge-pan, Text-tool mouse/stylus move/resize and formatting, "
                             "vertical-space direction/preview/undo, Select Object and Hand modes, "
                             "selection-preserving pan, hidden-layer fallback, remembered tool state, and shortcuts\n";
                closeWindow();
                return G_SOURCE_REMOVE;
            }
            default:
                return G_SOURCE_REMOVE;
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    void smokeDragPdfPage(Tool tool) {
        activateSelection(tool);
        auto* page = pdfPageForPage(0);
        require(page != nullptr, "PDF-backed XOPP fixture lost its first PDF page");
        PopplerRectangle* glyphs = nullptr;
        guint glyphCount = 0;
        require(poppler_page_get_text_layout(page, &glyphs, &glyphCount) && glyphCount > 0,
                "PDF-backed fixture has no selectable text layout");
        const auto firstGlyph = glyphs[0];
        double left = std::min(firstGlyph.x1, firstGlyph.x2);
        double right = std::max(firstGlyph.x1, firstGlyph.x2);
        double top = std::min(firstGlyph.y1, firstGlyph.y2);
        double bottom = std::max(firstGlyph.y1, firstGlyph.y2);
        for (guint index = 1; index < glyphCount; ++index) {
            const auto& glyph = glyphs[index];
            if (std::abs(glyph.y1 - firstGlyph.y1) > 1e-5 || std::abs(glyph.y2 - firstGlyph.y2) > 1e-5) {
                continue;
            }
            left = std::min(left, std::min(glyph.x1, glyph.x2));
            right = std::max(right, std::max(glyph.x1, glyph.x2));
            top = std::min(top, std::min(glyph.y1, glyph.y2));
            bottom = std::max(bottom, std::max(glyph.y1, glyph.y2));
        }
        g_free(glyphs);
        g_object_unref(page);
        const auto start = state.pageScreenPoint(0, left - 1, top - 1);
        const auto end = state.pageScreenPoint(0, right + 1, bottom + 1);
        g_signal_emit_by_name(mouse, "drag-begin", start.x, start.y);
        g_signal_emit_by_name(mouse, "drag-update", end.x - start.x, end.y - start.y);
        g_signal_emit_by_name(mouse, "drag-end", end.x - start.x, end.y - start.y);
    }

    gboolean smokePdfTextSelection() {
        switch (pdfTextSelectionVerifyStep++) {
            case 0: {
                require(nativeSession && document && pageCount >= 1 && state.texts.size() >= 3,
                        "PDF text selection verification requires a native PDF-backed XOPP fixture");
                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                require(pdfTextSelection && pdfTextSelection->finalized &&
                                pdfTextSelection->text.find("Test PDF for Xournal++") != std::string::npos &&
                                pdfTextSelection->text.find("テスト") == std::string::npos &&
                                !pdfTextSelection->textRects.empty(),
                        "Linear selection did not select text from the PDF background only");
                capture("pdf-text-linear-selection");

                auto* page = pdfPageForPage(0);
                require(page != nullptr, "PDF-backed XOPP fixture lost its first PDF page");
                PopplerRectangle* glyphs = nullptr;
                guint glyphCount = 0;
                require(poppler_page_get_text_layout(page, &glyphs, &glyphCount) && glyphCount > 0,
                        "PDF-backed fixture has no text layout for Smart Select");
                PopplerRectangle smartGlyph{};
                bool foundPdfOnlyGlyph = false;
                for (guint index = 0; index < glyphCount; ++index) {
                    const auto& glyph = glyphs[index];
                    const prototype::Point point{(glyph.x1 + glyph.x2) / 2, (glyph.y1 + glyph.y2) / 2};
                    if (!smartSelectHasEditableObjectAt(0, point)) {
                        smartGlyph = glyph;
                        foundPdfOnlyGlyph = true;
                        break;
                    }
                }
                g_free(glyphs);
                g_object_unref(page);
                require(foundPdfOnlyGlyph, "PDF-backed fixture has no glyph outside editable native objects");
                const auto smartStart = state.pageScreenPoint(
                        0, (smartGlyph.x1 + smartGlyph.x2) / 2, (smartGlyph.y1 + smartGlyph.y2) / 2);
                const auto smartEnd = state.pageScreenPoint(0, std::max(smartGlyph.x1, smartGlyph.x2) + 80,
                                                            (smartGlyph.y1 + smartGlyph.y2) / 2);
                input.setModifiers(false, false, false);
                activateSelection(Tool::SmartSelect);
                begin(Device::Mouse, smartStart.x, smartStart.y);
                require(smartSelectPdfGesture && !input.active(),
                        "Smart Select did not defer a PDF-text press until the drag threshold");
                motion(Device::Mouse, smartEnd.x, smartEnd.y);
                require(!smartSelectPdfGesture && pdfTextSelection && pdfTextSelection->dragging &&
                                pdfTextSelection->tool == Tool::SmartSelect &&
                                pdfTextSelection->style == prototype::PdfTextSelectionStyle::Linear,
                        "Smart Select did not delegate a PDF-text drag to linear text selection");
                finish(Device::Mouse, std::nullopt, smartEnd);
                require(pdfTextSelection && pdfTextSelection->finalized && !pdfTextSelection->text.empty(),
                        "Smart Select PDF-text drag did not finalize a native PDF selection");
                capture("smart-select-pdf-text");
                g_signal_emit_by_name(pdfSelectionStyleButton, "clicked");
                require(state.brush.tool == Tool::SmartSelect && pdfTextSelection &&
                                pdfTextSelection->style == prototype::PdfTextSelectionStyle::Rectangle,
                        "PDF selection actions changed the active Smart Select tool or failed to switch style");
                g_signal_emit_by_name(pdfSelectionStyleButton, "clicked");
                require(state.brush.tool == Tool::SmartSelect && pdfTextSelection &&
                                pdfTextSelection->style == prototype::PdfTextSelectionStyle::Linear,
                        "Smart Select PDF selection did not return to linear style");

                activateSelection(Tool::SmartSelect);
                begin(Device::Mouse, smartStart.x, smartStart.y);
                require(smartSelectPdfGesture.has_value(), "Smart Select PDF click was not deferred");
                finish(Device::Mouse, std::nullopt, smartStart);
                require(!smartSelectPdfGesture && !pdfTextSelection,
                        "A PDF-text click below the drag threshold unexpectedly selected PDF text");

                const auto blankStart = state.pageScreenPoint(0, 1, 1);
                const auto blankEnd = state.pageScreenPoint(0, 60, 60);
                require(!pdfTextAtCanvasPoint(0, {1, 1}), "Smart Select Alt test point is not in a blank PDF margin");
                input.setModifiers(false, false, true);
                begin(Device::Mouse, blankStart.x, blankStart.y);
                require(smartSelectPdfGesture && smartSelectPdfGesture->altPressed,
                        "Alt did not force Smart Select into deferred PDF-text routing");
                motion(Device::Mouse, blankEnd.x, blankEnd.y);
                require(pdfTextSelection && pdfTextSelection->dragging &&
                                pdfTextSelection->tool == Tool::SmartSelect,
                        "Alt-drag with Smart Select did not start PDF text selection on a blank page area");
                finish(Device::Mouse, std::nullopt, blankEnd);
                input.setModifiers(false, false, false);
                clearPdfTextSelection(false);
                require(!pdfTextSelection, "Alt-drag cleanup left a PDF selection active");

                activateSelection(Tool::SmartSelect);
                input.setModifiers(true, false, false);
                begin(Device::Mouse, smartStart.x, smartStart.y);
                require(!smartSelectPdfGesture && input.active(),
                        "Shift did not route Smart Select directly to rectangle selection");
                motion(Device::Mouse, smartEnd.x, smartEnd.y);
                finish(Device::Mouse, std::nullopt, smartEnd);
                input.setModifiers(false, false, false);
                require(!pdfTextSelection, "Shift-rectangle gesture unexpectedly entered PDF text selection");
                capture("smart-select-shift-rectangle");

                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                break;
            }
            case 1: {
                g_signal_emit_by_name(pdfSelectionStyleButton, "clicked");
                require(state.brush.tool == Tool::SelectPdfTextRect && pdfTextSelection &&
                                pdfTextSelection->style == prototype::PdfTextSelectionStyle::Rectangle &&
                                pdfTextSelection->text.find("Test PDF for Xournal++") != std::string::npos,
                        "Switching to rectangular PDF selection did not recompute the current selection");
                capture("pdf-text-rectangle-selection");
                break;
            }
            case 2: {
                auto* page = pdfPageForPage(0);
                require(page != nullptr, "PDF-backed XOPP fixture lost its first PDF page");
                PopplerRectangle* glyphs = nullptr;
                guint glyphCount = 0;
                require(poppler_page_get_text_layout(page, &glyphs, &glyphCount) && glyphCount > 0,
                        "PDF-backed fixture has no selectable text layout");
                const auto firstGlyph = glyphs[0];
                g_free(glyphs);
                g_object_unref(page);
                const auto click = state.pageScreenPoint(0, (firstGlyph.x1 + firstGlyph.x2) / 2,
                                                        (firstGlyph.y1 + firstGlyph.y2) / 2);
                g_signal_emit_by_name(textClick, "released", 2, click.x, click.y);
                require(pdfTextSelection && pdfTextSelection->finalized &&
                                pdfTextSelection->style == prototype::PdfTextSelectionStyle::Word &&
                                !pdfTextSelection->text.empty(),
                        "Double click did not select a PDF word");
                capture("pdf-text-word-selection");
                g_signal_emit_by_name(textClick, "released", 3, click.x, click.y);
                require(pdfTextSelection && pdfTextSelection->finalized &&
                                pdfTextSelection->style == prototype::PdfTextSelectionStyle::Line &&
                                pdfTextSelection->text.find("Test PDF for Xournal++") != std::string::npos,
                        "Triple click did not select the full PDF text line");
                capture("pdf-text-line-selection");
                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                pdfClipboardExpected = pdfTextSelection->text;
                g_signal_emit_by_name(pdfCopyButton, "clicked");
                require(!pdfTextSelection && !gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover)),
                        "Copy did not dismiss the PDF selection actions");
                auto* clipboard = gdk_display_get_clipboard(gtk_widget_get_display(canvas));
                gdk_clipboard_read_text_async(
                        clipboard, nullptr,
                        +[](GObject* source, GAsyncResult* result, gpointer data) {
                            auto* self = static_cast<Prototype*>(data);
                            GError* error = nullptr;
                            char* text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
                            self->pdfClipboardReadError = error ? error->message : "";
                            g_clear_error(&error);
                            self->pdfClipboardReadText = text ? text : "";
                            g_free(text);
                            self->pdfClipboardReadComplete = true;
                        },
                        this);
                break;
            }
            case 3: {
                if (!pdfClipboardReadComplete) {
                    --pdfTextSelectionVerifyStep;
                    return G_SOURCE_CONTINUE;
                }
                require(pdfClipboardReadError.empty() && pdfClipboardReadText == pdfClipboardExpected,
                        "PDF Copy did not publish the selected text to the clipboard");
                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                const auto oldInkCount = state.ink.size();
                g_signal_emit_by_name(pdfHighlightButton, "clicked");
                require(state.ink.size() > oldInkCount && !pdfTextSelection,
                        "Highlight did not add PDF text annotation strokes");
                for (size_t index = oldInkCount; index < state.ink.size(); ++index) {
                    require(state.ink[index].brush.tool == Tool::Highlighter && state.ink[index].fill == 60 &&
                                    state.ink[index].capStyle == 1,
                            "PDF highlight did not preserve marker opacity and butt caps");
                }
                capture("pdf-text-highlight");
                break;
            }
            case 4: {
                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                const auto oldInkCount = state.ink.size();
                g_signal_emit_by_name(pdfUnderlineButton, "clicked");
                require(state.ink.size() > oldInkCount && !pdfTextSelection,
                        "Underline did not add PDF text annotation strokes");
                for (size_t index = oldInkCount; index < state.ink.size(); ++index) {
                    require(state.ink[index].fill == 230 && state.ink[index].capStyle == 1,
                            "PDF underline did not preserve opacity and butt caps");
                }
                capture("pdf-text-underline");
                break;
            }
            case 5: {
                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                const auto oldInkCount = state.ink.size();
                g_signal_emit_by_name(pdfStrikethroughButton, "clicked");
                require(state.ink.size() > oldInkCount && !pdfTextSelection,
                        "Strikethrough did not add PDF text annotation strokes");
                for (size_t index = oldInkCount; index < state.ink.size(); ++index) {
                    require(state.ink[index].fill == 230 && state.ink[index].capStyle == 1,
                            "PDF strikethrough did not preserve opacity and butt caps");
                }
                capture("pdf-text-strikethrough");
                break;
            }
            case 6: {
                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                gtk_menu_button_popup(GTK_MENU_BUTTON(pdfHighlightPaletteButton));
                capture("pdf-text-highlight-presets");
                g_signal_emit_by_name(pdfHighlightPresetButtons[1], "clicked");
                require(std::abs(highlightBrush.color[0] - 140.0 / 255) < 1e-6 &&
                                std::abs(highlightBrush.color[1] - 233.0 / 255) < 1e-6 &&
                                std::abs(highlightBrush.color[2] - 154.0 / 255) < 1e-6,
                        "Green PDF highlight preset did not select the production color");
                const auto oldInkCount = state.ink.size();
                const auto oldRevision = state.revision();
                g_signal_emit_by_name(pdfHighlightButton, "clicked");
                require(state.ink.size() == oldInkCount && state.revision() == oldRevision + 1,
                        "Reapplying a highlight should recolor existing strokes in one edit");
                for (size_t index = 0; index < state.ink.size(); ++index) {
                    if (state.ink[index].fill == 60) {
                        require(std::abs(state.ink[index].brush.color[1] - 233.0 / 255) < 1e-6,
                                "Reapplying a PDF highlight did not recolor existing markers");
                    }
                }
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.revision() == oldRevision && state.ink.size() == oldInkCount &&
                                std::abs(state.ink.front().brush.color[0] - 1.0) < 1e-6,
                        "Undo did not restore the prior PDF highlight color");
                g_signal_emit_by_name(redoButton, "clicked");
                require(state.revision() == oldRevision + 1 &&
                                std::abs(state.ink.front().brush.color[1] - 233.0 / 255) < 1e-6,
                        "Redo did not restore the PDF highlight recolor");
                capture("pdf-text-highlight-recolored");
                gtk_menu_button_popup(GTK_MENU_BUTTON(pdfMarkerOpacityButton));
                capture("pdf-text-marker-opacity-control");
                gtk_range_set_value(GTK_RANGE(pdfMarkerOpacityScale), 128);
                require(pdfTextMarkerOpacity == 128, "PDF marker opacity control did not change its value");
                gtk_menu_button_popdown(GTK_MENU_BUTTON(pdfMarkerOpacityButton));
                smokeDragPdfPage(Tool::SelectPdfTextLinear);
                const auto beforeOpacityEdit = state.ink.size();
                g_signal_emit_by_name(pdfHighlightButton, "clicked");
                require(state.ink.size() > beforeOpacityEdit,
                        "Applying a different PDF marker opacity did not create updated annotations");
                for (size_t index = beforeOpacityEdit; index < state.ink.size(); ++index) {
                    require(state.ink[index].fill == 128,
                            "PDF highlight opacity control did not set the annotation fill value");
                }
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.ink.size() == beforeOpacityEdit,
                        "Undo did not remove highlights created with the changed opacity");
                gtk_range_set_value(GTK_RANGE(pdfMarkerOpacityScale), 60);
                require(pdfTextMarkerOpacity == 60, "PDF marker opacity did not restore its default value");
                break;
            }
            case 7: {
                pdfTextSelectionOutput = (std::filesystem::path(options.smokeDirectory) /
                                          "pdf-text-annotations.xopp").string();
                saveCopyTo(pdfTextSelectionOutput, [this] { pdfTextSelectionSaveComplete = true; });
                require(busy, "Saving PDF text annotations did not start");
                break;
            }
            default: {
                require(pdfTextSelectionSaveComplete && !modified() &&
                                std::filesystem::is_regular_file(pdfTextSelectionOutput),
                        "Native save did not preserve PDF text annotations");
                std::cout
                        << "PASS: GTK Smart Select PDF delegation, threshold and modifier routing, "
                           "linear/rectangle/word/line "
                           "selection, clipboard, marker presets and opacity, annotations, undo/redo and native save\n";
                closeWindow();
                return G_SOURCE_REMOVE;
            }
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    gboolean smokePreferencesVerify() {
        const auto control = [&](PreferenceKey key) { return preferenceControls[static_cast<size_t>(key)]; };
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
                    require(!importedConfig.settings.showSidebar &&
                                    !adw_overlay_split_view_get_show_sidebar(splitView) &&
                                    importedConfig.settings.mainToolRail == std::vector<prototype::MainToolAction>(
                                            prototype::defaultMainToolRail.begin(),
                                            prototype::defaultMainToolRail.end()) &&
                                    importedConfig.settings.themeVariant == prototype::ThemeVariant::ForceDark &&
                                    paletteSwatchButtons.size() == importedConfig.palette.swatches.size(),
                            "Fresh GTK 4 startup did not use the canvas-first tool and color rail defaults");
                    capture("main-tool-rail-default");
                    openPreferences();
                    break;
                case 1: {
                    require(control(PreferenceKey::Theme) && control(PreferenceKey::StylusButtonOne) &&
                                    control(PreferenceKey::StylusButtonTwo) &&
                                    control(PreferenceKey::AutosaveTimeout) &&
                                    control(PreferenceKey::PositionHighlight) &&
                                    control(PreferenceKey::CursorHighlightColor) &&
                                    control(PreferenceKey::CursorHighlightRadius) &&
                                    control(PreferenceKey::CursorHighlightBorderColor) &&
                                    control(PreferenceKey::CursorHighlightBorderWidth) &&
                                    control(PreferenceKey::RotationSnapTolerance) &&
                                    control(PreferenceKey::SnapRotation) && control(PreferenceKey::SnapGrid) &&
                                    control(PreferenceKey::SnapGridSize) && control(PreferenceKey::SnapGridTolerance) &&
                                    control(PreferenceKey::EdgePanSpeed) &&
                                    control(PreferenceKey::EdgePanMaxMultiplier),
                            "Preferences dialog is missing appearance, stylus or autosave controls");
                    require(control(PreferenceKey::FloatingToolboxRow1) &&
                                    control(PreferenceKey::FloatingToolboxRow2) &&
                                    control(PreferenceKey::FloatingToolboxRow3) &&
                                    control(PreferenceKey::FloatingToolboxRow4) && floatingToolboxApplyButton,
                            "Preferences dialog is missing the four floating toolbox rows");
                    require(control(PreferenceKey::LatexAutoCheckDependencies) &&
                                    control(PreferenceKey::LatexDefaultText) &&
                                    control(PreferenceKey::LatexTemplatePath) &&
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
                                    control(PreferenceKey::LatexTemporaryFileExtension) &&
                                    latexSettingsApplyButton && latexSetupTestButton,
                            "LaTeX Preferences page is missing an imported setting or editor control");
                    require(control(PreferenceKey::MainToolRail) && mainToolRailApplyButton &&
                                    std::string(gtk_editable_get_text(GTK_EDITABLE(
                                            control(PreferenceKey::MainToolRail)))) ==
                                            "SMART_SELECT,PEN,HIGHLIGHTER,ERASER,TEXT,HAND",
                            "Preferences dialog did not show the canvas-first main tool rail layout");
                    require(std::string(gtk_editable_get_text(GTK_EDITABLE(
                                            control(PreferenceKey::FloatingToolboxRow1)))) ==
                                            "PEN,HIGHLIGHTER,LASER_POINTER,PDF_TOOL,UNDO" &&
                                    std::string(gtk_editable_get_text(GTK_EDITABLE(
                                            control(PreferenceKey::FloatingToolboxRow2)))) ==
                                            "ZOOM_SLIDER,DRAW_ELLIPSE",
                            "Preferences rows did not show the imported legacy floating toolbar layout");
                    require(adw_combo_row_get_selected(ADW_COMBO_ROW(control(PreferenceKey::StylusButtonOne))) ==
                                            static_cast<guint>(prototype::ButtonToolAction::Highlighter) &&
                                    adw_combo_row_get_selected(ADW_COMBO_ROW(control(PreferenceKey::StylusButtonTwo))) ==
                                            static_cast<guint>(prototype::ButtonToolAction::Hand),
                            "Preferences rows did not show imported stylus mappings");
                    require(adw_switch_row_get_active(
                                    ADW_SWITCH_ROW(control(PreferenceKey::SnapRotation))),
                            "Rotation snapping should be enabled by default in Preferences");
                    capture("preferences-dialog", GTK_WIDGET(preferencesDialog), true);
                    adw_preferences_dialog_set_visible_page_name(preferencesDialog, "latex");
                    require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesDialog)) == "latex",
                            "Preferences dialog did not open the LaTeX page");
                    capture("preferences-latex", GTK_WIDGET(preferencesDialog), true);
                    adw_preferences_dialog_set_visible_page_name(preferencesDialog, "toolbar");
                    require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesDialog)) == "toolbar",
                            "Preferences dialog did not switch to the Toolbar page");
                    break;
                }
                case 2:
                    capture("preferences-toolbar");
                    adw_preferences_dialog_set_visible_page_name(preferencesDialog, "input");
                    require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesDialog)) == "input",
                            "Preferences dialog did not switch to the Input page");
                    break;
                case 3: {
                    capture("preferences-input");
                    adw_switch_row_set_active(ADW_SWITCH_ROW(control(PreferenceKey::PositionHighlight)), true);
                    const auto fill = unpackArgbColor(0x7f123456U);
                    gtk_color_dialog_button_set_rgba(
                            GTK_COLOR_DIALOG_BUTTON(control(PreferenceKey::CursorHighlightColor)), &fill);
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::CursorHighlightRadius)), 17.0);
                    const auto border = unpackArgbColor(0xc0abcdefU);
                    gtk_color_dialog_button_set_rgba(
                            GTK_COLOR_DIALOG_BUTTON(control(PreferenceKey::CursorHighlightBorderColor)), &border);
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::CursorHighlightBorderWidth)), 2.0);
                    adw_preferences_dialog_set_visible_page_name(preferencesDialog, "behavior");
                    require(std::string(adw_preferences_dialog_get_visible_page_name(preferencesDialog)) == "behavior",
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
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexDefaultText)),
                                          "\\frac{\\alpha}{2}");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexTemplatePath)),
                                          "/tmp/template with spaces.tex");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexGeneratorCommand)),
                                          "pdflatex -halt-on-error -interaction=nonstopmode '{}'");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexExternalEditorCommand)),
                                          "'/tmp/fake latex editor' --wait");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::LatexTemporaryFileExtension)), "tex");
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexAutoCheckDependencies)), false);
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexUseCustomEditorFont)), true);
                    require(gtk_widget_get_sensitive(control(PreferenceKey::LatexEditorFont)),
                            "Enabling the custom LaTeX font did not enable its font selector");
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexEditorWordWrap)), false);
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewAutoIndent)), false);
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewSyntaxHighlight)), false);
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexSourceViewShowLineNumbers)), true);
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexUseExternalEditor)), true);
                    adw_switch_row_set_active(
                            ADW_SWITCH_ROW(control(PreferenceKey::LatexExternalEditorAutoConfirm)), true);
                    require(!gtk_widget_get_sensitive(control(PreferenceKey::LatexEditorFont)) &&
                                    !gtk_widget_get_sensitive(control(PreferenceKey::LatexSourceViewTheme)) &&
                                    gtk_widget_get_sensitive(control(PreferenceKey::LatexExternalEditorCommand)) &&
                                    gtk_widget_get_sensitive(
                                            control(PreferenceKey::LatexExternalEditorAutoConfirm)),
                            "LaTeX preference sensitivity did not follow custom font and external-editor settings");
                    g_signal_emit_by_name(latexSettingsApplyButton, "clicked");
                    require(importedConfig.settings.latexDefaultText == "\\frac{\\alpha}{2}" &&
                                    prototype::preferences_detail::pathToUtf8(
                                            importedConfig.settings.latexTemplatePath) ==
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
                                    std::string(gtk_label_get_text(GTK_LABEL(latexSettingsApplyStatus))) == "Saved",
                            "Applying LaTeX preferences did not persist the text fields and editor options");
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanSpeed)), 31.0);
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanMaxMultiplier)), 3.5);
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::FloatingToolboxRow1)),
                                          "SMART_SELECT,PEN");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::FloatingToolboxRow2)), "ZOOM_SLIDER");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::FloatingToolboxRow3)), "FINE");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::FloatingToolboxRow4)), "DRAW_ARROW");
                    g_signal_emit_by_name(floatingToolboxApplyButton, "clicked");
                    require(importedConfig.settings.floatingToolboxRows[0] ==
                                            std::vector<std::string>({"SMART_SELECT", "PEN"}) &&
                                    importedConfig.settings.floatingToolboxRows[1] ==
                                            std::vector<std::string>({"ZOOM_SLIDER"}) &&
                                    importedConfig.settings.floatingToolboxRows[2] ==
                                            std::vector<std::string>({"FINE"}) &&
                                    importedConfig.settings.floatingToolboxRows[3] ==
                                            std::vector<std::string>({"DRAW_ARROW"}),
                            "Applying toolbar rows did not update the active layout");
                    g_signal_emit_by_name(floatingToolboxApplyButton, "clicked");
                    gtk_editable_set_text(GTK_EDITABLE(control(PreferenceKey::MainToolRail)),
                                          "SMART_SELECT,TEXT,PEN,HAND");
                    g_signal_emit_by_name(mainToolRailApplyButton, "clicked");
                    require(importedConfig.settings.mainToolRail ==
                                            std::vector<prototype::MainToolAction>({
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
                    auto* thirdConfiguredRow = gtk_widget_get_next_sibling(
                            gtk_widget_get_next_sibling(firstConfiguredRow));
                    g_signal_emit_by_name(gtk_widget_get_first_child(thirdConfiguredRow), "clicked");
                    require(std::abs(state.brush.width - prototype::widthsForTool(Tool::Pen)[1]) < 1e-8,
                            "Configured FINE action did not select the Pen fine-width preset");
                    auto* fourthConfiguredRow = gtk_widget_get_next_sibling(thirdConfiguredRow);
                    g_signal_emit_by_name(gtk_widget_get_first_child(fourthConfiguredRow), "clicked");
                    require(state.drawingMode == prototype::DrawingMode::Arrow,
                            "Configured DRAW_ARROW action did not select arrow drawing mode");
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
                                    std::abs(importedConfig.settings.zoomStep - 0.22) < 1e-8 &&
                                    std::abs(importedConfig.settings.snapRotationTolerance - 0.45) < 1e-8 &&
                                    importedConfig.settings.floatingToolboxRows[0] ==
                                            std::vector<std::string>({"SMART_SELECT", "PEN"}) &&
                                    importedConfig.settings.floatingToolboxRows[3] ==
                                            std::vector<std::string>({"DRAW_ARROW"}) &&
                                    importedConfig.settings.latexDefaultText == "\\frac{\\alpha}{2}" &&
                                    prototype::preferences_detail::pathToUtf8(
                                            importedConfig.settings.latexTemplatePath) ==
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
                                    importedConfig.settings.mainToolRail ==
                                            std::vector<prototype::MainToolAction>({
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
                    adw_dialog_close(ADW_DIALOG(preferencesDialog));
                    {
                        const auto anchor = prototype::Point{gtk_widget_get_width(canvas) / 2.0,
                                                             gtk_widget_get_height(canvas) / 2.0};
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
                    selectTool(Tool::Pen);
                    floatingToolboxSmokeReady = false;
                    floatingToolboxSmokeSource = g_timeout_add(
                            400,
                            +[](gpointer data) -> gboolean {
                                auto* self = static_cast<Prototype*>(data);
                                self->showFloatingToolbox(gtk_widget_get_width(self->canvas) / 2.0,
                                                          gtk_widget_get_height(self->canvas) / 2.0);
                                self->floatingToolboxSmokeReady = gtk_widget_get_visible(self->floatingToolbox) &&
                                                                  self->floatingToolboxToolButtons.size() >= 15;
                                self->floatingToolboxSmokeSource = 0;
                                return G_SOURCE_REMOVE;
                            },
                            this);
                    break;
                }
                case 4:
                    require(floatingToolboxSmokeReady, "Floating toolbox did not open with the expected tool actions");
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
                default:
                    require(floatingToolboxDismissSmokeReady,
                            "Floating toolbox did not close after the pointer-leave delay");
                    capture("preferences-edited");
                    std::cout << "PASS: Adw Preferences editing, legacy stylus mapping import and persistent GTK 4 settings\n";
                    gtk_window_close(GTK_WINDOW(window));
                    return G_SOURCE_REMOVE;
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
                                    std::abs(importedConfig.settings.zoomStep - 0.22) < 1e-8 &&
                                    std::abs(importedConfig.settings.snapRotationTolerance - 0.45) < 1e-8 &&
                                    importedConfig.settings.floatingToolboxRows[0] ==
                                            std::vector<std::string>({"SMART_SELECT", "PEN"}) &&
                                    importedConfig.settings.floatingToolboxRows[1] ==
                                            std::vector<std::string>({"ZOOM_SLIDER"}) &&
                                    importedConfig.settings.floatingToolboxRows[2] ==
                                            std::vector<std::string>({"FINE"}) &&
                                    importedConfig.settings.floatingToolboxRows[3] ==
                                            std::vector<std::string>({"DRAW_ARROW"}),
                            "GTK 4 preference edits were not restored at next startup");
                    openPreferences();
                    break;
                default:
                    require(adw_combo_row_get_selected(ADW_COMBO_ROW(control(PreferenceKey::Theme))) == 1 &&
                                    adw_combo_row_get_selected(
                                            ADW_COMBO_ROW(control(PreferenceKey::StylusButtonOne))) ==
                                            static_cast<guint>(prototype::ButtonToolAction::Pen) &&
                                    !adw_switch_row_get_active(
                                            ADW_SWITCH_ROW(control(PreferenceKey::PressureSensitivity))) &&
                                    adw_switch_row_get_active(
                                            ADW_SWITCH_ROW(control(PreferenceKey::PositionHighlight))),
                            "Preferences dialog did not display restored values");
                    require(Prototype::packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(
                                    control(PreferenceKey::CursorHighlightColor)))) == 0x7f123456U &&
                                    std::abs(gtk_spin_button_get_value(
                                                     GTK_SPIN_BUTTON(control(PreferenceKey::CursorHighlightRadius))) -
                                             17.0) < 1e-8 &&
                                    Prototype::packArgbColor(*gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(
                                            control(PreferenceKey::CursorHighlightBorderColor)))) == 0xc0abcdefU &&
                                    std::abs(gtk_spin_button_get_value(GTK_SPIN_BUTTON(
                                                     control(PreferenceKey::CursorHighlightBorderWidth))) -
                                             2.0) < 1e-8,
                            "Preferences dialog did not restore pointer highlight appearance settings");
                    require(!adw_switch_row_get_active(
                                    ADW_SWITCH_ROW(control(PreferenceKey::SnapRotation))),
                            "Preferences dialog did not restore the rotation snapping setting");
                    require(!adw_switch_row_get_active(ADW_SWITCH_ROW(control(PreferenceKey::SnapGrid))) &&
                                    std::abs(gtk_spin_button_get_value(
                                                     GTK_SPIN_BUTTON(control(PreferenceKey::SnapGridSize))) - 24.0) <
                                            1e-8 &&
                                    std::abs(gtk_spin_button_get_value(
                                                     GTK_SPIN_BUTTON(control(PreferenceKey::SnapGridTolerance))) -
                                             0.4) < 1e-8,
                            "Preferences dialog did not restore page-grid snapping values");
                    require(std::abs(gtk_spin_button_get_value(
                                             GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanSpeed))) - 31.0) < 1e-8 &&
                                    std::abs(gtk_spin_button_get_value(
                                             GTK_SPIN_BUTTON(control(PreferenceKey::EdgePanMaxMultiplier))) - 3.5) <
                                            1e-8,
                            "Preferences dialog did not restore selection edge-pan values");
                    require(std::string(gtk_editable_get_text(GTK_EDITABLE(
                                                    control(PreferenceKey::LatexDefaultText)))) ==
                                            "\\frac{\\alpha}{2}" &&
                                    prototype::preferences_detail::pathToUtf8(
                                            importedConfig.settings.latexTemplatePath) ==
                                            "/tmp/template with spaces.tex" &&
                                    std::string(gtk_editable_get_text(GTK_EDITABLE(
                                            control(PreferenceKey::LatexGeneratorCommand)))) ==
                                            "pdflatex -halt-on-error -interaction=nonstopmode '{}'" &&
                                    std::string(gtk_editable_get_text(GTK_EDITABLE(
                                            control(PreferenceKey::LatexExternalEditorCommand)))) ==
                                            "'/tmp/fake latex editor' --wait" &&
                                    !adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexAutoCheckDependencies))) &&
                                    adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexUseCustomEditorFont))) &&
                                    !adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexEditorWordWrap))) &&
                                    !adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexSourceViewAutoIndent))) &&
                                    !adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexSourceViewSyntaxHighlight))) &&
                                    adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexSourceViewShowLineNumbers))) &&
                                    adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexUseExternalEditor))) &&
                                    adw_switch_row_get_active(ADW_SWITCH_ROW(
                                            control(PreferenceKey::LatexExternalEditorAutoConfirm))) &&
                                    std::string(gtk_editable_get_text(GTK_EDITABLE(
                                            control(PreferenceKey::LatexTemporaryFileExtension)))) == "tex",
                            "Preferences dialog did not restore LaTeX settings and editor options");
                    capture("preferences-reloaded");
                    std::cout << "PASS: GTK 4 preferences survive application restart\n";
                    adw_dialog_close(ADW_DIALOG(preferencesDialog));
                    gtk_window_close(GTK_WINDOW(window));
                    return G_SOURCE_REMOVE;
            }
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    gboolean smokeImages() {
        require(imageVerifyMode == "insert-resize-save-reopen" && !imageVerifySource.empty(),
                "Image verification requires a source fixture and supported mode");
        if ((imageVerifyStep == 5 || imageVerifyStep >= 8) && (busy || autosaving)) {
            return G_SOURCE_CONTINUE;
        }
        switch (imageVerifyStep++) {
            case 0:
                if (!nativeSession || !gtk_widget_get_sensitive(imageButton)) {
                    --imageVerifyStep;
                    return G_SOURCE_CONTINUE;
                }
                require(state.images.empty(), "Image verification document should start without editable images");
                g_signal_emit_by_name(imageButton, "clicked");
                require(choosingFile, "Insert Image action did not open its native image chooser");
                break;
            case 1:
                if (!cancelPrivateChooser()) {
                    require(++chooserWaits < 60, "Insert Image chooser did not appear");
                    --imageVerifyStep;
                    return G_SOURCE_CONTINUE;
                }
                break;
            case 2:
                require(!choosingFile, "Cancelling Insert Image did not close the image chooser");
                insertImageFromPath(imageVerifySource);
                require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0} && modified(),
                        "Insert Image did not add a selected, undoable image");
                break;
            case 3: {
                capture("image-inserted");
                break;
            }
            case 4: {
                state.clearSelection();
                activateSelection(Tool::SelectRectangle);
                const auto selectionBounds = prototype::canvasImageBounds(state.images.at(0));
                const auto selectionStart = state.pageScreenPoint(state.page, selectionBounds.x0 - 2,
                                                                   selectionBounds.y0 - 2);
                const auto selectionEnd = state.pageScreenPoint(state.page, selectionBounds.x1 + 2,
                                                                 selectionBounds.y1 + 2);
                begin(Device::Mouse, selectionStart.x, selectionStart.y);
                motion(Device::Mouse, selectionEnd.x, selectionEnd.y);
                finish(Device::Mouse);
                require(state.selectedImages == std::vector<size_t>{0},
                        "Rectangle selection did not select the inserted image");
                const auto originalScale = state.images.at(0).transform.xx;
                const auto corners = prototype::canvasImageCorners(state.images.at(0));
                const auto start = state.pageScreenPoint(state.images[0].page, corners[2].x, corners[2].y);
                const auto releasePoint = state.pageScreenPoint(state.images[0].page, corners[2].x + 10,
                                                                corners[2].y + 7.5);
                begin(Device::Mouse, start.x, start.y);
                motion(Device::Mouse, releasePoint.x, releasePoint.y);
                finish(Device::Mouse);
                require(state.images[0].transform.xx > originalScale && state.canUndo(),
                        "Dragging an image corner did not resize the image");
                capture("image-resized");
                break;
            }
            case 5: {
                const auto& image = state.images.at(0);
                const auto center = prototype::canvasImagePoint(image, image.pixelWidth / 2.0,
                                                                image.pixelHeight / 2.0);
                const auto start = state.pageScreenPoint(image.page, center.x, center.y);
                const auto releasePoint = state.pageScreenPoint(image.page, center.x + 24, center.y + 19);
                const double originalX = image.transform.x;
                const double originalY = image.transform.y;
                begin(Device::Mouse, start.x, start.y);
                motion(Device::Mouse, releasePoint.x, releasePoint.y);
                finish(Device::Mouse);
                require(std::abs(state.images[0].transform.x - originalX - 24) < 1e-6 &&
                                std::abs(state.images[0].transform.y - originalY - 19) < 1e-6,
                        "Dragging an image body did not move the image");
                const auto movedTransform = state.images[0].transform;
                const auto centerBeforeRotation = prototype::canvasImageCenter(state.images[0]);
                const auto rotationHandle = state.selectionHandles()[8];
                const double handleRadius = std::hypot(rotationHandle.x - centerBeforeRotation.x,
                                                       rotationHandle.y - centerBeforeRotation.y);
                const double startAngle = std::atan2(rotationHandle.y - centerBeforeRotation.y,
                                                     rotationHandle.x - centerBeforeRotation.x);
                const double targetAngle = startAngle + G_PI / 6.0 + 0.01;
                const prototype::Point rotatedHandle{centerBeforeRotation.x + handleRadius * std::cos(targetAngle),
                                                     centerBeforeRotation.y + handleRadius * std::sin(targetAngle)};
                const auto rotationStart = state.pageScreenPoint(state.images[0].page, rotationHandle.x,
                                                                 rotationHandle.y);
                const auto rotationEnd = state.pageScreenPoint(state.images[0].page, rotatedHandle.x,
                                                               rotatedHandle.y);
                begin(Device::Mouse, rotationStart.x, rotationStart.y);
                motion(Device::Mouse, rotationEnd.x, rotationEnd.y);
                finish(Device::Mouse);
                require(std::abs(state.images[0].transform.xy) > 1e-6 &&
                                std::abs(state.images[0].transform.yx) > 1e-6 &&
                                std::abs(std::atan2(state.images[0].transform.yx, state.images[0].transform.xx) -
                                         G_PI / 6.0) < 1e-6 &&
                                std::hypot(prototype::canvasImageCenter(state.images[0]).x - centerBeforeRotation.x,
                                           prototype::canvasImageCenter(state.images[0]).y - centerBeforeRotation.y) < 1e-6,
                        "Image rotation should snap to 30 degrees around its center");
                const auto snappedTransform = state.images[0].transform;
                const auto snappedHandle = state.selectionHandles()[8];
                const double snappedHandleAngle = std::atan2(snappedHandle.y - centerBeforeRotation.y,
                                                             snappedHandle.x - centerBeforeRotation.x);
                const prototype::Point unsnappedHandle{
                        centerBeforeRotation.x + handleRadius * std::cos(snappedHandleAngle + 2 * G_PI / 180.0),
                        centerBeforeRotation.y + handleRadius * std::sin(snappedHandleAngle + 2 * G_PI / 180.0)};
                const auto unsnappedStart = state.pageScreenPoint(state.images[0].page, snappedHandle.x,
                                                                   snappedHandle.y);
                const auto unsnappedEnd = state.pageScreenPoint(state.images[0].page, unsnappedHandle.x,
                                                                 unsnappedHandle.y);
                input.setModifiers(false, false, true);
                begin(Device::Mouse, unsnappedStart.x, unsnappedStart.y);
                motion(Device::Mouse, unsnappedEnd.x, unsnappedEnd.y);
                finish(Device::Mouse);
                input.setModifiers(false, false, false);
                const double altRotation = std::atan2(state.images[0].transform.yx, state.images[0].transform.xx);
                require(std::abs(altRotation - (G_PI / 6.0 + 2 * G_PI / 180.0)) < 1e-6,
                        "Holding Alt should temporarily disable rotation snapping");
                const auto rotatedTransform = state.images[0].transform;
                state.undo();
                require(std::abs(state.images[0].transform.xx - snappedTransform.xx) < 1e-9 &&
                                std::abs(state.images[0].transform.xy - snappedTransform.xy) < 1e-9 &&
                                std::abs(state.images[0].transform.yx - snappedTransform.yx) < 1e-9 &&
                                std::abs(state.images[0].transform.yy - snappedTransform.yy) < 1e-9,
                        "Undo should restore the snapped image rotation after an Alt rotation");
                state.undo();
                require(std::abs(state.images[0].transform.xx - movedTransform.xx) < 1e-9 &&
                                std::abs(state.images[0].transform.xy - movedTransform.xy) < 1e-9 &&
                                std::abs(state.images[0].transform.yx - movedTransform.yx) < 1e-9 &&
                                std::abs(state.images[0].transform.yy - movedTransform.yy) < 1e-9,
                        "Undo should restore the image transform from before rotation");
                state.redo();
                require(std::abs(state.images[0].transform.xx - snappedTransform.xx) < 1e-9 &&
                                std::abs(state.images[0].transform.xy - snappedTransform.xy) < 1e-9 &&
                                std::abs(state.images[0].transform.yx - snappedTransform.yx) < 1e-9 &&
                                std::abs(state.images[0].transform.yy - snappedTransform.yy) < 1e-9,
                        "Redo should restore the snapped image rotation");
                state.redo();
                require(std::abs(state.images[0].transform.xy - rotatedTransform.xy) < 1e-9 &&
                                std::abs(state.images[0].transform.yx - rotatedTransform.yx) < 1e-9,
                        "Redo should restore the unsnapped Alt image rotation");
                imageVerifyExpected = state.images[0].transform;
                saveCopyTo((std::filesystem::path(options.smokeDirectory) / "image-saved.xopp").string());
                break;
            }
            case 6: {
                if (busy || autosaving) {
                    --imageVerifyStep;
                    return G_SOURCE_CONTINUE;
                }
                const auto path = (std::filesystem::path(options.smokeDirectory) / "image-saved.xopp").string();
                require(!modified(), "Saving a document with an edited image left it modified");
                require(savedCopyPath == path, "Saving an image copy did not update its destination");
                require(std::filesystem::is_regular_file(path), "Saving an image copy did not create the file");
                capture("image-rotated");
                requestOpenDocument(path);
                break;
            }
            case 7: {
                require(state.images.size() == 1 && state.images[0].encodedData.size() > 0 &&
                                std::abs(state.images[0].transform.xx - imageVerifyExpected.xx) < 1e-4 &&
                                std::abs(state.images[0].transform.yx - imageVerifyExpected.yx) < 1e-4 &&
                                std::abs(state.images[0].transform.xy - imageVerifyExpected.xy) < 1e-4 &&
                                std::abs(state.images[0].transform.yy - imageVerifyExpected.yy) < 1e-4 &&
                                std::abs(state.images[0].transform.x - imageVerifyExpected.x) < 1e-4 &&
                                std::abs(state.images[0].transform.y - imageVerifyExpected.y) < 1e-4,
                        "Saving and reopening changed the native image or its resized placement");
                capture("image-reopened");
                const auto pdf = (std::filesystem::path(options.smokeDirectory) / "image-exported.pdf").string();
                exportPdfTo(pdf);
                break;
            }
            default: {
                const auto pdf = (std::filesystem::path(options.smokeDirectory) / "image-exported.pdf").string();
                require(exportedPdfPath == pdf && std::filesystem::is_regular_file(pdf),
                        "Exporting the image document did not create a PDF");
                if (imageVerifyStep == 9) {
                    activateSelection(Tool::SmartSelect);
                    state.selectedImages = {0};
                    copySelection(true);
                    require(state.images.empty() && state.selectedImages.empty(),
                            "Cut did not remove the selected image after copying it");
                    auto* clipboard = gtk_widget_get_clipboard(canvas);
                    auto* formats = gdk_clipboard_get_formats(clipboard);
                    require(gdk_content_formats_contain_mime_type(formats, prototype::clipboard::mime) &&
                                    gdk_content_formats_contain_mime_type(formats, "image/png"),
                            "Image copy did not offer both the editable native object and PNG");
                    pasteSelection();
                    require(busy, "Native image paste did not begin its asynchronous clipboard read");
                    break;
                }
                if (imageVerifyStep == 10) {
                    require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0} &&
                                    state.images[0].encodedData.size() > 0,
                            "Native image clipboard paste did not restore an editable image");
                    state.undo();
                    require(state.images.empty(), "Undo did not remove the pasted image");
                    state.redo();
                    require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0},
                            "Redo did not restore the pasted image selection");

                    const auto pngData = encodeImageForClipboard(state.images[0]);
                    auto* bytes = g_bytes_new(pngData.data(), pngData.size());
                    auto* provider = gdk_content_provider_new_for_bytes("image/png", bytes);
                    g_bytes_unref(bytes);
                    require(gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas), provider),
                            "Could not offer a PNG-only clipboard fixture");
                    g_object_unref(provider);
                    state.deleteImage(0);
                    pasteSelection();
                    require(busy, "PNG clipboard paste did not begin its asynchronous read");
                    break;
                }
                if (imageVerifyStep == 11) {
                    require(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0} &&
                                    prototype::validCanvasImage(state.images[0]),
                            "A standard PNG clipboard image was not pasted as an editable object");
                    state.undo();
                    selectedImageIndex.reset();
                    require(state.images.empty(), "Undo did not remove the PNG-pasted image");
                    state.redo();
                    require(state.images.size() == 1, "Redo did not restore the PNG-pasted image");
                    capture("image-png-pasted");

                    duplicateSelection();
                    require(state.images.size() == 2 && state.selectedImages == std::vector<size_t>{1},
                            "Single-image duplicate did not add an editable image");
                    state.undo();
                    require(state.images.size() == 1, "Undo did not remove the image duplicate");
                    state.redo();
                    require(state.images.size() == 2, "Redo did not restore the image duplicate");

                    state.clearSelection();
                    state.selectedImages = {0, 1};
                    imageVerifyMultiDeltaX = state.images[1].transform.x - state.images[0].transform.x;
                    imageVerifyMultiDeltaY = state.images[1].transform.y - state.images[0].transform.y;
                    duplicateSelection();
                    require(state.images.size() == 4 && state.selectedImages == std::vector<size_t>{2, 3} &&
                                    std::abs(state.images[3].transform.x - state.images[2].transform.x -
                                             imageVerifyMultiDeltaX) < 1e-6 &&
                                    std::abs(state.images[3].transform.y - state.images[2].transform.y -
                                             imageVerifyMultiDeltaY) < 1e-6,
                            "Duplicating a multi-image selection should preserve its relative layout");
                    state.undo();
                    require(state.images.size() == 2, "One undo did not remove the complete image duplicate group");
                    state.redo();
                    require(state.images.size() == 4 && state.selectedImages == std::vector<size_t>{2, 3},
                            "Redo did not restore the complete image duplicate group");

                    state.clearSelection();
                    state.selectedImages = {0, 1};
                    copySelection(true);
                    require(state.images.size() == 2 && state.selectedImages.empty(),
                            "Cut did not remove the selected images as one operation");
                    auto* clipboard = gtk_widget_get_clipboard(canvas);
                    auto* formats = gdk_clipboard_get_formats(clipboard);
                    require(gdk_content_formats_contain_mime_type(formats, prototype::clipboard::mime) &&
                                    gdk_content_formats_contain_mime_type(formats, "image/png"),
                            "Multi-image copy did not offer native objects and a composite PNG");
                    state.undo();
                    require(state.images.size() == 4, "One undo did not restore the complete image cut group");
                    state.redo();
                    require(state.images.size() == 2, "Redo did not remove the complete image cut group");
                    pasteSelection();
                    require(busy, "Multi-image clipboard paste did not begin its asynchronous read");
                    break;
                }
                require(imageVerifyStep == 12 && state.images.size() == 4 &&
                                state.selectedImages == std::vector<size_t>{2, 3} &&
                                std::abs(state.images[3].transform.x - state.images[2].transform.x -
                                         imageVerifyMultiDeltaX) < 1e-6 &&
                                std::abs(state.images[3].transform.y - state.images[2].transform.y -
                                         imageVerifyMultiDeltaY) < 1e-6,
                        "Native multi-image clipboard paste did not preserve both images and their relative layout");
                state.undo();
                require(state.images.size() == 2, "One undo did not remove the complete image paste group");
                state.redo();
                require(state.images.size() == 4 && state.selectedImages == std::vector<size_t>{2, 3},
                        "Redo did not restore the complete image paste group");
                capture("image-multi-pasted");
                std::cout << "PASS: image insertion, resize, move, rotate, native save/reopen, PDF export, PNG "
                             "paste, and multi-image native copy/cut/paste, duplicate and grouped undo/redo\n";
                closeWindow();
                return G_SOURCE_REMOVE;
            }
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    gboolean smokeSettingsVerify() {
        if (settingsVerifyMode == "fallback") {
            require(importedConfig.usedSettingsFallback, "Expected corrupt settings.xml to trigger fallback");
            require(importedConfig.palette.usedFallback && paletteSwatchButtons.size() == 11,
                    "Expected corrupt or unreferenced palette to fall back to 11-color default");
            require(state.brush.tool == Tool::Pen && std::abs(state.brush.width - 1.41) < 1e-6 &&
                            state.eraserMode == prototype::EraserMode::Standard &&
                            std::abs(state.zoomStep - 0.10) < 1e-6 && autosaveSource != 0,
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
                require(paletteSwatchButtons.size() == 11 &&
                                importedConfig.palette.paletteName == "Xournal++ Palette",
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
                require(!gtk_widget_get_visible(palettePopover) &&
                                std::abs(state.brush.color[0] - 205 / 255.0) < 1e-2 &&
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

    gboolean smokeLaserPointers() {
        switch (laserVerifyStep++) {
            case 0: {
                require(bool(nativeSession), "Laser verification requires a native document fixture");
                laserInitialInkCount = state.ink.size();
                laserInitialRevision = state.revision();
                laserInitialUndo = state.canUndo();
                selectTool(Tool::LaserPointerPen);
                require(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(laserButtons[0])) &&
                                std::abs(state.brush.width - 2.4) < 1e-6,
                        "Laser Pointer Pen toolbar selection or default preset mismatch");
                const auto from = state.pageScreenPoint(state.page, 70, 110);
                const auto to = state.pageScreenPoint(state.page, 210, 150);
                require(input.begin(Device::Stylus, {from.x, from.y, 0.25}),
                        "Laser Pointer Pen did not accept stylus input");
                input.motion(Device::Stylus, {to.x, to.y, 0.8});
                input.finish(Device::Stylus, 0.9);
                require(state.laserTrails.size() == 1 && state.laserTrails.front().strokes.size() == 1 &&
                                state.laserTrails.front().strokes.front().brush.tool == Tool::LaserPointerPen &&
                                state.ink.size() == laserInitialInkCount && state.revision() == laserInitialRevision &&
                                state.canUndo() == laserInitialUndo,
                        "Laser Pointer Pen wrote into the document or undo history");
                update();
                waitForNextPaint();
                capture("laser-pen");
                break;
            }
            case 1: {
                selectTool(Tool::LaserPointerHighlighter);
                require(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(laserButtons[1])) &&
                                std::abs(state.brush.width - 8.5) < 1e-6,
                        "Laser Pointer Highlighter toolbar selection or default preset mismatch");
                const auto from = state.pageScreenPoint(state.page, 70, 190);
                const auto to = state.pageScreenPoint(state.page, 220, 190);
                require(input.begin(Device::Mouse, {from.x, from.y, std::nullopt}),
                        "Laser Pointer Highlighter did not accept mouse input");
                input.motion(Device::Mouse, {to.x, to.y, std::nullopt});
                input.finish(Device::Mouse);
                require(state.laserTrails.size() == 1 && state.laserTrails.front().strokes.size() == 2 &&
                                state.laserTrails.front().strokes.back().brush.tool == Tool::LaserPointerHighlighter &&
                                !state.laserTrails.front().strokes.back().pressureSensitive &&
                                state.ink.size() == laserInitialInkCount && state.revision() == laserInitialRevision,
                        "Laser highlighter did not share a transient trail without changing the document");
                update();
                waitForNextPaint();
                capture("laser-highlighter");
                break;
            }
            case 2: {
                require(state.laserTrails.size() == 1 && state.laserTrails.front().fadeStart,
                        "Completed laser marks did not schedule their fade delay");
                require(state.laserTrails.front().alpha == 255,
                        "Laser marks began fading before the configured delay elapsed");
                break;
            }
            case 3:
                if (state.laserTrails.size() == 1 && state.laserTrails.front().alpha == 255) {
                    --laserVerifyStep;
                    return G_SOURCE_CONTINUE;
                }
                require(state.laserTrails.size() == 1 && state.laserTrails.front().alpha < 255,
                        "The real GTK timeout did not start fading the laser trail");
                capture("laser-fading");
                break;
            case 4:
                if (!state.laserTrails.empty()) {
                    --laserVerifyStep;
                    return G_SOURCE_CONTINUE;
                }
                capture("laser-faded");
                require(state.ink.size() == laserInitialInkCount && state.revision() == laserInitialRevision &&
                                state.canUndo() == laserInitialUndo,
                        "Faded laser marks changed saved or undoable document state");
                break;
            case 5:
                laserOutputPath =
                        (std::filesystem::path(options.smokeDirectory) / "laser-pointer-saved.xopp").string();
                saveCopyTo(laserOutputPath, [this] { laserSaveComplete = true; });
                require(busy, "Saving after laser input did not start the worker operation");
                break;
            default:
                require(laserSaveComplete && std::filesystem::is_regular_file(laserOutputPath),
                        "The native document save after laser input did not finish");
                std::cout << "PASS: GTK laser pointer pen/highlighter trails fade without entering document history\n";
                closeWindow();
                return G_SOURCE_REMOVE;
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    gboolean smokePlayObject() {
        switch (playObjectVerifyStep++) {
            case 0: {
                require(nativeSession && !audioTargets.empty() && !audioLayerDocuments.empty(),
                        "Play Object verification requires native audio-target and faded-layer previews");
                require(bool(audioPlayback), "Play Object verification has no audio playback controller");
                const auto& target = audioTargets.front();
                double localX = 0;
                double localY = 0;
                if (target.shape == prototype::AudioTargetShape::Stroke) {
                    const auto& point = target.points[target.points.size() / 2];
                    localX = point.x;
                    localY = point.y;
                } else {
                    localX = target.xx * target.naturalWidth / 2.0 + target.xy * target.naturalHeight / 2.0 +
                             target.x;
                    localY = target.yx * target.naturalWidth / 2.0 + target.yy * target.naturalHeight / 2.0 +
                             target.y;
                }
                selectTool(Tool::PlayObject);
                waitForNextPaint();
                capture("play-object-faded");
                const auto click = state.pageScreenPoint(target.page, localX, localY);
                begin(Device::Mouse, click.x, click.y);
                require(audioPlayback->isActive() && gtk_widget_get_visible(audioControls),
                        "Clicking an audio-linked object did not start playback and show its controls");
                g_signal_emit_by_name(audioPauseButton, "clicked");
                require(audioPlayback->isPaused(), "Play Object playback did not pause");
                g_signal_emit_by_name(audioBackButton, "clicked");
                g_signal_emit_by_name(audioForwardButton, "clicked");
                g_signal_emit_by_name(audioPauseButton, "clicked");
                require(!audioPlayback->isPaused(), "Play Object playback did not resume");
                audioPlayback->stop();
                require(!audioPlayback->isActive() && !gtk_widget_get_visible(audioControls),
                        "Stopping audio did not hide the playback controls");
                selectTool(Tool::Pen);
                waitForNextPaint();
                capture("play-object-normal");
                selectTool(Tool::PlayObject);
                waitForNextPaint();
                capture("play-object-faded-again");
                break;
            }
            case 1: {
                const auto outside = state.pageScreenPoint(state.page, 25, 700);
                begin(Device::Mouse, outside.x, outside.y);
                require(!audioPlayback->isActive(), "Clicking away from audio-linked objects started playback");
                playObjectSaveOutput =
                        (std::filesystem::path(options.smokeDirectory) / "play-object-saved.xopp").string();
                saveCopyTo(playObjectSaveOutput, [this] { playObjectSaveComplete = true; });
                require(busy, "Saving the audio-linked native document did not start");
                break;
            }
            default:
                require(playObjectSaveComplete && std::filesystem::is_regular_file(playObjectSaveOutput),
                        "The native document save after Play Object verification did not finish");
                std::cout << "PASS: GTK Play Object hit testing, playback controls, fading, and native save\n";
                closeWindow();
                return G_SOURCE_REMOVE;
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    gboolean smokeLinks() {
        switch (linkVerifyStep++) {
            case 0: {
                require(nativeSession && state.texts.size() >= 1 &&
                                std::any_of(state.texts.begin(), state.texts.end(),
                                            [](const prototype::TextItem& item) { return item.isLink; }),
                        "Link verification requires an editable native link in the source document");
                const auto original = std::find_if(state.texts.begin(), state.texts.end(),
                                                   [](const prototype::TextItem& item) { return item.isLink; });
                const size_t originalIndex = static_cast<size_t>(std::distance(state.texts.begin(), original));
                const auto originalUrl = original->linkUrl;
                const auto originalText = original->text;
                const auto linkPoint = state.pageScreenPoint(original->page, original->x + 2, original->y + 2);

                selectTool(Tool::Link);
                updateLinkHover(linkPoint.x, linkPoint.y);
                require(linkInfoPopover && gtk_widget_get_visible(linkInfoPopover),
                        "Hovering an existing link did not show its URL popover");
                handleLinkClick(linkPoint.x, linkPoint.y, 1, true);
                require(openedLinkForTest == originalUrl,
                        "Control-click did not activate the URL for the link under the pointer");
                handleLinkClick(linkPoint.x, linkPoint.y, 2, false);
                require(messageDialog && editingLinkIndex == originalIndex,
                        "Double-clicking an existing link did not open its editor");
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), "Edited GTK4 link", -1);
                gtk_drop_down_set_selected(linkPrefixDropdown, 0);
                gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), "migration.example/phase29");
                gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), "Sans");
                gtk_spin_button_set_value(linkFontSize, 15);
                gtk_drop_down_set_selected(linkAlignmentDropdown, 1);
                answerDialog("save");
                require(state.texts[originalIndex].isLink && state.texts[originalIndex].text == "Edited GTK4 link" &&
                                state.texts[originalIndex].linkUrl == "https://migration.example/phase29" &&
                                state.texts[originalIndex].alignment == 1 && state.canUndo(),
                        "Editing a native link did not preserve its type, save its fields and enter undo history");
                const auto editedPoint = state.pageScreenPoint(state.texts[originalIndex].page,
                                                               state.texts[originalIndex].x + 2,
                                                               state.texts[originalIndex].y + 2);
                openedLinkForTest.clear();
                handleLinkClick(editedPoint.x, editedPoint.y, 1, true);
                require(openedLinkForTest == "https://migration.example/phase29",
                        "Control-click after editing did not use the saved URL");
                state.undo();
                require(state.texts[originalIndex].text == originalText &&
                                state.texts[originalIndex].linkUrl == originalUrl,
                        "Undo did not restore the original native link");
                state.redo();
                require(state.texts[originalIndex].text == "Edited GTK4 link" &&
                                state.texts[originalIndex].linkUrl == "https://migration.example/phase29",
                        "Redo did not restore the edited native link");
                capture("link-edited");

                const size_t beforeInsert = state.texts.size();
                const auto newPosition = state.pageScreenPoint(state.page, 330, 280);
                handleLinkClick(newPosition.x, newPosition.y, 1, false);
                require(messageDialog && !editingLinkIndex, "Clicking empty page space did not open the Add Link form");
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(linkTextInput), "Contact", -1);
                gtk_drop_down_set_selected(linkPrefixDropdown, 2);
                gtk_editable_set_text(GTK_EDITABLE(linkUrlInput), "migration@example.org");
                gtk_editable_set_text(GTK_EDITABLE(linkFontEntry), "Sans");
                gtk_spin_button_set_value(linkFontSize, 12);
                gtk_drop_down_set_selected(linkAlignmentDropdown, 0);
                answerDialog("save");
                require(state.texts.size() == beforeInsert + 1 && state.texts.back().isLink &&
                                state.texts.back().text == "Contact" &&
                                state.texts.back().linkUrl == "mailto:migration@example.org" && state.canUndo(),
                        "Creating a link did not add a native, undoable link object");
                const auto newIndex = state.texts.size() - 1;
                state.undo();
                require(state.texts.size() == beforeInsert, "Undo did not remove the newly created link");
                state.redo();
                require(state.texts.size() == beforeInsert + 1 && state.texts[newIndex].linkUrl ==
                                "mailto:migration@example.org",
                        "Redo did not restore the newly created link");
                capture("link-created");

                linkOutputPath = (std::filesystem::path(options.smokeDirectory) / "links-saved.xopp").string();
                saveCopyTo(linkOutputPath, [this] { linkSaveComplete = true; });
                require(busy, "Saving edited links did not start a document-worker operation");
                break;
            }
            default: {
                require(linkSaveComplete && std::filesystem::is_regular_file(linkOutputPath),
                        "The native link save did not finish");
                capture("link-saved");
                std::cout << "PASS: GTK native links create, edit, activate, undo/redo and save\n";
                closeWindow();
                return G_SOURCE_REMOVE;
            }
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    gboolean smoke() {
        if (busy || deferredAction) {
            require(++documentWaits < 120, "Document operation did not finish");
            return G_SOURCE_CONTINUE;
        }
        documentWaits = 0;
        if (g_get_monotonic_time() - lastDocumentCompletion < 250000) {
            return G_SOURCE_CONTINUE;  // Let disabled-control/dialog transitions settle before snapshots.
        }
        if (paints <= lastSmokePaint) {
            if (++paintWaits < 60) {
                return G_SOURCE_CONTINUE;
            }
            throw std::runtime_error("Window did not finish painting the requested state");
        }
        paintWaits = 0;
        if (preferencesVerifyMode.empty() && imageVerifyMode.empty() && (!frames || pageCache.empty())) {
            require(++smokeWaits < 60, "Canvas has not rendered");
            return G_SOURCE_CONTINUE;
        }
        if (!preferencesVerifyMode.empty()) {
            return smokePreferencesVerify();
        }
        if (!settingsVerifyMode.empty()) {
            return smokeSettingsVerify();
        }
        if (!layerVerifyMode.empty()) {
            return smokeLayers();
        }
        if (!selectionVerifyMode.empty()) {
            return smokeSelectionModes();
        }
        if (!pdfTextSelectionVerifyMode.empty()) {
            return smokePdfTextSelection();
        }
        if (!linkVerifyMode.empty()) {
            return smokeLinks();
        }
        if (!laserVerifyMode.empty()) {
            return smokeLaserPointers();
        }
        if (!playObjectVerifyMode.empty()) {
            return smokePlayObject();
        }
        if (!imageVerifyMode.empty()) {
            return smokeImages();
        }
        if (documentSmokeStep >= 0) {
            return smokeDocuments();
        }
        const auto x = state.offsetX + 80 * state.zoom;
        const auto y = state.offsetY + 80 * state.zoom;
        bool finishSmoke = false;
        switch (smokeStep++) {
            case 0: {
                auto* native = gtk_widget_get_native(canvas);
                const graphene_point_t origin{0, 0};
                graphene_point_t canvasOrigin;
                require(gtk_widget_compute_point(canvas, GTK_WIDGET(native), &origin, &canvasOrigin),
                        "Cannot locate canvas in native window");
                double tx, ty;
                gtk_native_get_surface_transform(native, &tx, &ty);
                const auto converted = surfacePoint(canvasOrigin.x + 40 - tx, canvasOrigin.y + 60 - ty);
                require(converted && std::abs(converted->x - 40) < 0.01 && std::abs(converted->y - 60) < 0.01,
                        "Pointer anchor failed to account for sidebar and toolbar offsets");
                capture("initial");
                baselineRasters = rasterizations;
                require(std::abs(state.brush.width - 1.41) < 1e-6 &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 2u,
                        "Initial Pen preset did not match Medium 1.41pt");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 4u);
                require(std::abs(state.brush.width - 5.67) < 1e-6, "Very Thick Pen preset did not apply 5.67pt");
                const GdkRGBA red{1, 0, 0, 1};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &red);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(widthButton), 6);
                require(gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 5u,
                        "Custom width did not switch preset selector to Custom");
                g_signal_emit_by_name(mouse, "drag-begin", x, y);
                g_signal_emit_by_name(mouse, "drag-update", 100.0, 30.0);
                g_signal_emit_by_name(mouse, "drag-end", 160.0, 0.0);
                require(state.ink.size() == 1 && state.ink[0].brush.width == 6 && state.ink[0].brush.color[0] == 1,
                        "Color or width controls did not style the stroke");
                break;
            }
            case 1: {
                require(rasterizations == baselineRasters, "Ink unnecessarily rerasterized PDF");
                capture("ink");
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_z, 0u, GDK_CONTROL_MASK, &handled);
                require(handled && state.ink.empty() && gtk_widget_get_sensitive(redoButton), "Keyboard undo failed");
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Z, 0u,
                                      GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
                require(handled && state.ink.size() == 1, "Keyboard redo failed");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[1]), true);
                require(state.brush.tool == Tool::Highlighter && std::abs(state.brush.width - 8.50) < 1e-6 &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 2u,
                        "Highlighter did not restore its Medium 8.50pt preset");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
                require(std::abs(state.brush.width - 19.84) < 1e-6,
                        "Highlighter Thick preset did not apply 19.84pt");
                g_signal_emit_by_name(mouse, "drag-begin", x, y + 50);
                g_signal_emit_by_name(mouse, "drag-update", 100.0, 0.0);
                g_signal_emit_by_name(mouse, "drag-end", 160.0, 0.0);
                require(state.ink.size() == 2 && state.ink[1].brush.tool == Tool::Highlighter,
                        "Highlighter controller did not commit ink");
                break;
            }
            case 2:
                require(rasterizations == baselineRasters, "Brush changes unnecessarily rerasterized PDF");
                capture("tools");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                require(state.brush.width == 6 && state.brush.color[0] == 1 &&
                                gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 5u,
                        "Pen custom settings were lost on tool switch");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
                require(!gtk_widget_get_visible(colorBox) && gtk_widget_get_visible(eraserModeDropdown) &&
                                gtk_widget_get_sensitive(widthButton) && std::abs(state.brush.width - 8.50) < 1e-6 &&
                                state.eraserMode == prototype::EraserMode::Standard,
                        "Eraser did not expose mode and size controls with Medium 8.50pt preset");
                g_signal_emit_by_name(mouse, "drag-begin", x + 50, y + 15);
                g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
                require(state.ink.size() == 3, "Standard eraser control did not split pen ink");
                break;
            case 3:
                capture("eraser-split");
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.ink.size() == 2, "Undo button did not restore split pen ink");
                g_signal_emit_by_name(redoButton, "clicked");
                require(state.ink.size() == 3, "Redo button did not split pen ink again");
                g_signal_emit_by_name(undoButton, "clicked");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 1u);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
                require(state.eraserMode == prototype::EraserMode::Whiteout &&
                                std::abs(state.brush.width - 12.0) < 1e-6,
                        "Eraser mode or Thick preset did not update state");
                g_signal_emit_by_name(mouse, "drag-begin", x + 35, y - 5);
                g_signal_emit_by_name(mouse, "drag-update", 30.0, 90.0);
                g_signal_emit_by_name(mouse, "drag-end", 30.0, 90.0);
                require(state.ink.size() == 3 && state.ink.back().brush.tool == Tool::Eraser,
                        "Whiteout eraser did not commit whiteout ink");
                break;
            case 4:
                capture("eraser-whiteout");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 2u);
                require(state.eraserMode == prototype::EraserMode::DeleteStroke,
                        "Delete Stroke mode was not selected");
                g_signal_emit_by_name(mouse, "drag-begin", x + 65, y + 85);
                g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
                require(state.ink.size() == 2, "Delete Stroke eraser did not remove whiteout stroke");
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.ink.size() == 3, "Undo did not restore deleted whiteout stroke");
                g_signal_emit_by_name(redoButton, "clicked");
                require(state.ink.size() == 2, "Redo did not remove whiteout stroke again");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 2u);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                g_signal_emit_by_name(mouse, "drag-begin", x, y);
                g_signal_emit_by_name(mouse, "cancel", nullptr);
                require(!state.current, "Gesture cancellation left ink active");
                checkInputParity(x, y);
                g_signal_emit_by_name(pinch, "begin", nullptr);
                g_signal_emit_by_name(pinch, "scale-changed", 1.5);
                g_signal_emit_by_name(pinch, "end", nullptr);
                require(!state.pinching && state.scrollBlockedUntil > g_get_monotonic_time(), "Pinch cleanup failed");
                break;
            case 5: {
                require(rasterizations > baselineRasters, "Zoom did not refresh PDF resolution");
                capture("zoom");
                const auto& cached1x = ensureCache(1);
                const auto width1x = cairo_image_surface_get_width(cached1x.surface.get());
                const auto& cached2x = ensureCache(2);
                require(cairo_image_surface_get_width(cached2x.surface.get()) >= width1x * 2 - 1,
                        "2x cache must preserve logical page size and increase resolution");
                require(cairo_image_surface_get_width(cached2x.surface.get()) <= 4096 &&
                                cairo_image_surface_get_height(cached2x.surface.get()) <= 4096,
                        "Cache exceeds dimension limit");
                smokeDrawingTools();
                if (pageCount > 1) {
                    const double zoomBeforeNav = state.zoom;
                    gtk_list_box_select_row(pageList, gtk_list_box_get_row_at_index(pageList, 1));
                    require(state.page == 1 && std::abs(state.zoom - zoomBeforeNav) < 1e-9 && state.ink.size() == 2,
                            "Sidebar navigation lost session ink or reset zoom");
                }
                gtk_widget_set_halign(contentView, GTK_ALIGN_CENTER);
                gtk_widget_set_size_request(contentView, 520, -1);
                fitted = false;
                update();
                break;
            }
            case 6:
                require(adw_overlay_split_view_get_collapsed(splitView), "Narrow sidebar did not collapse");
                require(gtk_widget_get_width(canvas) < 720, "Narrow viewport did not resize");
                capture("narrow", contentView);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebarButton), true);
                require(adw_overlay_split_view_get_show_sidebar(splitView), "Sidebar toggle did not open overlay");
                break;
            case 7:
                break;  // Let the overlay animation finish before capturing it.
            default: {
                smokeCacheBytes = totalCacheBytes();
                capture("sidebar", contentView);
                if (pageCount > 1) {
                    gtk_list_box_select_row(pageList, gtk_list_box_get_row_at_index(pageList, 0));
                    const auto centerPage = state.hitPage(gtk_widget_get_width(canvas) / 2.0,
                                                           gtk_widget_get_height(canvas) / 2.0);
                    require(state.page == 0 && centerPage && centerPage->page == 0 &&
                                    !adw_overlay_split_view_get_show_sidebar(splitView),
                            "Selecting a page did not scroll to it or dismiss the overlay");
                }
                require(state.ink.size() == 2, "Session ink did not survive sidebar navigation");
                if (nativeSession) {
                    const auto pageSize = state.pageSize(0);
                    std::optional<prototype::Point> blank;
                    for (double y = pageSize.height - 36; y > pageSize.height * 0.55 && !blank; y -= 24) {
                        for (double x = pageSize.width - 150; x > pageSize.width * 0.45; x -= 32) {
                            if (!textAt(0, {x, y})) {
                                blank = prototype::Point{x, y};
                                break;
                            }
                        }
                    }
                    require(blank.has_value(), "Text smoke could not find an unused spot on the page");
                    state.offsetY = gtk_widget_get_height(canvas) / 2.0 - state.pageTop(0) * state.zoom -
                                    blank->y * state.zoom;
                    update();
                    selectTool(Tool::Text);
                    require(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(textButton)) &&
                                    std::none_of(toolButtons.begin(), toolButtons.end(), [](GtkWidget* button) {
                                        return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
                                    }),
                            "Text tool did not exclusively show its selected state");
                    gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), 24.0);
                    const auto createAt = state.pageScreenPoint(0, blank->x, blank->y);
                    g_signal_emit_by_name(textClick, "released", 1, createAt.x, createAt.y);
                    require(textEditor->active(), "Text tool click did not open an inline editor");
                    auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
                    gtk_text_buffer_set_text(buffer, "GTK rich π", -1);
                    GtkTextIter selectionStart;
                    GtkTextIter selectionEnd;
                    gtk_text_buffer_get_bounds(buffer, &selectionStart, &selectionEnd);
                    gtk_text_buffer_select_range(buffer, &selectionStart, &selectionEnd);
                    gboolean handled = false;
                    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_b, 0u, GDK_CONTROL_MASK, &handled);
                    require(handled, "Ctrl+B did not format selected text in the inline editor");
                    const GdkRGBA textColor{208.0f / 255, 32.0f / 255, 16.0f / 255, 1};
                    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &textColor);
                    require(std::abs(state.textColor[0] - 208.0 / 255) < 1e-7 &&
                                    std::abs(state.textColor[1] - 32.0 / 255) < 1e-7 &&
                                    std::abs(state.textColor[2] - 16.0 / 255) < 1e-7,
                            "The Text toolbar color control did not update the active text color");
                    updateTextEditorPosition();
                    update();
                } else {
                    require(!gtk_widget_get_sensitive(textButton),
                            "Text tool should be unavailable without a document worker");
                    finishSmoke = true;
                }
                break;
            }
            case 9: {
                require(textEditor->active(), "Inline text editor closed before its render capture");
                capture("text-editor");
                auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
                const auto newTextIndex = state.texts.size();
                finishTextEditing();
                require(state.texts.size() == newTextIndex + 1 && state.texts.back().text == "GTK rich π" &&
                                state.texts.back().fontSize == 24.0 && !state.texts.back().styleRuns.empty() &&
                                state.texts.back().styleRuns[0].color == 0xffd02010U &&
                                state.selectedTexts == std::vector<size_t>{newTextIndex} && state.canUndo(),
                        "Inline text creation did not preserve its formatting or select the new object for transforms");
                state.undo();
                require(state.texts.size() == newTextIndex, "Undo did not remove a newly created text item");
                state.redo();
                require(state.texts.size() == newTextIndex + 1 &&
                                state.selectedTexts == std::vector<size_t>{newTextIndex},
                        "Redo did not restore a newly created text item and its selection");

                activateSelection(Tool::SmartSelect);
                auto created = state.texts.back();
                const auto editAt = state.pageScreenPoint(created.page, created.x + created.layoutWidth / 2,
                                                          created.y + created.layoutHeight / 2);
                g_signal_emit_by_name(textClick, "released", 1, editAt.x, editAt.y);
                require(textEditor->active(), "Smart Select click did not edit native text");
                buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
                GtkTextIter textEnd;
                gtk_text_buffer_get_end_iter(buffer, &textEnd);
                gtk_text_buffer_insert(buffer, &textEnd, "!", 1);
                finishTextEditing();
                require(state.texts.back().text == "GTK rich π!" && !state.texts.back().styleRuns.empty() &&
                                state.selectedTexts == std::vector<size_t>{newTextIndex},
                        "Editing existing rich text lost content or its style runs");

                selectTool(Tool::Text);
                created = state.texts.back();
                const auto moveStart = state.pageScreenPoint(created.page, created.x + created.layoutWidth / 2,
                                                             created.y + created.layoutHeight / 2);
                g_signal_emit_by_name(textClick, "pressed", 1, moveStart.x, moveStart.y);
                g_signal_emit_by_name(mouse, "drag-begin", moveStart.x, moveStart.y);
                require(state.movingSelection() && state.selectedTexts == std::vector<size_t>{newTextIndex},
                        "Dragging existing text with the Text tool did not start its transform");
                g_signal_emit_by_name(mouse, "drag-update", 24.0 * state.zoom, 16.0 * state.zoom);
                g_signal_emit_by_name(mouse, "drag-end", 24.0 * state.zoom, 16.0 * state.zoom);
                g_signal_emit_by_name(textClick, "released", 1, moveStart.x, moveStart.y);
                require(!textEditor->active() && std::abs(state.texts.back().x - created.x - 24) < 1e-6 &&
                                std::abs(state.texts.back().y - created.y - 16) < 1e-6 &&
                                state.selectedTexts == std::vector<size_t>{newTextIndex},
                        "Text-tool drag did not move the object or incorrectly opened the inline editor");

                const auto beforeResize = state.texts.back();
                const auto resizeHandle = state.pageScreenPoint(beforeResize.page, state.selectionHandles()[4].x,
                                                                state.selectionHandles()[4].y);
                g_signal_emit_by_name(textClick, "pressed", 1, resizeHandle.x, resizeHandle.y);
                g_signal_emit_by_name(mouse, "drag-begin", resizeHandle.x, resizeHandle.y);
                require(state.movingSelection(), "A selected text corner did not start resizing in the Text tool");
                g_signal_emit_by_name(mouse, "drag-update", 20.0 * state.zoom, 20.0 * state.zoom);
                g_signal_emit_by_name(mouse, "drag-end", 20.0 * state.zoom, 20.0 * state.zoom);
                g_signal_emit_by_name(textClick, "released", 1, resizeHandle.x, resizeHandle.y);
                require(std::abs(state.texts.back().xx - beforeResize.xx) > 1e-5 &&
                                state.selectedTexts == std::vector<size_t>{newTextIndex},
                        "Text-tool corner drag did not resize text while retaining its selection");
                state.undo();
                require(std::abs(state.texts.back().xx - beforeResize.xx) < 1e-8 &&
                                state.selectedTexts == std::vector<size_t>{newTextIndex},
                        "Undo did not restore the text resize and its selection");
                update();
                break;
            }
            case 10:
                capture("text-edited");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                finishSmoke = true;
                break;
        }
        if (finishSmoke) {
            const auto p0 = state.pageScreenPoint(0, 80, 80);
            begin(Device::Mouse, p0.x, p0.y);
            gtk_widget_set_visible(window, false);
            require(!state.current && !state.pinching && !input.active() && !input.spaceHeld && state.ink.size() == 3,
                    "Unmap dropped delivered ink or left temporary input active");
            std::cout << "PASS: GTK4 Cairo canvas, real PDF, colors, widths, highlight, undo/redo, eraser, "
                         "sidebar navigation/overlay, stylus/backlog/Space adapters, cancel, pinch, narrow "
                         "viewport, geometric drawing, shape recognition, stabilization, rich text, 2x raster cache"
                      << "\nframes=" << frames << " rasterizations=" << rasterizations
                      << " device_scale=" << gtk_widget_get_scale_factor(canvas)
                      << " viewport_width=" << gtk_widget_get_width(canvas)
                      << " cache_bytes=" << smokeCacheBytes << '\n';
            if (options.worker.empty()) {
                g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
                return G_SOURCE_REMOVE;
            }
            documentSmokeStep = 0;
            gtk_widget_set_halign(contentView, GTK_ALIGN_FILL);
            gtk_widget_set_size_request(contentView, -1, -1);
            gtk_window_present(GTK_WINDOW(window));
            fitted = false;
            update();
        }
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    static GtkWidget* responseButton(GtkWidget* widget, const char* label) {
        if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0) {
            return widget;
        }
        for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
            if (auto* found = responseButton(child, label)) {
                return found;
            }
        }
        return nullptr;
    }

    void answerDialog(const char* response) {
        require(messageDialog, "Expected a document dialog");
        auto* button = responseButton(GTK_WIDGET(messageDialog),
                                      adw_alert_dialog_get_response_label(messageDialog, response));
        require(button && gtk_widget_is_sensitive(button), "Dialog response is unavailable");
        g_signal_emit_by_name(button, "clicked");
    }

    bool cancelPrivateChooser() {
        auto* windows = gtk_window_get_toplevels();
        for (guint i = 0; i < g_list_model_get_n_items(windows); ++i) {
            auto* candidate = GTK_WINDOW(g_list_model_get_item(windows, i));
            const bool chooser = candidate != GTK_WINDOW(window) &&
                                 gtk_window_get_transient_for(candidate) == GTK_WINDOW(window) &&
                                 gtk_window_get_modal(candidate);
            if (chooser) {
                gtk_window_close(candidate);
            }
            g_object_unref(candidate);
            if (chooser) {
                return true;
            }
        }
        return false;
    }

    gboolean smokeDocuments() {
        const auto first = (std::filesystem::path(options.smokeDirectory) / "saved-copy.xopp").string();
        const auto second = (std::filesystem::path(options.smokeDirectory) / "saved-again.xopp").string();
        switch (documentSmokeStep++) {
            case 0:
                require(modified() && nativeSession, "Native document or dirty state missing");
                capture("document-dirty");
                saveCopyTo(first);
                require(busy && !gtk_widget_is_sensitive(canvas), "Saving did not block concurrent editing");
                gtk_window_close(GTK_WINDOW(window));
                require(!closed, "Window closed during an active save");
                break;
            case 1:
                require(!modified() && std::filesystem::is_regular_file(first), "Save did not commit clean state");
                require(state.ink.size() == 3 && state.canUndo(), "Saving discarded editable ink or undo history");
                capture("document-saved");
                state.undo();
                require(modified(), "Undo away from saved state must be dirty");
                protectDocument([this] { smokeContinued = true; });
                require(messageDialog, "Unsaved changes did not require a response");
                break;
            case 2:
                capture("unsaved");
                answerDialog("cancel");
                require(!smokeContinued && !messageDialog && state.ink.size() == 2,
                        "Cancel discarded changes or continued the action");
                openDocument(first + ".missing");
                break;
            case 3:
                require(ioErrors == 1 && messageDialog && state.ink.size() == 2 && modified(),
                        "Failed open lost the current document or changes");
                capture("open-error");
                answerDialog("close");
                saveCopyTo(first, [this] { smokeContinued = true; });
                break;
            case 4:
                require(ioErrors == 2 && messageDialog && modified() && !smokeContinued,
                        "Failed save cleared changes or ran its continuation");
                answerDialog("close");
                state.redo();
                require(!modified(), "Redo back to saved revision must be clean");
                openDocument(first);
                break;
            case 5: {
                require(nativeSession && sourcePath == first && state.ink.size() == 3 && !modified() &&
                                !state.canUndo() && !state.canRedo() && pageCount == 2,
                        "Reopen did not hydrate native strokes at clean revision 0");
                state.erase(state.offsetX + 80 * state.zoom, state.offsetY + 80 * state.zoom);
                require(modified() && state.canUndo() && state.ink.size() != 3,
                        "Erasing a preloaded native stroke did not mark the document dirty");
                state.undo();
                require(!modified() && !state.canUndo() && state.ink.size() == 3,
                        "Undoing native stroke erase did not restore clean revision 0");
                capture("reopened");
                const auto x = state.offsetX + 60 * state.zoom;
                const auto y = state.offsetY + 60 * state.zoom;
                input.begin(Device::Stylus, {x, y, 0.25});
                input.motion(Device::Stylus, {x + 40 * state.zoom, y + 20 * state.zoom, 0.75});
                input.finish(Device::Stylus);
                protectDocument([this] { smokeContinued = true; });
                require(messageDialog && state.ink.size() == 4 && state.ink.back().pressureSensitive,
                        "Pressure ink not retained for saving");
                break;
            }
            case 6:
                capture("save-prompt");
                answerDialog("save");
                require(busy && !smokeContinued, "Save continuation ran before completion");
                break;
            case 7:
                require(smokeContinued && !modified() && saveTarget == first && std::filesystem::is_regular_file(first),
                        "Normal Save from the prompt failed before continuing");
                require(!std::filesystem::exists(first + "~"), "Successful Save retained its temporary backup");
                smokeSaveChoice = second;
                chooseSave();
                require(busy, "Save As did not start");
                break;
            case 8:
                require(!modified() && saveTarget == second && sourcePath == second &&
                                std::filesystem::is_regular_file(second),
                        "Save As did not associate the new filename");
                capture("saved-again");
                state.undo();
                smokeContinued = false;
                protectDocument([this] { smokeContinued = true; });
                break;
            case 9:
                capture("discard-prompt");
                answerDialog("discard");
                require(smokeContinued && !messageDialog, "Discard did not continue the requested action");
                gtk_window_close(GTK_WINDOW(window));
                require(messageDialog && !closed, "Close did not protect unsaved changes");
                answerDialog("cancel");
                require(!closed && gtk_widget_get_visible(window), "Cancel did not keep the window open");
                break;
            case 10:
                chooseOpen();
                require(choosingFile, "Open action did not request a native file chooser");
                break;
            case 11:
                if (!cancelPrivateChooser()) {
                    require(++chooserWaits < 60, "Native Open chooser did not appear");
                    --documentSmokeStep;
                }
                break;
            case 12:
                require(!choosingFile && !messageDialog && sourcePath == second && modified(),
                        "Cancelling Open changed the current document");
                chooseSave();
                require(choosingFile, "Save action did not request a native file chooser");
                break;
            case 13:
                if (!cancelPrivateChooser()) {
                    require(++chooserWaits < 60, "Native Save chooser did not appear");
                    --documentSmokeStep;
                }
                break;
            case 14:
                require(!choosingFile && !messageDialog && modified() && !closed,
                        "Cancelling Save discarded changes or closed the document");
                break;
            case 15:
                smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "normal-save.xopp").string();
                chooseSave();
                break;
            case 16: {
                require(!modified() && saveTarget.ends_with("normal-save.xopp"), "New Save As failed");
                const auto x = state.offsetX + 140 * state.zoom;
                const auto y = state.offsetY + 160 * state.zoom;
                input.begin(Device::Mouse, {x, y, std::nullopt});
                input.finish(Device::Mouse);
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_s, 0, GDK_CONTROL_MASK, &handled);
                require(handled && busy, "Ctrl+S did not start normal Save");
                break;
            }
            case 17: {
                require(!modified() && !std::filesystem::exists(saveTarget + "~"),
                        "Normal Save did not complete or remove its temporary backup");
                capture("normal-saved");
                auto packets = prototype::bridge::transferInk(state.ink);
                auto texts = state.texts;
                auto images = prototype::bridge::transferImages(state.images);
                auto layers = state.pageLayers;
                auto pageSizes = currentPageSizes(state);
                packets.push_back({0, false, false, 2, {0, 1, 0}, {{200, 220, 1}, {230, 220, 1}}});
                const auto session = nativeSession;
                const auto worker = options.worker;
                const auto target = saveTarget;
                const auto before = saveVersion;
                runDocumentJob("Testing external edit…",
                               [session, worker, target, before, packets, texts, images, layers, pageSizes] {
                    session->save(worker, target, packets, texts, images, layers, pageSizes, before);
                }, [] {});
                break;
            }
            case 18: {
                smokeExternalVersion = prototype::bridge::documentVersion(saveTarget).token;
                require(smokeExternalVersion != saveVersion, "External edit simulation did not change the native file");
                const auto x = state.offsetX + 170 * state.zoom;
                const auto y = state.offsetY + 190 * state.zoom;
                input.begin(Device::Mouse, {x, y, std::nullopt});
                input.finish(Device::Mouse);
                save();
                break;
            }
            case 19:
                require(ioErrors == 3 && messageDialog && modified() && state.ink.size() == 5 &&
                                prototype::bridge::documentVersion(saveTarget).token == smokeExternalVersion,
                        "A stale Save overwrote the external edit or lost pending session ink");
                capture("save-conflict");
                answerDialog("close");
                break;
            case 20:
                // Explicit Save As may replace a chosen current filename after
                // the native chooser's overwrite confirmation. Ctrl+S may not.
                smokeSaveChoice = saveTarget;
                chooseSave();
                break;
            case 21: {
                require(!modified() && prototype::bridge::documentVersion(saveTarget).token == saveVersion &&
                                saveVersion != smokeExternalVersion && !std::filesystem::exists(saveTarget + "~"),
                        "Explicit Save As did not resolve the selected filename conflict");
                capture("conflict-resolved");
                const auto x = state.offsetX + 210 * state.zoom;
                const auto y = state.offsetY + 230 * state.zoom;
                input.begin(Device::Mouse, {x, y, std::nullopt});
                input.finish(Device::Mouse);
                break;
            }
            case 22: {
                state.undo();
                require(!modified(), "Undo before autosave test must restore clean state");
                smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "autosave-note.xopp").string();
                chooseSave();
                break;
            }
            case 23: {
                require(!modified() && saveTarget.ends_with("autosave-note.xopp") && activeAutosavePath.empty(),
                        "Baseline autosave document was not saved cleanly");
                smokeAutosaveBaseVersion = saveVersion;
                const auto x = state.offsetX + 90 * state.zoom;
                const auto y = state.offsetY + 250 * state.zoom;
                begin(Device::Stylus, x, y, 0.3);
                motion(Device::Stylus, x + 55 * state.zoom, y + 18 * state.zoom, 0.8);
                finish(Device::Stylus, 0.8);
                // Keep a second stylus stroke actively in progress while background autosave runs.
                begin(Device::Stylus, x, y + 36 * state.zoom, 0.4);
                motion(Device::Stylus, x + 45 * state.zoom, y + 48 * state.zoom, 0.7);
                require(modified() && input.active() && state.current.has_value(),
                        "Active stylus stroke was not started before autosave");
                scheduleAutosave(1);
                autosaveWaits = 0;
                break;
            }
            case 24: {
                const auto autosaveSidecar =
                        prototype::bridge::autosavePathForTarget(saveTarget, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                require(input.active() && state.current.has_value() && gtk_widget_is_sensitive(canvas),
                        "Background autosave interrupted an active stroke or disabled the canvas");
                if (autosaving || activeAutosavePath != autosaveSidecar || autosavedRevision != state.revision()) {
                    require(++autosaveWaits < 60, "Periodic background autosave did not complete");
                    --documentSmokeStep;
                    break;
                }
                require(std::filesystem::is_regular_file(autosaveSidecar) &&
                                prototype::bridge::documentVersion(saveTarget).token == smokeAutosaveBaseVersion &&
                                !std::filesystem::exists(saveTarget + "~") &&
                                !std::filesystem::exists(autosaveSidecar + ".swap"),
                        "Autosave did not write sidecar cleanly or touched the primary saved document");
                finish(Device::Stylus, 0.7);
                require(!input.active() && state.ink.size() == 7, "Active stroke did not commit after autosave");
                autosaveWaits = 0;
                break;
            }
            case 25:
                if (autosaving || autosavedRevision != state.revision()) {
                    require(++autosaveWaits < 60, "Updated periodic autosave did not complete");
                    --documentSmokeStep;
                    break;
                }
                break;  // Paint the updated "Unsaved changes · Autosaved" header before capturing.
            case 26: {
                const auto autosaveSidecar = activeAutosavePath;
                capture("autosaved");
                state.undo();
                state.undo();
                require(!modified() && std::filesystem::is_regular_file(autosaveSidecar),
                        "Expected clean state prior to autosave cleanup");
                startAutosave();
                require(activeAutosavePath.empty() && !std::filesystem::exists(autosaveSidecar),
                        "Autosave check did not remove redundant sidecar after undoing to clean state");
                state.redo();
                state.redo();
                autosaveWaits = 0;
                break;
            }
            case 27: {
                const auto autosaveDoc = saveTarget;
                const auto autosaveSidecar =
                        prototype::bridge::autosavePathForTarget(autosaveDoc, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                if (autosaving || activeAutosavePath != autosaveSidecar || autosavedRevision != state.revision()) {
                    require(++autosaveWaits < 60, "Autosave did not recreate sidecar after redo");
                    --documentSmokeStep;
                    break;
                }
                const auto discardDoc =
                        (std::filesystem::path(options.smokeDirectory) / "discard-autosave.xopp").string();
                const auto discardSidecar =
                        prototype::bridge::autosavePathForTarget(discardDoc, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                const auto recoverDoc =
                        (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
                const auto recoverSidecar =
                        prototype::bridge::autosavePathForTarget(recoverDoc, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                std::filesystem::copy_file(autosaveDoc, discardDoc,
                                           std::filesystem::copy_options::overwrite_existing);
                std::filesystem::copy_file(autosaveSidecar, discardSidecar,
                                           std::filesystem::copy_options::overwrite_existing);
                std::filesystem::copy_file(autosaveDoc, recoverDoc,
                                           std::filesystem::copy_options::overwrite_existing);
                std::filesystem::copy_file(autosaveSidecar, recoverSidecar,
                                           std::filesystem::copy_options::overwrite_existing);
                const auto cacheProtected =
                        prototype::bridge::autosavePathForTarget({}, cacheRoot(), 999999).string();
                std::filesystem::create_directories(std::filesystem::path(cacheProtected).parent_path());
                std::filesystem::copy_file(autosaveSidecar, cacheProtected,
                                           std::filesystem::copy_options::overwrite_existing);
                requestOpenDocument(cacheProtected);
                require(ioErrors == 4 && messageDialog && modified() && saveTarget == autosaveDoc,
                        "Direct open inside cache directory was not refused safely");
                std::filesystem::remove(cacheProtected);
                answerDialog("close");
                scheduleAutosave(static_cast<guint>(options.autosaveSeconds));
                save();
                break;
            }
            case 28: {
                const auto previousSidecar =
                        prototype::bridge::autosavePathForTarget(saveTarget, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                require(!modified() && activeAutosavePath.empty() && !std::filesystem::exists(previousSidecar),
                        "Normal Save did not remove active autosave sidecar");
                const auto discardDoc =
                        (std::filesystem::path(options.smokeDirectory) / "discard-autosave.xopp").string();
                requestOpenDocument(discardDoc);
                require(messageDialog, "Opening a document with a sidecar autosave did not prompt");
                answerDialog("discard");
                break;
            }
            case 29: {
                const auto discardDoc =
                        (std::filesystem::path(options.smokeDirectory) / "discard-autosave.xopp").string();
                const auto discardSidecar =
                        prototype::bridge::autosavePathForTarget(discardDoc, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                require(!modified() && sourcePath == discardDoc && state.ink.size() == 5 &&
                                !std::filesystem::exists(discardSidecar),
                        "Discard Autosave did not remove the sidecar or opened dirty state");
                const auto recoverDoc =
                        (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
                smokeAutosaveBaseVersion = prototype::bridge::documentVersion(recoverDoc).token;
                requestOpenDocument(recoverDoc);
                require(messageDialog, "Sidecar recovery prompt did not appear");
                break;
            }
            case 30:
                capture("autosave-prompt");
                answerDialog("recover");
                break;
            case 31: {
                const auto recoverDoc =
                        (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
                const auto recoverSidecar =
                        prototype::bridge::autosavePathForTarget(recoverDoc, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                require(modified() && recoveredChanges && saveTarget == recoverDoc &&
                                activeAutosavePath == recoverSidecar &&
                                std::filesystem::is_regular_file(recoverSidecar) &&
                                prototype::bridge::documentVersion(recoverDoc).token == smokeAutosaveBaseVersion,
                        "Recovered document did not stay dirty, keep its sidecar, or leave saved file untouched");
                const auto x = state.offsetX + 120 * state.zoom;
                const auto y = state.offsetY + 260 * state.zoom;
                input.begin(Device::Mouse, {x, y, std::nullopt});
                input.finish(Device::Mouse);
                state.undo();
                require(modified(), "Undoing back to revision 0 after recovery falsely marked document clean");
                capture("autosave-recovered");
                save();
                break;
            }
            case 32: {
                const auto recoverDoc =
                        (std::filesystem::path(options.smokeDirectory) / "recover-autosave.xopp").string();
                const auto recoverSidecar =
                        prototype::bridge::autosavePathForTarget(recoverDoc, cacheRoot(),
                                                                 static_cast<long long>(::getpid()))
                                .string();
                require(!modified() && !recoveredChanges && activeAutosavePath.empty() &&
                                !std::filesystem::exists(recoverSidecar) &&
                                prototype::bridge::documentVersion(recoverDoc).token != smokeAutosaveBaseVersion,
                        "Saving a recovered document did not update target or clean up its sidecar autosave");
                capture("autosave-saved");
                const auto x = state.offsetX + 80 * state.zoom;
                const auto y = state.offsetY + 360 * state.zoom;
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
                begin(Device::Stylus, x, y, 0.3);
                motion(Device::Stylus, x + 160 * state.zoom, y, 0.9);
                finish(Device::Stylus, 0.9);
                // Split the stylus stroke with Standard eraser and add a Whiteout stroke, then save natively.
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 2u);
                begin(Device::Mouse, x + 80 * state.zoom, y);
                finish(Device::Mouse);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 1u);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
                begin(Device::Mouse, x + 30 * state.zoom, y - 25 * state.zoom);
                motion(Device::Mouse, x + 30 * state.zoom, y + 25 * state.zoom);
                finish(Device::Mouse);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                require(state.ink.size() == 10 && state.ink.back().brush.tool == Tool::Eraser,
                        "Expected two split pen segments and one whiteout stroke before native save");
                smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "eraser-modes.xopp").string();
                chooseSave();
                break;
            }
            case 33: {
                require(!modified() && saveTarget.ends_with("eraser-modes.xopp") &&
                                std::filesystem::is_regular_file(saveTarget),
                        "Saving split ink and whiteout stroke failed");
                const auto nativeEdited =
                        (std::filesystem::path(options.smokeDirectory) / "native-edited.xopp").string();
                std::filesystem::copy_file(saveTarget, nativeEdited,
                                           std::filesystem::copy_options::overwrite_existing);
                openDocument(nativeEdited);
                break;
            }
            case 34: {
                const auto nativeEdited =
                        (std::filesystem::path(options.smokeDirectory) / "native-edited.xopp").string();
                require(!modified() && sourcePath == nativeEdited && saveTarget == nativeEdited &&
                                state.ink.size() == 10 && state.ink.back().brush.tool == Tool::Eraser &&
                                !state.canUndo() && !state.canRedo(),
                        "Reopening native document did not load all 10 editable top-layer strokes");
                capture("native-loaded");
                const auto y = state.offsetY + 360 * state.zoom;
                // 1. Partially split a pre-existing native pressure-sensitive pen stroke.
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 2u);
                begin(Device::Mouse, state.offsetX + 205 * state.zoom, y);
                finish(Device::Mouse);
                require(state.ink.size() == 11 && modified(),
                        "Standard eraser did not split preloaded native pen stroke");
                // 2. Delete the pre-existing native Whiteout stroke and the left native pen segment.
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 2u);
                begin(Device::Mouse, state.offsetX + 110 * state.zoom, state.offsetY + 338 * state.zoom);
                finish(Device::Mouse);
                require(state.ink.size() == 10, "Delete Stroke did not remove preloaded native whiteout stroke");
                begin(Device::Mouse, state.offsetX + 135 * state.zoom, y);
                finish(Device::Mouse);
                require(state.ink.size() == 9, "Delete Stroke did not remove preloaded native pen segment");
                // 3. Verify full Undo back to clean revision 0 and Redo back to revision 3.
                state.undo();
                state.undo();
                state.undo();
                require(!modified() && state.ink.size() == 10 && !state.canUndo(),
                        "Undoing native stroke edits did not restore clean revision 0");
                state.redo();
                state.redo();
                state.redo();
                require(modified() && state.ink.size() == 9 && !state.canRedo(),
                        "Redoing native stroke edits did not restore edited state");
                break;
            }
            case 35: {
                capture("native-erased");
                gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_s, 0, GDK_CONTROL_MASK, &handled);
                require(handled && busy, "Ctrl+S did not start Save for edited native strokes");
                break;
            }
            case 36: {
                require(!modified() && saveTarget.ends_with("native-edited.xopp") &&
                                !std::filesystem::exists(saveTarget + "~") && pageCount == 2,
                        "Ctrl+S did not persist edited native strokes cleanly");
                capture("native-saved");
                // Position the continuous viewport across the Page 1 / Page 2 boundary so Page 2 holds >50% area.
                const double viewW = gtk_widget_get_width(canvas);
                const double viewH = gtk_widget_get_height(canvas);
                fitMode = false;
                fitted = true;
                state.offsetY = viewH * 0.42 - state.pageTop(1) * state.zoom;
                state.syncActivePage(viewW, viewH);
                require(state.page == 1, "Continuous scroll did not promote Page 2 as active page");
                // Dragging inside the 15pt vertical inter-page gap must temporarily pan without drawing ink.
                const auto gapPt = state.pageScreenPoint(0, 120, state.pageSize(0).height + 7.5);
                const double oldOffsetY = state.offsetY;
                g_signal_emit_by_name(mouse, "drag-begin", gapPt.x, gapPt.y);
                require(input.panning(), "Dragging in the 15pt inter-page gap must activate the hand tool");
                g_signal_emit_by_name(mouse, "drag-update", 0.0, -12.0);
                g_signal_emit_by_name(mouse, "drag-end", 0.0, -12.0);
                require(state.ink.size() == 9 && std::abs(state.offsetY - (oldOffsetY - 12.0)) < 0.001,
                        "Inter-page gap pan unexpectedly drew ink or miscalculated offset");
                // Draw highlighter + pressure pen ink directly onto Page 2 without clicking the sidebar.
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[1]), true);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
                const auto hStart = state.pageScreenPoint(1, 70, 95);
                const auto hEnd = state.pageScreenPoint(1, 310, 95);
                begin(Device::Mouse, hStart.x, hStart.y);
                motion(Device::Mouse, hEnd.x, hEnd.y);
                finish(Device::Mouse);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
                const auto pStart = state.pageScreenPoint(1, 70, 135);
                const auto pEnd = state.pageScreenPoint(1, 290, 135);
                begin(Device::Stylus, pStart.x, pStart.y, 0.35);
                motion(Device::Stylus, pEnd.x, pEnd.y, 0.90);
                finish(Device::Stylus, 0.90);
                require(modified() && state.ink.size() == 11 && state.ink[9].page == 1 && state.ink[10].page == 1,
                        "Direct drawing on Page 2 in continuous view did not target Page 2");
                break;
            }
            case 37: {
                capture("continuous-pages");
                // Cross-page scrolling must reuse the bounded 4-page LRU raster cache without rerasterizing Poppler.
                const int rastersBeforeScroll = rasterizations;
                const double zoomBeforeShortcuts = state.zoom;
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Home, 0u, GDK_CONTROL_MASK, &handled);
                require(handled && state.page == 0 && std::abs(state.zoom - zoomBeforeShortcuts) < 1e-9,
                        "Ctrl+Home failed or reset zoom");
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_J, 0u, GDK_SHIFT_MASK, &handled);
                require(handled && state.page == 1 && std::abs(state.zoom - zoomBeforeShortcuts) < 1e-9,
                        "Shift+J next-page shortcut failed or reset zoom");
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_K, 0u, GDK_SHIFT_MASK, &handled);
                require(handled && state.page == 0, "Shift+K previous-page shortcut failed");
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_End, 0u, GDK_CONTROL_MASK, &handled);
                require(handled && state.page == 1, "Ctrl+End last-page shortcut failed");
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_K, 0u, GDK_SHIFT_MASK, &handled);
                require(handled && state.page == 0 && rasterizations == rastersBeforeScroll,
                        "Page navigation across cached pages unnecessarily rerasterized PDF");
                // Exercise ':' and Ctrl+G Go-to-Page dialogs.
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_colon, 0u, GdkModifierType(0), &handled);
                require(handled && messageDialog && gotoPageSpin, "Colon shortcut did not open Go-to-Page dialog");
                answerDialog("cancel");
                require(!messageDialog && state.page == 0, "Cancelling Go-to-Page dialog changed active page");
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_g, 0u, GDK_CONTROL_MASK, &handled);
                require(handled && messageDialog && gotoPageSpin, "Ctrl+G did not open Go-to-Page dialog");
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(gotoPageSpin), 2);
                break;
            }
            case 38:
                break;  // Let AdwAlertDialog open animation finish before capturing goto-page-dialog.
            case 39: {
                capture("goto-page-dialog");
                answerDialog("go");
                require(!messageDialog && state.page == 1, "Go-to-Page dialog did not jump to Page 2");
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_d, 0u, GDK_CONTROL_MASK, &handled);
                require(handled && busy, "Ctrl+D did not start Insert Page After");
                break;
            }
            case 40: {
                require(pageCount == 3 && state.page == 2 && modified() && gtk_widget_get_sensitive(deletePageButton),
                        "Insert Page After did not create Page 3 or mark document modified");
                const auto p3Start = state.pageScreenPoint(2, 80, 90);
                const auto p3End = state.pageScreenPoint(2, 260, 90);
                begin(Device::Mouse, p3Start.x, p3Start.y);
                motion(Device::Mouse, p3End.x, p3End.y);
                finish(Device::Mouse);
                require(state.ink.size() == 12 && state.ink.back().page == 2,
                        "Drawing on newly inserted Page 3 did not record page index 2");
                // Frame the boundary between Page 2 and newly inserted lined Page 3.
                const double viewW = gtk_widget_get_width(canvas);
                const double viewH = gtk_widget_get_height(canvas);
                fitMode = false;
                fitted = true;
                state.offsetY = viewH * 0.36 - state.pageTop(2) * state.zoom;
                state.syncActivePage(viewW, viewH);
                break;
            }
            case 41: {
                capture("page-inserted");
                // Export as PDF (Ctrl+E) must include unsaved strokes and newly inserted Page 3 without clearing dirty state.
                smokeExportChoice = (std::filesystem::path(options.smokeDirectory) / "exported-notes.pdf").string();
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_e, 0u, GDK_CONTROL_MASK, &handled);
                require(handled && busy, "Ctrl+E did not start PDF export");
                break;
            }
            case 42: {
                const auto exportedPdf =
                        (std::filesystem::path(options.smokeDirectory) / "exported-notes.pdf").string();
                require(std::filesystem::is_regular_file(exportedPdf) && modified(),
                        "PDF export failed or falsely cleared .xopp unsaved changes");
                smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "multipage-edited.xopp").string();
                chooseSave();
                break;
            }
            case 43: {
                require(!modified() && saveTarget.ends_with("multipage-edited.xopp") && pageCount == 3,
                        "Saving 3-page document failed");
                navigateToPage(1);
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Delete, 0u,
                                      GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
                require(handled && busy, "Ctrl+Shift+Delete did not start Delete Current Page");
                break;
            }
            case 44: {
                // Deleting old Page 2 (index 1, 2 strokes) shifts old Page 3 (1 stroke) down to index 1 -> 10 strokes total.
                require(pageCount == 2 && modified() && state.ink.size() == 10 && state.ink.back().page == 1,
                        "Deleting Page 2 did not remove its strokes or shift Page 3 strokes to index 1");
                smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "after-delete-page.xopp").string();
                chooseSave();
                break;
            }
            case 45: {
                require(!modified() && saveTarget.ends_with("after-delete-page.xopp") && pageCount == 2,
                        "Saving document after deleting Page 2 failed");
                const auto p0 = state.pageScreenPoint(state.page, 210, 230);
                input.begin(Device::Mouse, {p0.x, p0.y, std::nullopt});
                input.finish(Device::Mouse);
                break;
            }
            case 46: {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                navigateToPage(0);
                const auto a = state.pageScreenPoint(0, 300, 400);
                const auto b = state.pageScreenPoint(0, 350, 420);
                input.begin(Device::Stylus, {a.x, a.y, .25});
                input.motion(Device::Stylus, {b.x, b.y, .75});
                input.finish(Device::Stylus);
                smokeSelectionIndex = state.ink.size() - 1;
                const auto originalRevision = state.revision();
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_R, 0u,
                                      GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
                require(handled && state.brush.tool == Tool::SelectRectangle, "Rectangle selection shortcut failed");
                g_signal_emit_by_name(keys,"key-released",GDK_KEY_Shift_L,0u,GdkModifierType(0));
                const auto from = state.pageScreenPoint(0, 295, 395);
                const auto to = state.pageScreenPoint(0, 355, 425);
                g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
                g_signal_emit_by_name(mouse, "drag-update", to.x - from.x, to.y - from.y);
                g_signal_emit_by_name(mouse, "drag-end", to.x - from.x, to.y - from.y);
                require(state.selected == std::vector<size_t>{smokeSelectionIndex} &&
                                state.revision() == originalRevision, "Rectangle selection changed ink or saved status");
                break;
            }
            case 47: {
                capture("selection");
                const auto from = state.pageScreenPoint(0, 320, 410);
                const auto to = state.pageScreenPoint(0, 340, 450);
                g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
                g_signal_emit_by_name(mouse, "drag-update", to.x - from.x, to.y - from.y);
                require(std::abs(state.selectionBox()->x0 - 320) < 1e-6 &&
                                std::abs(state.committedInk()[smokeSelectionIndex].points.front().x - 300) < 1e-6,
                        "Selection preview leaked into committed autosave state");
                g_signal_emit_by_name(mouse, "drag-end", to.x - from.x, to.y - from.y);
                require(std::abs(state.ink[smokeSelectionIndex].points.front().y - 440) < 1e-6,
                        "Selection drag did not apply page-coordinate translation");
                break;
            }
            case 48:
                capture("selection-moved");
                smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "selection-moved.xopp").string();
                chooseSave();
                break;
            case 49: {
                require(!modified() && !state.selected.empty(), "Saving discarded selection or failed");
                g_signal_emit_by_name(undoButton, "clicked");
                require(modified() && std::abs(state.ink[smokeSelectionIndex].points.front().x - 300) < 1e-6 &&
                                state.selected == std::vector<size_t>{smokeSelectionIndex},
                        "Move undo did not retain selection and restore coordinates");
                g_signal_emit_by_name(redoButton, "clicked");
                require(!modified() && std::abs(state.ink[smokeSelectionIndex].points.front().x - 320) < 1e-6,
                        "Move redo did not return to saved state");
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Delete, 0u, GdkModifierType(0), &handled);
                require(handled && state.ink.size() == smokeSelectionIndex && state.selected.empty(),
                        "Delete did not remove the selected stroke");
                break;
            }
            case 50:
                capture("selection-deleted");
                smokeSaveChoice = (std::filesystem::path(options.smokeDirectory) / "selection-deleted.xopp").string();
                chooseSave();
                break;
            case 51: {
                require(!modified(), "Saving deleted native ink failed");
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.ink.size() == smokeSelectionIndex + 1 && state.selected.size() == 1 && modified(),
                        "Delete undo did not restore the selected native stroke");
                const auto from = state.pageScreenPoint(0, 340, 450);
                g_signal_emit_by_name(mouse, "drag-begin", from.x, from.y);
                g_signal_emit_by_name(mouse, "drag-update", 30.0, 20.0);
                g_signal_emit_by_name(mouse, "cancel", nullptr);
                require(std::abs(state.ink[smokeSelectionIndex].points.front().x - 320) < 1e-6 && !state.selecting(),
                        "Selection cancellation retained transient coordinates");
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_a, 0u, GDK_CONTROL_MASK, &handled);
                require(handled && !state.selected.empty(), "Select All failed");
                for (auto index: state.selected) {
                    require(state.ink[index].page == state.page, "Select All crossed page boundaries");
                }
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Escape, 0u, GdkModifierType(0), &handled);
                require(state.selected.empty(), "Escape did not clear selection");
                std::cout << "PASS: rectangle selection, native stroke move/delete, saved-state undo/redo, "
                             "selection cancellation, committed autosave snapshots, Select All and Escape\n";
                break;
            }
            case 52: {
                activateSelection(Tool::SmartSelect);
                state.clearSelection();
                navigateToPage(0);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                const auto a = state.pageScreenPoint(0, 200, 450);
                const auto b = state.pageScreenPoint(0, 300, 470);
                const auto c = state.pageScreenPoint(0, 240, 530);
                input.begin(Device::Stylus, {a.x,a.y,.25});
                input.motion(Device::Stylus, {b.x,b.y,.5});
                input.motion(Device::Stylus, {c.x,c.y,.75});
                input.finish(Device::Stylus);
                smokeSelectionIndex = state.ink.size()-1;
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_G, 0u,
                                      GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
                require(handled && state.brush.tool == Tool::SelectLasso, "Lasso shortcut failed");
                g_signal_emit_by_name(keys,"key-released",GDK_KEY_Shift_L,0u,GdkModifierType(0));
                const auto from = state.pageScreenPoint(0,190,440);
                g_signal_emit_by_name(mouse,"drag-begin",from.x,from.y);
                for (const auto p: {prototype::Point{310,440},prototype::Point{310,540},
                                    prototype::Point{190,540},prototype::Point{190,440}}) {
                    const auto to = state.pageScreenPoint(0,p.x,p.y);
                    g_signal_emit_by_name(mouse,"drag-update",to.x-from.x,to.y-from.y);
                }
                g_signal_emit_by_name(mouse,"drag-end",0.0,0.0);
                require(state.ink[smokeSelectionIndex].page == 0 &&
                                state.selected == std::vector<size_t>{smokeSelectionIndex} && state.selectedTexts.empty(),
                        "Lasso did not isolate the native stroke for the stroke clipboard check");
                break;
            }
            case 53: {
                capture("lasso-selection");
                const auto handles = state.selectionHandles();
                const auto a = state.pageScreenPoint(0,handles[4].x,handles[4].y);
                const auto b = state.pageScreenPoint(0,handles[0].x+1.5*(handles[4].x-handles[0].x),
                                                         handles[0].y+1.5*(handles[4].y-handles[0].y));
                smokeTransformWidth = state.ink.back().brush.width;
                const bool oldSnapGridEnabled = state.snapGridEnabled;
                state.snapGridEnabled = false;
                g_signal_emit_by_name(mouse,"drag-begin",a.x,a.y);
                require(state.selected == std::vector<size_t>{smokeSelectionIndex} && state.selectedTexts.empty(),
                        "Resize handle press discarded the selected stroke");
                g_signal_emit_by_name(mouse,"drag-update",b.x-a.x,b.y-a.y);
                require(state.committedInk().back().brush.width == smokeTransformWidth,"Resize preview leaked into autosave");
                g_signal_emit_by_name(mouse,"drag-end",b.x-a.x,b.y-a.y);
                state.snapGridEnabled = oldSnapGridEnabled;
                require(std::abs(state.ink.back().brush.width-1.5*smokeTransformWidth)<1e-6,
                        "Resize lost native width scaling");
                break;
            }
            case 54: {
                capture("selection-resized");
                const auto box = state.selectionBox();
                const auto h = state.selectionHandles()[8];
                const auto cx = (box->x0+box->x1)/2, cy = (box->y0+box->y1)/2;
                const auto a = state.pageScreenPoint(0,h.x,h.y);
                const auto b = state.pageScreenPoint(0,cx-(h.y-cy),cy+(h.x-cx));
                g_signal_emit_by_name(mouse,"drag-begin",a.x,a.y);
                g_signal_emit_by_name(mouse,"drag-update",b.x-a.x,b.y-a.y);
                g_signal_emit_by_name(mouse,"drag-end",b.x-a.x,b.y-a.y);
                require(std::abs(state.ink.back().brush.width-1.5*smokeTransformWidth)<1e-6,"Rotation changed stroke width");
                break;
            }
            case 55: {
                capture("selection-rotated");
                smokeClipboardCount = state.ink.size();
                gboolean handled = false;
                g_signal_emit_by_name(keys,"key-pressed",GDK_KEY_c,0u,GDK_CONTROL_MASK,&handled);
                require(handled,"Copy shortcut failed");
                g_signal_emit_by_name(keys,"key-pressed",GDK_KEY_v,0u,GDK_CONTROL_MASK,&handled);
                require(handled && busy,"Paste did not start async clipboard read");
                break;
            }
            case 56: {
                require(state.ink.size()==smokeClipboardCount+1 && state.selected.size()==1 &&
                                std::abs(state.ink.back().brush.width-1.5*smokeTransformWidth)<1e-6,
                        "Clipboard paste lost native stroke or width");
                capture("selection-pasted");
                duplicateSelection();
                require(state.ink.size()==smokeClipboardCount+2,"Duplicate selection failed");
                g_signal_emit_by_name(undoButton,"clicked");
                require(state.ink.size()==smokeClipboardCount+1,"Duplicate was not a single undo action");
                gboolean handled=false;
                g_signal_emit_by_name(keys,"key-pressed",GDK_KEY_x,0u,GDK_CONTROL_MASK,&handled);
                require(handled && state.ink.size()==smokeClipboardCount,"Cut did not remove clipboard selection");
                g_signal_emit_by_name(keys,"key-pressed",GDK_KEY_v,0u,GDK_CONTROL_MASK,&handled);
                break;
            }
            case 57: {
                require(state.ink.size()==smokeClipboardCount+1 && state.selected.size()==1,"Cut/paste failed");
                const auto box = state.selectionBox();
                const auto a = state.pageScreenPoint(0,(box->x0+box->x1)/2,(box->y0+box->y1)/2);
                const auto b = state.pageScreenPoint(1,180,220);
                g_signal_emit_by_name(mouse,"drag-begin",a.x,a.y);
                g_signal_emit_by_name(mouse,"drag-update",b.x-a.x,b.y-a.y);
                require(state.displayPage(state.selected.front())==1 && state.committedInk().back().page==0,
                        "Cross-page preview leaked into committed document");
                g_signal_emit_by_name(mouse,"drag-end",b.x-a.x,b.y-a.y);
                require(state.ink.back().page==1,"Cross-page transfer did not reassign page");
                g_signal_emit_by_name(undoButton,"clicked");
                require(state.ink.back().page==0,"Cross-page undo failed");
                g_signal_emit_by_name(redoButton,"clicked");
                navigateToPage(1);
                break;
            }
            case 58:
                smokeSaveChoice=(std::filesystem::path(options.smokeDirectory)/"selection-transforms.xopp").string();
                chooseSave();
                break;
            case 59: {
                require(!modified() && state.ink.back().page==1,"Saving selection transforms failed");
                capture("selection-transferred");
                gboolean handled=false;
                g_signal_emit_by_name(keys,"key-pressed",GDK_KEY_Escape,0u,GdkModifierType(0),&handled);
                g_signal_emit_by_name(keys,"key-pressed",GDK_KEY_s,0u,GdkModifierType(0),&handled);
                require(handled && state.brush.tool==Tool::SmartSelect,"Smart Select shortcut failed");
                const auto p=state.ink.back().points.front();
                const auto from=state.pageScreenPoint(1,p.x,p.y);
                g_signal_emit_by_name(mouse,"drag-begin",from.x,from.y);
                g_signal_emit_by_name(mouse,"drag-update",15.0,15.0);
                g_signal_emit_by_name(mouse,"drag-end",15.0,15.0);
                require(state.selected.size()==1 && modified(),"Smart Select did not pick and move stroke");
                capture("smart-selection");
                std::cout << "PASS: lasso, native resize/rotate, real GDK Copy/Cut/Paste, duplicate, cross-page transfer and Smart Select\n";
                break;
            }
            case 60: {
                smokeClipboardCount=state.ink.size();
                auto* bytes=g_bytes_new_static("malformed",9);
                auto* provider=gdk_content_provider_new_for_bytes(prototype::clipboard::mime,bytes);
                require(gdk_clipboard_set_content(gtk_widget_get_clipboard(canvas),provider),"Test clipboard offer failed");
                g_object_unref(provider);g_bytes_unref(bytes);
                pasteSelection();
                break;
            }
            case 61:
                require(messageDialog && state.ink.size()==smokeClipboardCount && state.selected.size()==1,
                        "Malformed clipboard changed selection or ink");
                answerDialog("close");
                std::cout << "PASS: malformed async clipboard rejected atomically\n";
                break;
            default:
                require(busyHeartbeats > 0, "The UI main loop did not run during document I/O");
                std::cout << "PASS: document actions, async open/save/export-pdf, continuous multi-page viewport, "
                             "Go-to-Page, Insert/Delete Page, saved revisions, native reopen, pressure transfer, "
                             "failed-open/save retention, Save/Discard/Cancel, close protection, native Open/Save "
                             "chooser cancellation, normal Save/Save As, external-edit conflict, background autosave, "
                             "cache-open guard, sidecar recovery\n"
                          << "busy_main_loop_ticks=" << busyHeartbeats
                          << " autosave_main_loop_ticks=" << autosaveHeartbeats << '\n';
                gtk_window_close(GTK_WINDOW(window));
                require(messageDialog, "Close prompt disappeared");
                answerDialog("discard");
                return G_SOURCE_REMOVE;
        }
        update();
        lastSmokePaint = paints;
        gtk_widget_queue_draw(window);
        return G_SOURCE_CONTINUE;
    }

    Options options;
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
    GtkWidget* saveButton = nullptr;
    GtkWidget* saveAsButton = nullptr;
    GtkWidget* exportPdfButton = nullptr;
    GtkWidget* prevPageButton = nullptr;
    GtkWidget* nextPageButton = nullptr;
    GtkWidget* pageButton = nullptr;
    GtkWidget* preferencesButton = nullptr;
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
    std::optional<size_t> selectedImageIndex;
    std::optional<ImageGesture> imageGesture;
    std::optional<Device> textSelectionOwner;
    std::optional<prototype::Point> selectionEdgePanPointer;
    std::optional<prototype::Point> pointerPosition;
    std::optional<prototype::Point> pendingStylusTextTap;
    std::optional<SmartSelectPdfGesture> smartSelectPdfGesture;
    bool textDragHandled = false;
    std::optional<PdfTextSelectionState> pdfTextSelection;
    PopplerDocument* document = nullptr;
    PopplerDocument* pdfTextDocument = nullptr;
    std::string pdfTextDocumentPath;
    std::vector<PopplerDocument*> layerDocuments;
    std::vector<PopplerDocument*> audioLayerDocuments;
    std::vector<prototype::AudioTarget> audioTargets;
    int pageCount = 1;
    bool syncingPage = false;
    GtkWidget* window = nullptr;
    GdkPaintable* windowPaintable = nullptr;
    GdkFrameClock* paintClock = nullptr;
    int paints = 0;
    int lastSmokePaint = 0;
    int paintWaits = 0;
    double selectionVerifyEdgePanOffsetX = 0;
    double selectionVerifyEdgePanScreenX = 0;
    GtkWidget* contentView = nullptr;
    GtkWidget* canvas = nullptr;
    GtkWidget* canvasOverlay = nullptr;
    std::unique_ptr<prototype::TextEditorOverlay> textEditor;
    GtkWidget* textButton = nullptr;
    GtkWidget* linkButton = nullptr;
    GtkWidget* playObjectButton = nullptr;
    GtkWidget* floatingToolboxToolButton = nullptr;
    GtkWidget* floatingToolbox = nullptr;
    GtkWidget* floatingToolboxRowsContainer = nullptr;
    GtkWidget* floatingToolboxApplyButton = nullptr;
    GtkWidget* mainToolRailApplyButton = nullptr;
    size_t floatingToolboxStaticToolButtonCount = 0;
    std::vector<std::pair<Tool, GtkWidget*>> floatingToolboxToolButtons;
    prototype::Point floatingToolboxAnchor{};
    guint floatingToolboxHideSource = 0;
    guint floatingToolboxSmokeSource = 0;
    bool floatingToolboxSmokeReady = true;
    guint floatingToolboxDismissSmokeSource = 0;
    bool floatingToolboxDismissSmokeReady = true;
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
    GtkWidget* widthButton = nullptr;
    GtkWidget* drawingOptionsExpander = nullptr;
    GtkWidget* drawingModeDropdown = nullptr;
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
    AdwPreferencesDialog* preferencesDialog = nullptr;
    GtkWidget* latexTemplatePathEntry = nullptr;
    GtkWidget* latexSetupTestButton = nullptr;
    GtkWidget* latexSetupTestStatus = nullptr;
    GtkWidget* latexSettingsApplyButton = nullptr;
    GtkWidget* latexSettingsApplyStatus = nullptr;
    std::vector<std::string> latexSourceViewThemeIds;
    GtkWindow* pluginManagerWindow = nullptr;
    GtkWidget* pluginManagerList = nullptr;
    GtkWidget* pluginManagerApplyButton = nullptr;
    std::map<std::string, GtkWidget*> pluginManagerSwitches;
    std::array<GtkWidget*, static_cast<size_t>(PreferenceKey::Count)> preferenceControls{};
    GtkListBox* pageList = nullptr;
    std::vector<GtkWidget*> thumbnailWidgets;
    std::map<int, std::pair<std::unique_ptr<cairo_surface_t, SurfaceDeleter>, double>> thumbnails;
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
            Tool::Highlighter,
            prototype::behavior::defaultHighlighterColor,
            prototype::behavior::highlighterWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    prototype::Brush eraserBrush{
            Tool::Eraser,
            prototype::behavior::whiteoutColor,
            prototype::behavior::eraserWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    prototype::Brush laserPenBrush{
            Tool::LaserPointerPen,
            prototype::behavior::laserPointerColor,
            prototype::behavior::laserPenWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    prototype::Brush laserHighlighterBrush{
            Tool::LaserPointerHighlighter,
            prototype::behavior::laserPointerColor,
            prototype::behavior::laserHighlighterWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    bool syncingBrush = false;
    bool syncingTools = false;
    bool syncingPreferenceControls = false;
    GtkGesture* mouse = nullptr;
    GtkGesture* stylus = nullptr;
    GtkEventController* pointerMotion = nullptr;
    std::vector<GtkGesture*> panGestures;
    GtkGesture* pinch = nullptr;
    GtkEventController* keys = nullptr;
    std::vector<PageCacheEntry> pageCache;
    std::vector<PageCacheEntry> layerPageCache;
    uint64_t cacheClock = 0;
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
    int rasterizations = 0;
    int baselineRasters = 0;
    int smokeStep = 0;
    size_t smokeCacheBytes = 0;
    int snapshotWaits = 0;
    double smokeTransformWidth = 0;
    size_t smokeClipboardCount = 0;
    size_t smokeSelectionIndex = 0;
    int smokeWaits = 0;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--dark") {
                options.dark = true;
                options.explicitDark = true;
            } else if (argument == "--autosave-seconds" && i + 1 < argc) {
                const std::string_view raw = argv[++i];
                int parsed = 0;
                const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
                if (ec != std::errc{} || end != raw.data() + raw.size() || parsed < 1 || parsed > 86400) {
                    throw std::runtime_error("Autosave interval must be between 1 and 86400 seconds");
                }
                options.autosaveSeconds = parsed;
                options.autosaveSecondsOverride = parsed;
            } else if (argument == "--disable-audio") {
                options.disableAudio = true;
            } else if (argument == "--config-dir" && i + 1 < argc) {
                const auto cfgPath = std::filesystem::absolute(argv[++i]);
                std::error_code ec;
                const auto st = std::filesystem::symlink_status(cfgPath, ec);
                if (ec || st.type() != std::filesystem::file_type::directory) {
                    throw std::runtime_error("Configuration directory is missing or not a directory");
                }
                options.configDirectory = cfgPath.string();
            } else if (argument == "--plugin-dir" && i + 1 < argc) {
                const auto pluginPath = std::filesystem::absolute(argv[++i]);
                std::error_code ec;
                const auto st = std::filesystem::symlink_status(pluginPath, ec);
                if (ec || st.type() != std::filesystem::file_type::directory) {
                    throw std::runtime_error("Plugin directory is missing or not a directory");
                }
                options.pluginDirectory = pluginPath.string();
            } else if (argument == "--verify-plugins") {
                options.verifyPlugins = true;
            } else if (argument == "--verify-beamer") {
                options.verifyBeamer = true;
            } else if (argument == "--verify-layer-actions") {
                options.verifyLayerActions = true;
            } else if (argument == "--verify-latex") {
                options.verifyLatex = true;
            } else if ((argument == "--pdf" || argument == "--document" || argument == "--worker" ||
                        argument == "--smoke-dir") &&
                       i + 1 < argc) {
                const auto path = std::filesystem::absolute(argv[++i]).string();
                if (argument == "--pdf") {
                    options.pdf = path;
                } else if (argument == "--document") {
                    options.nativeDocument = path;
                } else if (argument == "--worker") {
                    options.worker = path;
                } else {
                    options.smokeDirectory = path;
                    std::filesystem::create_directories(path);
                }
            } else {
                throw std::runtime_error("Usage: xournalpp-gtk4-canvas [--pdf FILE | --document FILE --worker WORKER] "
                                         "[--config-dir DIRECTORY] [--autosave-seconds SECONDS] [--dark] "
                                         "[--plugin-dir DIRECTORY] [--verify-plugins] [--verify-beamer] "
                                         "[--verify-layer-actions] [--verify-latex] "
                                         "[--disable-audio] "
                                         "[--smoke-dir DIRECTORY]");
            }
        }
        if (!options.nativeDocument.empty() && (!options.pdf.empty() || options.worker.empty())) {
            throw std::runtime_error("Use --document with --worker, separately from --pdf");
        }
        if (!options.worker.empty() && !g_file_test(options.worker.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
            throw std::runtime_error("Document worker is missing or not executable");
        }
        if (options.verifyPlugins && options.smokeDirectory.empty()) {
            throw std::runtime_error("Plugin verification requires --smoke-dir DIRECTORY");
        }
        if (options.verifyBeamer && options.smokeDirectory.empty()) {
            throw std::runtime_error("Beamer verification requires --smoke-dir DIRECTORY");
        }
        if (options.verifyLayerActions && options.smokeDirectory.empty()) {
            throw std::runtime_error("LayerActions verification requires --smoke-dir DIRECTORY");
        }
        if (options.verifyLatex && (options.smokeDirectory.empty() || options.nativeDocument.empty() ||
                                    options.worker.empty())) {
            throw std::runtime_error("LaTeX verification requires --smoke-dir, --document and --worker");
        }
        Prototype app(std::move(options));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
