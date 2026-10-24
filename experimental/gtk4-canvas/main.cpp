#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <adwaita.h>
#include <poppler.h>

#include "BehaviorDefaults.h"
#include "CanvasInput.h"
#include "CanvasState.h"
#include "DocumentAutosave.h"
#include "DocumentSession.h"
#include "DocumentJob.h"
#include "DocumentInk.h"
#include "InkRenderer.h"
#include "TextRenderer.h"
#include "PaletteImport.h"
#include "SettingsImport.h"
#include "StylusSamples.h"
#include "StrokeClipboard.h"
#include "TextEditorOverlay.h"

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
    std::string smokeDirectory;
    int autosaveSeconds = prototype::behavior::autosaveTimeoutSeconds;
    std::optional<int> autosaveSecondsOverride;
    bool dark = false;
    bool explicitDark = false;
    int width = 1100;
    int height = 820;
};

struct RecoveryInfo {
    std::string saveTarget;
    std::string saveVersion;
    std::string autosavePath;
};

struct SurfaceDeleter {
    void operator()(cairo_surface_t* surface) const { cairo_surface_destroy(surface); }
};

struct PageCacheEntry {
    int pageIndex = -1;
    double scale = 0;
    uint64_t stamp = 0;
    std::unique_ptr<cairo_surface_t, SurfaceDeleter> surface;
};

class Prototype {
public:
    explicit Prototype(Options options): options(std::move(options)) {
        const auto cfgDir = !this->options.configDirectory.empty() ?
                                    std::filesystem::path(this->options.configDirectory) :
                                    (configRoot() / "xournalpp");
        importedConfig = prototype::importSettingsFromConfigDir(cfgDir);
        if (!importedConfig.warning.empty()) {
            std::cerr << "xournalpp-gtk4-canvas: " << importedConfig.warning << '\n';
        }
        prototype::applyImportedSettingsToCanvas(state, importedConfig.settings);
        input.setStabilizerConfig(importedConfig.settings.stabilizer);
        penBrush = {Tool::Pen, importedConfig.settings.penColor, importedConfig.settings.penWidth};
        highlightBrush = {Tool::Highlighter, importedConfig.settings.highlighterColor,
                          importedConfig.settings.highlighterWidth};
        eraserBrush = {Tool::Eraser, prototype::behavior::whiteoutColor, importedConfig.settings.eraserWidth};
        state.brush = importedConfig.settings.activeTool == Tool::Highlighter ?
                              highlightBrush :
                              (importedConfig.settings.activeTool == Tool::Eraser ? eraserBrush : penBrush);
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
    }

