#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <gtk/gtk.h>
#include <gio/gio.h>

#include "CanvasImage.h"

namespace prototype {

struct LatexEditorPreferences {
    std::string sourceViewThemeId;
    bool sourceViewAutoIndent = true;
    bool sourceViewSyntaxHighlight = true;
    bool sourceViewShowLineNumbers = false;
    std::string editorFont{"Monospace 12"};
    bool useCustomEditorFont = false;
    bool editorWordWrap = true;
    bool useExternalEditor = false;
    bool externalEditorAutoConfirm = false;
    std::string externalEditorCommand;
    std::string temporaryFileExtension{"tex"};
};

class LatexEditor final: public std::enable_shared_from_this<LatexEditor> {
public:
    using Completion = std::function<void(std::optional<CanvasImage>)>;
    using SetupTestCompletion = std::function<void(bool, std::string)>;

    static std::shared_ptr<LatexEditor> create(GtkWindow* parent, std::filesystem::path templatePath,
                                               std::string generatorCommand, std::string initialSource,
                                               std::array<double, 3> textColor,
                                               std::optional<CanvasImage> initialImage, bool selectInitialSource,
                                               LatexEditorPreferences preferences, Completion completion);
    static void testConfiguration(std::filesystem::path templatePath, std::string generatorCommand,
                                  std::string source, std::array<double, 3> textColor,
                                  SetupTestCompletion completion);
    ~LatexEditor();

    LatexEditor(const LatexEditor&) = delete;
    LatexEditor& operator=(const LatexEditor&) = delete;

    void present();
    void cancel();
    bool readyForInsertion() const;
    void activateInsert();

private:
    LatexEditor(GtkWindow* parent, std::filesystem::path templatePath, std::string generatorCommand,
                std::string initialSource, std::array<double, 3> textColor,
                std::optional<CanvasImage> initialImage, bool selectInitialSource,
                LatexEditorPreferences preferences, Completion completion);

    void buildWindow();
    void compileCurrentSource();
    void startCompile(std::string source, std::uint64_t revision);
    void finishCompile(GSubprocess* process, GAsyncResult* result, std::uint64_t revision,
                       const std::string& source);
    void setStatus(std::string message, bool valid = false);
    std::string sourceText() const;
    void ensureTempDirectory();
    void launchExternalEditor();
    void finishExternalEditor(GSubprocess* completedProcess, GAsyncResult* result);
    void accept();
    void closeWindow();

    static void drawPreview(GtkDrawingArea* area, cairo_t* cr, int width, int height, gpointer data);
    static void compileComplete(GObject* sourceObject, GAsyncResult* result, gpointer data);
    static void externalEditorComplete(GObject* sourceObject, GAsyncResult* result, gpointer data);
    static void setupTestComplete(GObject* sourceObject, GAsyncResult* result, gpointer data);

    GtkWindow* parent = nullptr;
    GtkWindow* window = nullptr;
    GtkTextView* sourceView = nullptr;
    GtkTextBuffer* sourceBuffer = nullptr;
    GtkWidget* sourceScroll = nullptr;
    GtkWidget* externalEditorPanel = nullptr;
    GtkWidget* externalEditorButton = nullptr;
    GtkLabel* externalEditorStatus = nullptr;
    GtkDrawingArea* previewArea = nullptr;
    GtkLabel* statusLabel = nullptr;
    GtkTextBuffer* outputBuffer = nullptr;
    GtkWidget* insertButton = nullptr;
    std::filesystem::path templatePath;
    std::filesystem::path tempDirectory;
    std::filesystem::path externalEditorFile;
    std::string generatorCommand;
    std::string templateText;
    std::string initialSource;
    std::array<double, 3> textColor{};
    LatexEditorPreferences preferences;
    std::optional<CanvasImage> currentImage;
    Completion completion;
    bool selectInitialSource = false;
    GSubprocess* process = nullptr;
    GCancellable* cancellable = nullptr;
    GSubprocess* externalEditorProcess = nullptr;
    GCancellable* externalEditorCancellable = nullptr;
    GtkCssProvider* sourceCssProvider = nullptr;
    std::uint64_t requestedRevision = 0;
    bool compileRunning = false;
    bool externalEditorOpen = false;
    bool externalEditorCompleted = false;
    bool closed = false;
};

}  // namespace prototype