    ~Prototype() {
        for (guint* source: {&heartbeatSource, &autosaveSource, &crashHelperSource, &recoveryVerifySource}) {
            if (*source) {
                g_source_remove(*source);
                *source = 0;
            }
        }
        if (closed) {
            cleanupActiveAutosave();
        }
        if (paintClock) {
            g_signal_handlers_disconnect_by_data(paintClock, this);
            g_clear_object(&paintClock);
        }
        g_clear_object(&windowPaintable);
        if (document) {
            g_object_unref(document);
        }
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
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto previous = activeAutosavePath;
        autosaving = true;
        updateTitle();
        auto* application = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window)));
        g_application_hold(application);
        prototype::bridge::documentJob(
                [session, worker, target, packets, texts] { session->autosave(worker, target, packets, texts); },
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

    void showError(const std::string& message) {
        ++ioErrors;
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Document operation failed",
                                                                  message.c_str()));
        adw_alert_dialog_add_response(messageDialog, "close", "Close");
        adw_alert_dialog_set_close_response(messageDialog, "close");
        g_signal_connect(messageDialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
            static_cast<Prototype*>(data)->messageDialog = nullptr;
        }), this);
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
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
        auto hydratedInk = [&] {
            try {
                return prototype::bridge::sessionInk(session->initialInk);
            } catch (...) {
                g_object_unref(candidate);
                throw;
            }
        }();
        // Commit the replacement only after the new native snapshot and preview
        // are valid. Failed opens leave the old document and undo history intact.
        if (!recovery || recovery->autosavePath != activeAutosavePath) {
            cleanupActiveAutosave();
        }
        input.reset();
        const auto brush = state.brush;
        const auto eraserMode = state.eraserMode;
        const auto eraserWidth = state.eraserWidth;
        state = CanvasState{};
        prototype::applyImportedSettingsToCanvas(state, importedConfig.settings);
        state.setPages(std::move(pageSizes));
        state.selectActivePage(0);
        state.ink = std::move(hydratedInk);
        state.texts = session->initialTexts;
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
        pageCount = count;
        nativeSession = session;
        sourcePath = source;
        saveTarget = recovery ? recovery->saveTarget : session->originalSavePath;
        saveVersion = recovery ? recovery->saveVersion : session->originalVersion;
        pageCache.clear();
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
        runDocumentJob(
                "Opening document…",
                [loaded, worker, path] {
                    *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, path);
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
        runDocumentJob(
                "Recovering autosave…",
                [loaded, targetSave, targetVersion, worker, path, sidecar] {
                    *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, sidecar);
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
        runDocumentJob(
                "Restoring recovery file…",
                [loaded, worker, recoveryPath, adopted] {
                    *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, recoveryPath);
                    (*loaded)->autosave(worker, adopted, (*loaded)->initialInk, (*loaded)->initialTexts);
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
        const auto session = nativeSession;
        const auto worker = options.worker;
        runDocumentJob("Saving copy…", [session, worker, path, packets, texts] {
            session->saveCopy(worker, path, packets, texts);
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
        const auto session = nativeSession;
        const auto worker = options.worker;
        const auto version = std::make_shared<std::string>();
        runDocumentJob("Saving…", [session, worker, path, expected, packets, texts, version] {
            const auto before = expected.empty() ? prototype::bridge::documentVersion(path).token : expected;
            *version = session->save(worker, path, packets, texts, before);
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
        const auto session = nativeSession;
        const auto worker = options.worker;
        // Exporting to PDF never alters .xopp savedRevision, saveTarget, or active autosave artifacts.
        runDocumentJob("Exporting PDF…",
                       [session, worker, path, packets, texts] {
                           session->exportPdf(worker, path, packets, texts);
                       },
                       [this, path] { exportedPdfPath = path; });
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
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Inserting page…",
                [session, worker, afterPage, staged] {
                    *staged = session->stageInsertPageAfter(worker, afterPage);
                },
                [this, session, afterPage, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<prototype::PageSize> sizes;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                    } catch (...) {
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }
                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    pageCount = static_cast<int>(sizes.size());
                    state.insertPageAfter(afterPage, sizes.at(static_cast<size_t>(afterPage + 1)));
                    state.setPages(std::move(sizes));
                    state.selectActivePage(afterPage + 1);
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    pageCache.clear();
                    thumbnails.clear();
                    thumbnailWidgets.clear();
                    pageList = nullptr;
                    adw_overlay_split_view_set_sidebar(splitView, createSidebar());
                    navigateToPage(afterPage + 1);
                });
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
        auto staged = std::make_shared<prototype::bridge::DocumentSession::StagedRevision>();
        runDocumentJob(
                "Deleting page…",
                [session, worker, removeIndex, staged] {
                    *staged = session->stageDeletePageAt(worker, removeIndex);
                },
                [this, session, removeIndex, staged] {
                    PopplerDocument* candidate = nullptr;
                    std::vector<prototype::PageSize> sizes;
                    try {
                        std::tie(candidate, sizes) = openPreviewPdf(staged->previewPath().string());
                    } catch (...) {
                        prototype::bridge::DocumentSession::discardRevision(*staged);
                        throw;
                    }
                    session->commitRevision(*staged);
                    g_clear_object(&document);
                    document = candidate;
                    pageCount = static_cast<int>(sizes.size());
                    state.deletePageAt(removeIndex);
                    state.setPages(std::move(sizes));
                    const int nextActive = std::min(removeIndex, pageCount - 1);
                    state.selectActivePage(nextActive);
                    savedRevision.reset();
                    savedNormally = false;
                    savedCopyPath.clear();
                    pageCache.clear();
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

    void activateSelection(Tool tool) {
        selectionTool = tool;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
        selectTool(tool);
    }

    void copySelection(bool cut = false) {
        if (busy || state.selected.empty()) return;
        resetInput(true);
        try {
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
        if (busy || state.selected.empty()) return;
        resetInput(true);
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
                const std::string message = error ? error->message : "Clipboard has no editable Xournal strokes";
                g_clear_error(&error);
                self->finishClipboardRead(message);
                return;
            }
            auto stream = std::shared_ptr<GInputStream>(rawStream, [](GInputStream* stream) { g_object_unref(stream); });
            auto decoded = std::make_shared<std::vector<Ink>>();
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
                *decoded = prototype::clipboard::decode(bytes);
            }, [self, decoded](const std::string& error) {
                std::string failure = error;
                if (failure.empty()) {
                    const auto sz = self->state.pageSize(self->state.page);
                    auto center = self->state.pageLocalPoint(self->state.page,
                            gtk_widget_get_width(self->canvas)/2.0, gtk_widget_get_height(self->canvas)/2.0);
                    center.x = std::clamp(center.x, 0.0, sz.width);
                    center.y = std::clamp(center.y, 0.0, sz.height);
                    if (!self->state.pasteStrokes(std::move(*decoded), self->state.page, center))
                        failure = "Cannot place clipboard strokes";
                    else self->activateSelection(self->selectionTool);
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
            if (item.page == page && prototype::textContainsPoint(item, point.x, point.y) &&
                (!best || item.layerOrder >= bestOrder)) {
                best = index;
                bestOrder = item.layerOrder;
            }
        }
        return best;
    }

    void startTextEditing(double x, double y) {
        if (!textEditor || !nativeSession || busy || choosingFile || messageDialog) {
            return;
        }
        const auto hit = state.hitPage(x, y);
        if (!hit) {
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
        } else {
            item.page = hit->page;
            item.x = hit->local.x;
            item.y = hit->local.y;
            item.fontName = state.textFontName;
            item.fontSize = state.textFontSize;
            const auto red = static_cast<uint32_t>(std::lround(std::clamp(state.brush.color[0], 0.0, 1.0) * 255));
            const auto green = static_cast<uint32_t>(std::lround(std::clamp(state.brush.color[1], 0.0, 1.0) * 255));
            const auto blue = static_cast<uint32_t>(std::lround(std::clamp(state.brush.color[2], 0.0, 1.0) * 255));
            item.color = 0xff000000U | (red << 16U) | (green << 8U) | blue;
        }
        textEditor->begin(item, index, state.zoom);
        textEditor->setWrap(item.wrapWidth >= 0);
        gtk_editable_set_text(GTK_EDITABLE(textFontEntry), item.fontName.c_str());
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), item.fontSize);
        updateTextEditorPosition();
        update();
    }

    void finishTextEditing() {
        if (!textEditor || !textEditor->active()) {
            return;
        }
        const auto completed = textEditor->finish();
        if (completed) {
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
        }
    }

    void selectTool(Tool tool) {
        if (textEditor && textEditor->active()) {
            finishTextEditing();
        }
        syncingTools = true;
        if (textButton) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(textButton), tool == Tool::Text);
        }
        if (prototype::isSelectionTool(tool)) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[3]), true);
        } else if (tool == Tool::Text) {
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
        }
        input.cancel();
        if (!prototype::isSelectionTool(tool)) {
            state.clearSelection();
        }
        state.brush = tool == Tool::Highlighter ? highlightBrush : (tool == Tool::Eraser ? eraserBrush : penBrush);
        state.brush.tool = tool;
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
        gtk_widget_set_visible(colorBox, tool != Tool::Eraser && !prototype::isSelectionTool(tool));
        gtk_widget_set_visible(sizeDropdown, !prototype::isSelectionTool(tool) && tool != Tool::Text);
        gtk_widget_set_visible(widthButton, !prototype::isSelectionTool(tool) && tool != Tool::Text);
        gtk_widget_set_visible(selectionLabel, prototype::isSelectionTool(tool));
        gtk_widget_set_visible(selectionModeDropdown, prototype::isSelectionTool(tool));
        gtk_widget_set_visible(selectionActions, prototype::isSelectionTool(tool));
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
            gtk_drop_down_set_selected(GTK_DROP_DOWN(selectionModeDropdown), static_cast<guint>(tool)-3);
        }
        gtk_widget_set_sensitive(colorButton, tool != Tool::Eraser);
        gtk_widget_set_sensitive(paletteButton, tool != Tool::Eraser);
        gtk_widget_set_visible(eraserModeDropdown, tool == Tool::Eraser);
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

    GtkWidget* createPalette() {
        paletteSwatchButtons.clear();
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
            const auto& entry = importedConfig.palette.swatches[i];
            auto* swatch = gtk_button_new();
            gtk_widget_add_css_class(swatch, "flat");
            auto* dot = gtk_drawing_area_new();
            gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(dot), 20);
            gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(dot), 20);
            g_object_set_data(G_OBJECT(dot), "swatch-index", GUINT_TO_POINTER(static_cast<guint>(i)));
            gtk_drawing_area_set_draw_func(
                    GTK_DRAWING_AREA(dot),
                    +[](GtkDrawingArea* area, cairo_t* cr, int w, int h, gpointer data) {
                        auto* self = static_cast<Prototype*>(data);
                        const auto idx =
                                static_cast<size_t>(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(area), "swatch-index")));
                        if (idx >= self->importedConfig.palette.swatches.size()) {
                            return;
                        }
                        const auto& c = self->importedConfig.palette.swatches[idx].color;
                        const double cx = w / 2.0;
                        const double cy = h / 2.0;
                        const double r = std::max(2.0, std::min(w, h) / 2.0 - 2.0);
                        cairo_arc(cr, cx, cy, r, 0, 2 * G_PI);
                        cairo_set_source_rgb(cr, c[0], c[1], c[2]);
                        cairo_fill_preserve(cr);
                        cairo_set_source_rgba(cr, 0.45, 0.45, 0.45, 0.65);
                        cairo_set_line_width(cr, 1.0);
                        cairo_stroke(cr);
                    },
                    this, nullptr);
            gtk_button_set_child(GTK_BUTTON(swatch), dot);
            gtk_widget_set_tooltip_text(swatch, entry.name.c_str());
            gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL, entry.name.c_str(),
                                           -1);
            g_object_set_data(G_OBJECT(swatch), "swatch-index", GUINT_TO_POINTER(static_cast<guint>(i)));
            g_signal_connect(swatch, "clicked", G_CALLBACK((+[](GtkButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 const auto idx = static_cast<size_t>(
                                         GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "swatch-index")));
                                 if (idx < self->importedConfig.palette.swatches.size()) {
                                     const auto& c = self->importedConfig.palette.swatches[idx].color;
                                     const GdkRGBA rgba{static_cast<float>(c[0]), static_cast<float>(c[1]),
                                                        static_cast<float>(c[2]), 1.0f};
                                     gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(self->colorButton),
                                                                      &rgba);
                                 }
                                 gtk_popover_popdown(GTK_POPOVER(self->palettePopover));
                                 gtk_widget_grab_focus(self->canvas);
                             })),
                             this);
            gtk_grid_attach(GTK_GRID(grid), swatch, static_cast<int>(i) % columns, static_cast<int>(i) / columns, 1, 1);
            paletteSwatchButtons.push_back(swatch);
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
        return box;
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
            if (document) {
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
        for (size_t index = 0; index < state.ink.size(); ++index) {
            if (state.displayPage(index) == number && state.ink[index].page == number) {
                prototype::drawInkPreview(cr, state, index);
            }
        }
        for (size_t index = 0; index < state.ink.size(); ++index) {
            if (state.displayPage(index) == number && state.ink[index].page != number)
                prototype::drawInkPreview(cr, state, index);
        }
        if (state.current && state.current->page == number) {
            prototype::drawInk(cr, *state.current);
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

        auto* view = adw_toolbar_view_new();
        contentView = view;
        auto* header = adw_header_bar_new();
        heading = adw_window_title_new("Blank notebook", "");
        adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), heading);
        sidebarButton = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(sidebarButton), "view-sidebar-start-symbolic");
        gtk_widget_set_tooltip_text(sidebarButton, "Show pages");
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebarButton);
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
                                self->input.cancel();
                                self->state.undo();
                                self->update();
                            }));
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), undoButton);
        redoButton = button("edit-redo-symbolic", "Redo ink (Ctrl+Shift+Z)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                auto* self = static_cast<Prototype*>(data);
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
        gtk_widget_set_halign(toolbar, GTK_ALIGN_CENTER);
        auto* toolGroup = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(toolGroup, "linked");
        const std::array names = {"Pen", "Highlight", "Eraser", "Select"};
        for (size_t i = 0; i < toolButtons.size(); ++i) {
            toolButtons[i] = gtk_toggle_button_new_with_label(names[i]);
            if (i) {
                gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(toolButtons[i]), GTK_TOGGLE_BUTTON(toolButtons[0]));
            }
            g_object_set_data(G_OBJECT(toolButtons[i]), "tool", GUINT_TO_POINTER(i));
            g_signal_connect(toolButtons[i], "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 if (gtk_toggle_button_get_active(button) && !self->syncingTools) {
                                     const auto index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "tool"));
                                     self->selectTool(index == 3 ? self->selectionTool : static_cast<Tool>(index));
                                 }
                             }),
                             this);
            gtk_box_append(GTK_BOX(toolGroup), toolButtons[i]);
        }
        gtk_widget_set_tooltip_text(toolButtons[3], "Rectangle (Ctrl+Shift+R), Lasso (Ctrl+Shift+G), Smart (S)");
        textButton = gtk_toggle_button_new();
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
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), textButton);
        gtk_box_append(GTK_BOX(toolbar), toolGroup);

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
        g_signal_connect(colorButton, "notify::rgba", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush && self->state.brush.tool != Tool::Eraser) {
                                 const auto* color =
                                         gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(self->colorButton));
                                 self->state.brush.color[0] = color->red;
                                 self->state.brush.color[1] = color->green;
                                 self->state.brush.color[2] = color->blue;
                                 self->input.cancel();
                                 self->update();
                             }
                         }),
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

        const char* selectionModes[] = {"Rectangle", "Lasso", "Smart", nullptr};
        selectionModeDropdown = gtk_drop_down_new_from_strings(selectionModes);
        g_signal_connect(selectionModeDropdown, "notify::selected", G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer data) {
            auto* self = static_cast<Prototype*>(data);
            const auto tool = static_cast<Tool>(3 + gtk_drop_down_get_selected(GTK_DROP_DOWN(object)));
            if (tool != self->state.brush.tool && prototype::isSelectionTool(self->state.brush.tool)) self->activateSelection(tool);
        }), this);
        gtk_box_append(GTK_BOX(toolbar), selectionModeDropdown);
        selectionActions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(selectionActions, "linked");
        gtk_box_append(GTK_BOX(selectionActions), button("edit-copy-symbolic", "Copy strokes (Ctrl+C)",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->copySelection(); })));
        gtk_box_append(GTK_BOX(selectionActions), button("edit-cut-symbolic", "Cut strokes (Ctrl+X)",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->copySelection(true); })));
        gtk_box_append(GTK_BOX(selectionActions), button("edit-paste-symbolic", "Paste strokes (Ctrl+V)",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->pasteSelection(); })));
        gtk_box_append(GTK_BOX(selectionActions), button("edit-duplicate-symbolic", "Duplicate strokes",
                G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->duplicateSelection(); })));
        gtk_box_append(GTK_BOX(toolbar), selectionActions);
        selectionLabel = gtk_label_new("Drag to select");
        gtk_widget_set_tooltip_text(selectionLabel, "Drag to move, drag a square to resize, or drag the circle to rotate");
        gtk_box_append(GTK_BOX(toolbar), selectionLabel);
        const auto initialToolIdx = static_cast<size_t>(importedConfig.settings.activeTool);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[initialToolIdx]), true);
        selectTool(importedConfig.settings.activeTool);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), toolbar);

        auto* navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(navigation, "toolbar");
        gtk_widget_set_halign(navigation, GTK_ALIGN_CENTER);
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
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), navigation);

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
        adw_breakpoint_bin_set_child(ADW_BREAKPOINT_BIN(adaptive), GTK_WIDGET(splitView));
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
        if (!options.smokeDirectory.empty()) {
            if (const char* verifyMode = g_getenv("XOURNALPP_SETTINGS_VERIFY_MODE"); verifyMode && *verifyMode) {
                settingsVerifyMode = verifyMode;
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
        updateTextEditorPosition();
        updateTitle();
        gtk_label_set_text(GTK_LABEL(selectionLabel), state.selected.empty() ? "Drag to select" :
                               (std::to_string(state.selected.size()) + " selected").c_str());
        gtk_widget_set_sensitive(openButton, !options.worker.empty());
        gtk_widget_set_sensitive(saveButton, bool(nativeSession));
        gtk_widget_set_sensitive(saveAsButton, bool(nativeSession));
        gtk_widget_set_sensitive(exportPdfButton, bool(nativeSession));
        gtk_widget_set_sensitive(textButton, bool(nativeSession));
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

    void resetInput(bool interrupted = false) {
        if (interrupted) {
            input.interrupt();
        } else {
            input.reset();
        }
        for (auto* pan: panGestures) {
            static_cast<PanState*>(g_object_get_data(G_OBJECT(pan), "pan-state"))->enabled = false;
        }
        if (canvas) {
            gtk_widget_set_cursor_from_name(canvas, nullptr);
        }
    }

    void begin(Device device, double x, double y, std::optional<double> pressure = std::nullopt,
               bool eraserEnd = false, uint32_t timeMs = 0) {
        gtk_widget_grab_focus(canvas);
        input.begin(device, {x, y, pressure, timeMs}, eraserEnd);
        if (input.panning()) {
            gtk_widget_set_cursor_from_name(canvas, "grabbing");
        }
        update();
    }

    void motion(Device device, double x, double y, std::optional<double> pressure = std::nullopt,
                uint32_t timeMs = 0) {
        input.motion(device, {x, y, pressure, timeMs});
        if (input.panning()) {
            fitted = true;
            fitMode = false;
            state.syncActivePage(gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
        }
        update();
    }

    void finish(Device device, std::optional<double> releasePressure = std::nullopt) {
        input.finish(device, releasePressure);
        if (!input.active()) {
            gtk_widget_set_cursor_from_name(canvas, input.spaceHeld ? "grab" : nullptr);
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
                input.motion(Device::Stylus, *point);
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
        g_signal_connect(mouse, "drag-end", G_CALLBACK(+[](GtkGestureDrag* gesture, double, double, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (mouseEvent(GTK_EVENT_CONTROLLER(gesture)) &&
                                 self->state.drawingMode != prototype::DrawingMode::Spline) {
                                 self->finish(Device::Mouse);
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
                             if (presses != 1) return;
                             const auto tool = self->state.brush.tool;
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
        g_signal_connect(stylus, "up", G_CALLBACK(+[](GtkGestureStylus* gesture, double, double, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             self->finish(Device::Stylus, pressure(gesture));
                             self->update();
                         }),
                         this);
        addGesture(stylus, Device::Stylus);

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
                    self->input.setModifiers((modifiers & GDK_SHIFT_MASK) || key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R,
                                             (modifiers & GDK_CONTROL_MASK) || key == GDK_KEY_Control_L || key == GDK_KEY_Control_R,
                                             (modifiers & GDK_ALT_MASK) || key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R);
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
                            self->copySelection(); return true;
                        }
                        if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_x || key == GDK_KEY_X)) {
                            self->copySelection(true); return true;
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
                        if (self->input.splineInProgress) {
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
                        self->input.cancel();
                        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toolButtons[3]), true);
                        self->state.selectAllOnPage();
                    } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                               (key == GDK_KEY_Delete || key == GDK_KEY_BackSpace) && !self->state.selected.empty()) {
                        self->input.cancel();
                        self->state.deleteSelection();
                    } else if (key == GDK_KEY_space && !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK))) {
                        self->input.spaceHeld = true;
                        gtk_widget_set_cursor_from_name(self->canvas, "grab");
                    } else if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_z || key == GDK_KEY_Z)) {
                        if (modifiers & GDK_SHIFT_MASK) {
                            self->input.cancel();
                            self->state.redo();
                        } else {
                            self->input.cancel();
                            self->state.undo();
                        }
                    } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_y) {
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
                                (key == GDK_KEY_t && self->nativeSession))) {
                        if (key == GDK_KEY_t) {
                            self->selectTool(Tool::Text);
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
                             if (key == GDK_KEY_space) {
                                 self->input.spaceHeld = false;
                                 gtk_widget_set_cursor_from_name(self->canvas,
                                                                 self->input.panning() ? "grabbing" : nullptr);
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
            if (entry.pageIndex == pageIndex && std::abs(entry.scale - scale) < 0.0001) {
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
            if (entry.pageIndex == pageIndex) {
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
        slot->scale = scale;
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
        const double background = options.dark ? 0.10 : 0.88;
        cairo_set_source_rgb(cr, background, background, background);
        cairo_paint(cr);
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
            cairo_set_source_rgba(cr, 0, 0, 0, options.dark ? 0.45 : 0.18);
            cairo_rectangle(cr, sx0 - 0.5, sy0 - 0.5, (sx1 - sx0) + 1.0, (sy1 - sy0) + 1.0);
            cairo_set_line_width(cr, 1.0);
            cairo_stroke(cr);
            cairo_restore(cr);

            cairo_save(cr);
            cairo_translate(cr, sx0, sy0);
            cairo_scale(cr, state.zoom, state.zoom);
            cairo_rectangle(cr, 0, 0, sz.width, sz.height);
            cairo_clip(cr);
            cairo_save(cr);
            cairo_scale(cr, 1.0 / cached.scale, 1.0 / cached.scale);
            cairo_set_source_surface(cr, cached.surface.get(), 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
            struct LayerItem {
                size_t order;
                bool text;
                size_t index;
            };
            std::vector<LayerItem> layerItems;
            layerItems.reserve(state.ink.size() + state.texts.size());
            for (size_t index = 0; index < state.ink.size(); ++index) {
                if (state.displayPage(index) == i && state.ink[index].page == i) {
                    layerItems.push_back({state.ink[index].layerOrder, false, index});
                }
            }
            for (size_t index = 0; index < state.texts.size(); ++index) {
                if (state.texts[index].page == i) {
                    layerItems.push_back({state.texts[index].layerOrder, true, index});
                }
            }
            std::stable_sort(layerItems.begin(), layerItems.end(),
                             [](const LayerItem& a, const LayerItem& b) { return a.order < b.order; });
            for (const auto& item: layerItems) {
                if (item.text) {
                    prototype::drawText(cr, state.texts[item.index]);
                } else {
                    prototype::drawInkPreview(cr, state, item.index);
                }
            }
            for (size_t index = 0; index < state.ink.size(); ++index) {
                if (state.displayPage(index) == i && state.ink[index].page != i)
                    prototype::drawInkPreview(cr, state, index);
            }
            if (state.current && state.current->page == i) {
                prototype::drawInk(cr, *state.current);
            }
            prototype::drawSelection(cr, state, i);
            cairo_restore(cr);
        }
        if (!drewAny) {
            ensurePageCache(state.page, deviceScale);
        }
        ++frames;
    }

    void capture(const std::string& name, GtkWidget* target = nullptr) {
        // Snapshot the window so a crop retains ancestor backgrounds and styling.
        auto* paintable = GDK_PAINTABLE(g_object_ref(windowPaintable));
        auto* snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(window),
                               gtk_widget_get_height(window));
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
                                   static_cast<guint>(prototype::DrawingMode::Rectangle));
        const auto start = state.pageScreenPoint(0, 190, 230);
        begin(Device::Mouse, start.x, start.y);
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
        if (!frames || pageCache.empty()) {
            require(++smokeWaits < 60, "Canvas has not rendered");
            return G_SOURCE_CONTINUE;
        }
        if (!settingsVerifyMode.empty()) {
            return smokeSettingsVerify();
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
                                    !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toolButtons[0])),
                            "Text tool did not show its selected state in the header");
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
                                state.canUndo(),
                        "Inline text creation did not preserve its UTF-8 content, size and formatting");
                state.undo();
                require(state.texts.size() == newTextIndex, "Undo did not remove a newly created text item");
                state.redo();
                require(state.texts.size() == newTextIndex + 1, "Redo did not restore a newly created text item");

                activateSelection(Tool::SmartSelect);
                const auto created = state.texts.back();
                const auto editAt = state.pageScreenPoint(created.page, created.x + 2, created.y + 2);
                g_signal_emit_by_name(textClick, "released", 1, editAt.x, editAt.y);
                require(textEditor->active(), "Smart Select click did not edit native text");
                buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
                GtkTextIter textEnd;
                gtk_text_buffer_get_end_iter(buffer, &textEnd);
                gtk_text_buffer_insert(buffer, &textEnd, "!", 1);
                finishTextEditing();
                require(state.texts.back().text == "GTK rich π!" && !state.texts.back().styleRuns.empty(),
                        "Editing existing rich text lost content or its style runs");
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
                packets.push_back({0, false, false, 2, {0, 1, 0}, {{200, 220, 1}, {230, 220, 1}}});
                const auto session = nativeSession;
                const auto worker = options.worker;
                const auto target = saveTarget;
                const auto before = saveVersion;
                runDocumentJob("Testing external edit…", [session, worker, target, before, packets, texts] {
                    session->save(worker, target, packets, texts, before);
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
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
                const auto a = state.pageScreenPoint(0, 260, 300);
                const auto b = state.pageScreenPoint(0, 360, 320);
                const auto c = state.pageScreenPoint(0, 300, 380);
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
                const auto from = state.pageScreenPoint(0,250,290);
                g_signal_emit_by_name(mouse,"drag-begin",from.x,from.y);
                for (const auto p: {prototype::Point{370,290},prototype::Point{370,390},prototype::Point{250,390},prototype::Point{250,290}}) {
                    const auto to = state.pageScreenPoint(0,p.x,p.y);
                    g_signal_emit_by_name(mouse,"drag-update",to.x-from.x,to.y-from.y);
                }
                g_signal_emit_by_name(mouse,"drag-end",0.0,0.0);
                require(state.selected == std::vector<size_t>{smokeSelectionIndex},"Lasso did not select native editable stroke");
                break;
            }
            case 53: {
                capture("lasso-selection");
                const auto handles = state.selectionHandles();
                const auto a = state.pageScreenPoint(0,handles[4].x,handles[4].y);
                const auto b = state.pageScreenPoint(0,handles[0].x+1.5*(handles[4].x-handles[0].x),
                                                         handles[0].y+1.5*(handles[4].y-handles[0].y));
                smokeTransformWidth = state.ink.back().brush.width;
                g_signal_emit_by_name(mouse,"drag-begin",a.x,a.y);
                g_signal_emit_by_name(mouse,"drag-update",b.x-a.x,b.y-a.y);
                require(state.committedInk().back().brush.width == smokeTransformWidth,"Resize preview leaked into autosave");
                g_signal_emit_by_name(mouse,"drag-end",b.x-a.x,b.y-a.y);
                require(std::abs(state.ink.back().brush.width-1.5*smokeTransformWidth)<1e-6,"Resize lost native width scaling");
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
    prototype::SettingsImportResult importedConfig;
    int effectiveAutosaveSeconds = prototype::behavior::autosaveTimeoutSeconds;
    std::string settingsVerifyMode;
    int settingsVerifyStep = 0;
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
    std::function<void()> afterConfirmation;
    GtkWidget* heading = nullptr;
    GtkWidget* openButton = nullptr;
    GtkWidget* saveButton = nullptr;
    GtkWidget* saveAsButton = nullptr;
    GtkWidget* exportPdfButton = nullptr;
    GtkWidget* prevPageButton = nullptr;
    GtkWidget* nextPageButton = nullptr;
    GtkWidget* pageButton = nullptr;
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
    PopplerDocument* document = nullptr;
    int pageCount = 1;
    bool syncingPage = false;
    GtkWidget* window = nullptr;
    GdkPaintable* windowPaintable = nullptr;
    GdkFrameClock* paintClock = nullptr;
    int paints = 0;
    int lastSmokePaint = 0;
    int paintWaits = 0;
    GtkWidget* contentView = nullptr;
    GtkWidget* canvas = nullptr;
    GtkWidget* canvasOverlay = nullptr;
    std::unique_ptr<prototype::TextEditorOverlay> textEditor;
    GtkWidget* textButton = nullptr;
    GtkWidget* textFontEntry = nullptr;
    GtkWidget* textFontSize = nullptr;
    GtkWidget* textFormatBox = nullptr;
    std::array<GtkWidget*, 6> textFormatButtons{};
    GtkGestureClick* textClick = nullptr;
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
    std::vector<GtkWidget*> paletteSwatchButtons;
    GtkWidget* eraserModeDropdown = nullptr;
    GtkWidget* sizeDropdown = nullptr;
    GtkWidget* widthButton = nullptr;
    GtkWidget* drawingOptionsExpander = nullptr;
    GtkWidget* drawingModeDropdown = nullptr;
    GtkWidget* averagingDropdown = nullptr;
    GtkWidget* preprocessorDropdown = nullptr;
    std::array<GtkWidget*, 4> toolButtons{};
    GtkWidget* selectionLabel = nullptr;
    GtkWidget* selectionModeDropdown = nullptr;
    GtkWidget* selectionActions = nullptr;
    Tool selectionTool = Tool::SelectRectangle;
    GCancellable* clipboardCancellation = nullptr;
    guint clipboardTimer = 0;
    AdwOverlaySplitView* splitView = nullptr;
    GtkListBox* pageList = nullptr;
    std::vector<GtkWidget*> thumbnailWidgets;
    std::map<int, std::pair<std::unique_ptr<cairo_surface_t, SurfaceDeleter>, double>> thumbnails;
    prototype::Brush penBrush;
    prototype::Brush highlightBrush{
            Tool::Highlighter,
            prototype::behavior::defaultHighlighterColor,
            prototype::behavior::highlighterWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    prototype::Brush eraserBrush{
            Tool::Eraser,
            prototype::behavior::whiteoutColor,
            prototype::behavior::eraserWidths[static_cast<size_t>(prototype::behavior::ToolSize::Medium)]};
    bool syncingBrush = false;
    bool syncingTools = false;
    GtkGesture* mouse = nullptr;
    GtkGesture* stylus = nullptr;
    GtkEventController* pointerMotion = nullptr;
    std::vector<GtkGesture*> panGestures;
    GtkGesture* pinch = nullptr;
    GtkEventController* keys = nullptr;
    std::vector<PageCacheEntry> pageCache;
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
            } else if (argument == "--config-dir" && i + 1 < argc) {
                const auto cfgPath = std::filesystem::absolute(argv[++i]);
                std::error_code ec;
                const auto st = std::filesystem::symlink_status(cfgPath, ec);
                if (ec || st.type() != std::filesystem::file_type::directory) {
                    throw std::runtime_error("Configuration directory is missing or not a directory");
                }
                options.configDirectory = cfgPath.string();
            } else if ((argument == "--pdf" || argument == "--document" || argument == "--worker" ||
                        argument == "--smoke-dir") && i + 1 < argc) {
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
                                         "[--smoke-dir DIRECTORY]");
            }
        }
        if (!options.nativeDocument.empty() && (!options.pdf.empty() || options.worker.empty())) {
            throw std::runtime_error("Use --document with --worker, separately from --pdf");
        }
        if (!options.worker.empty() && !g_file_test(options.worker.c_str(), G_FILE_TEST_IS_EXECUTABLE)) {
            throw std::runtime_error("Document worker is missing or not executable");
        }
        Prototype app(std::move(options));
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
