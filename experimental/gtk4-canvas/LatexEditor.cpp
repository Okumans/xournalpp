#include "LatexEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <vector>

#include <adwaita.h>
#include <gio/gio.h>
#include <poppler.h>
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
#include <gtksourceview/gtksource.h>
#endif
#ifdef G_OS_UNIX
#include <signal.h>
#endif

#include "ImageRenderer.h"

namespace prototype {
namespace {

std::string hexColor(const std::array<double, 3>& color) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(6);
    for (const double channel: color) {
        const auto value = static_cast<unsigned int>(std::lround(std::clamp(channel, 0.0, 1.0) * 255.0));
        result.push_back(digits[(value >> 4U) & 0xfU]);
        result.push_back(digits[value & 0xfU]);
    }
    return result;
}

std::string trimAscii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string instantiateTemplate(const std::string& input, const std::string& sourceTemplate,
                               const std::array<double, 3>& textColor) {
    std::map<std::string, std::string> variables;
    std::vector<std::string> bodyLines;
    const std::regex directive(R"(^%xpp:([A-Za-z_][A-Za-z0-9_]*)=(.*)$)");
    std::istringstream inputStream(input);
    std::string line;
    while (std::getline(inputStream, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, directive)) {
            bodyLines.push_back(line);
            continue;
        }

        std::string key = match[1].str();
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        if (key != "TOOL_INPUT" && key != "TEXT_COLOR") {
            variables[key] = trimAscii(match[2].str());
        }
    }

    std::string body;
    for (const auto& bodyLine: bodyLines) {
        if (!body.empty()) {
            body.push_back('\n');
        }
        body += bodyLine;
    }
    variables["TOOL_INPUT"] = std::move(body);
    variables["TEXT_COLOR"] = hexColor(textColor);

    const std::regex substitution(R"(%%XPP_[A-Z][A-Z0-9_]*%%)");
    std::string output;
    output.reserve(sourceTemplate.size());
    std::size_t templatePosition = 0;
    for (std::sregex_iterator it(sourceTemplate.begin(), sourceTemplate.end(), substitution);
         it != std::sregex_iterator{}; ++it) {
        const auto& match = *it;
        const auto position = static_cast<std::size_t>(match.position());
        output.append(sourceTemplate, templatePosition, position - templatePosition);
        const auto placeholder = match.str();
        constexpr std::size_t prefixLength = 6;
        constexpr std::size_t suffixLength = 2;
        const auto key = placeholder.substr(prefixLength, placeholder.length() - prefixLength - suffixLength);
        if (const auto variable = variables.find(key); variable != variables.end()) {
            output += variable->second;
        }
        templatePosition = position + static_cast<std::size_t>(match.length());
    }
    output.append(sourceTemplate, templatePosition);
    return output;
}

std::string readTemplate(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error || !std::filesystem::is_regular_file(status)) {
        throw std::runtime_error("The configured LaTeX template is not a regular file: " + path.string());
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 8 * 1024 * 1024) {
        throw std::runtime_error("The configured LaTeX template exceeds the 8 MiB limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open the configured LaTeX template: " + path.string());
    }
    std::string contents(std::istreambuf_iterator<char>(stream), {});
    if (!stream.good() && !stream.eof()) {
        throw std::runtime_error("Cannot read the configured LaTeX template: " + path.string());
    }
    return contents;
}

struct StrvDeleter {
    void operator()(gchar** value) const { g_strfreev(value); }
};

std::filesystem::path createTempDirectory() {
    GError* error = nullptr;
    gchar* directory = g_dir_make_tmp("xournalpp-gtk4-latex-XXXXXX", &error);
    if (!directory) {
        const std::string detail = error ? error->message : "Cannot create a temporary directory";
        g_clear_error(&error);
        throw std::runtime_error(detail);
    }
    std::filesystem::path result(directory);
    g_free(directory);
    return result;
}

GSubprocess* spawnGenerator(const std::filesystem::path& tempDirectory, const std::string& command,
                            const std::string& texContents) {
    const auto texPath = tempDirectory / "tex.tex";
    GError* error = nullptr;
    if (!g_file_set_contents(texPath.c_str(), texContents.data(), static_cast<gssize>(texContents.size()), &error)) {
        const std::string detail = error ? error->message : "Cannot write the temporary TeX source";
        g_clear_error(&error);
        throw std::runtime_error(detail);
    }

    std::string expandedCommand = command;
    const auto texFilename = texPath.string();
    for (auto position = expandedCommand.find("{}"); position != std::string::npos;
         position = expandedCommand.find("{}", position + texFilename.size())) {
        expandedCommand.replace(position, 2, texFilename);
    }

    gint argc = 0;
    gchar** argvRaw = nullptr;
    if (!g_shell_parse_argv(expandedCommand.c_str(), &argc, &argvRaw, &error)) {
        const std::string detail = error ? error->message : "Invalid LaTeX generator command";
        g_clear_error(&error);
        throw std::runtime_error("Failed to parse LaTeX generator command: " + detail);
    }
    std::unique_ptr<gchar*, StrvDeleter> argv(argvRaw);
    if (argc < 1 || !argv.get()[0] || !argv.get()[0][0]) {
        throw std::runtime_error("The LaTeX generator command is empty");
    }
    gchar* executable = g_find_program_in_path(argv.get()[0]);
    if (!executable) {
        throw std::runtime_error("Cannot find LaTeX generator in PATH: " + std::string(argv.get()[0]));
    }
    g_free(argv.get()[0]);
    argv.get()[0] = executable;

    constexpr auto flags = static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                        G_SUBPROCESS_FLAGS_STDERR_MERGE);
    GSubprocessLauncher* launcher = g_subprocess_launcher_new(flags);
    g_subprocess_launcher_set_cwd(launcher, tempDirectory.c_str());
    GSubprocess* process = g_subprocess_launcher_spawnv(launcher, argv.get(), &error);
    g_object_unref(launcher);
    if (!process) {
        const std::string detail = error ? error->message : "Cannot start the LaTeX generator";
        g_clear_error(&error);
        throw std::runtime_error(detail);
    }
    return process;
}

std::string quotedCssString(std::string_view value) {
    std::string result{"\""};
    for (const char character: value) {
        if (character == '\\' || character == '"') {
            result.push_back('\\');
        }
        if (character == '\n' || character == '\r') {
            continue;
        }
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

struct SetupTestContext {
    std::filesystem::path tempDirectory;
    LatexEditor::SetupTestCompletion completion;
};

}  // namespace

std::shared_ptr<LatexEditor> LatexEditor::create(GtkWindow* parent, std::filesystem::path templatePath,
                                                 std::string generatorCommand, std::string initialSource,
                                                 std::array<double, 3> textColor,
                                                 std::optional<CanvasImage> initialImage, bool selectInitialSource,
                                                 LatexEditorPreferences preferences, Completion completion) {
    auto editor = std::shared_ptr<LatexEditor>(new LatexEditor(parent, std::move(templatePath),
                                                               std::move(generatorCommand),
                                                               std::move(initialSource), textColor,
                                                               std::move(initialImage), selectInitialSource,
                                                               std::move(preferences),
                                                               std::move(completion)));
    editor->buildWindow();
    return editor;
}

LatexEditor::LatexEditor(GtkWindow* parent, std::filesystem::path templatePath, std::string generatorCommand,
                         std::string initialSource, std::array<double, 3> textColor,
                         std::optional<CanvasImage> initialImage, bool selectInitialSource,
                         LatexEditorPreferences preferences, Completion completion):
        parent(parent), templatePath(std::move(templatePath)), generatorCommand(std::move(generatorCommand)),
        initialSource(std::move(initialSource)), textColor(textColor), preferences(std::move(preferences)),
        currentImage(std::move(initialImage)), completion(std::move(completion)),
        selectInitialSource(selectInitialSource) {}

void LatexEditor::testConfiguration(std::filesystem::path templatePath, std::string generatorCommand,
                                    std::string source, std::array<double, 3> textColor,
                                    SetupTestCompletion completion) {
    std::filesystem::path tempDirectory;
    try {
        if (source.size() > maxCanvasLatexSourceBytes) {
            throw std::runtime_error("LaTeX source exceeds the 1 MiB limit");
        }
        const auto templateContents = readTemplate(templatePath);
        tempDirectory = createTempDirectory();
        const auto texContents = instantiateTemplate(source, templateContents, textColor);
        auto* process = spawnGenerator(tempDirectory, generatorCommand, texContents);
        auto* context = new SetupTestContext{std::move(tempDirectory), std::move(completion)};
        g_subprocess_communicate_utf8_async(process, nullptr, nullptr, setupTestComplete, context);
        g_object_unref(process);
    } catch (const std::exception& error) {
        if (!tempDirectory.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(tempDirectory, ignored);
        }
        completion(false, error.what());
    }
}

void LatexEditor::setupTestComplete(GObject* sourceObject, GAsyncResult* result, gpointer data) {
    std::unique_ptr<SetupTestContext> context(static_cast<SetupTestContext*>(data));
    auto* process = G_SUBPROCESS(sourceObject);
    GError* error = nullptr;
    gchar* output = nullptr;
    const gboolean communicated = g_subprocess_communicate_utf8_finish(process, result, &output, nullptr, &error);
    const std::string compilerOutput = output ? output : "";
    g_free(output);

    std::string message;
    bool succeeded = false;
    if (!communicated || error || !g_subprocess_get_successful(process)) {
        message = error ? error->message : "The LaTeX generator returned an error";
        g_clear_error(&error);
        if (!compilerOutput.empty()) {
            message += "\n\n" + compilerOutput.substr(0, 8192);
        }
    } else {
        try {
            const auto pdfPath = context->tempDirectory / "tex.pdf";
            std::error_code fileError;
            const auto length = std::filesystem::file_size(pdfPath, fileError);
            if (fileError || length == 0 || length > maxCanvasImageBytes) {
                throw std::runtime_error("The generated PDF is missing, empty or exceeds the 64 MiB limit");
            }
            std::ifstream stream(pdfPath, std::ios::binary);
            if (!stream) {
                throw std::runtime_error("Cannot read the generated LaTeX PDF");
            }
            std::string bytes(static_cast<std::size_t>(length), '\0');
            if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
                throw std::runtime_error("Cannot read the generated LaTeX PDF");
            }
            double width = 0;
            double height = 0;
            const auto document = decodeCanvasLatexPdf(bytes, width, height);
            if (!document || width <= 0 || height <= 0) {
                throw std::runtime_error("The generated LaTeX PDF has invalid page dimensions");
            }
            message = "The template and TeX generator compiled the sample formula successfully.";
            succeeded = true;
            if (!compilerOutput.empty()) {
                message += "\n\n" + compilerOutput.substr(0, 8192);
            }
        } catch (const std::exception& failure) {
            message = failure.what();
            if (!compilerOutput.empty()) {
                message += "\n\n" + compilerOutput.substr(0, 8192);
            }
        }
    }

    std::error_code ignored;
    std::filesystem::remove_all(context->tempDirectory, ignored);
    context->completion(succeeded, std::move(message));
}

LatexEditor::~LatexEditor() {
    if (cancellable) {
        g_cancellable_cancel(cancellable);
    }
    if (process) {
        g_subprocess_force_exit(process);
        g_clear_object(&process);
    }
    g_clear_object(&cancellable);
    if (externalEditorCancellable) {
        g_cancellable_cancel(externalEditorCancellable);
    }
    if (externalEditorProcess) {
#ifdef G_OS_UNIX
        g_subprocess_send_signal(externalEditorProcess, SIGTERM);
#else
        g_subprocess_force_exit(externalEditorProcess);
#endif
        g_clear_object(&externalEditorProcess);
    }
    g_clear_object(&externalEditorCancellable);
    g_clear_object(&sourceCssProvider);
    if (!tempDirectory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(tempDirectory, ignored);
    }
}

void LatexEditor::buildWindow() {
    window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(window, "Edit LaTeX");
    gtk_window_set_default_size(window, 900, 640);
    gtk_window_set_modal(window, true);
    gtk_window_set_transient_for(window, parent);
    gtk_window_set_destroy_with_parent(window, true);

    auto* header = ADW_HEADER_BAR(adw_header_bar_new());
    auto* heading = gtk_label_new("Edit LaTeX");
    gtk_widget_add_css_class(heading, "title");
    adw_header_bar_set_title_widget(header, heading);
    gtk_window_set_titlebar(window, GTK_WIDGET(header));

    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    auto* panes = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_wide_handle(GTK_PANED(panes), true);
    gtk_widget_set_vexpand(panes, true);
    gtk_widget_set_margin_start(panes, 16);
    gtk_widget_set_margin_end(panes, 16);
    gtk_widget_set_margin_top(panes, 14);
    gtk_widget_set_margin_bottom(panes, 12);

    auto* sourceColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(sourceColumn), gtk_label_new(preferences.useExternalEditor ? "External editor" : "Formula"));
    sourceScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sourceScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sourceScroll, 300, 300);
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
    sourceView = GTK_TEXT_VIEW(gtk_source_view_new());
#else
    sourceView = GTK_TEXT_VIEW(gtk_text_view_new());
#endif
    sourceBuffer = gtk_text_view_get_buffer(sourceView);
    gtk_text_view_set_monospace(sourceView, !preferences.useCustomEditorFont);
    gtk_text_view_set_wrap_mode(sourceView, preferences.editorWordWrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(sourceView, 12);
    gtk_text_view_set_right_margin(sourceView, 12);
    gtk_text_view_set_top_margin(sourceView, 10);
    gtk_text_view_set_bottom_margin(sourceView, 10);
    gtk_accessible_update_property(GTK_ACCESSIBLE(sourceView), GTK_ACCESSIBLE_PROPERTY_LABEL, "LaTeX source", -1);
#ifdef GTK4_CANVAS_HAVE_SOURCEVIEW
    auto* sourceManager = gtk_source_language_manager_get_default();
    if (preferences.sourceViewSyntaxHighlight) {
        auto* language = gtk_source_language_manager_get_language(sourceManager, "latex");
        gtk_source_buffer_set_language(GTK_SOURCE_BUFFER(sourceBuffer), language);
    }
    auto* schemeManager = gtk_source_style_scheme_manager_get_default();
    if (!preferences.sourceViewThemeId.empty()) {
        auto* scheme = gtk_source_style_scheme_manager_get_scheme(schemeManager,
                                                                  preferences.sourceViewThemeId.c_str());
        if (scheme) {
            gtk_source_buffer_set_style_scheme(GTK_SOURCE_BUFFER(sourceBuffer), scheme);
        }
    }
    gtk_source_view_set_auto_indent(GTK_SOURCE_VIEW(sourceView), preferences.sourceViewAutoIndent);
    gtk_source_view_set_indent_on_tab(GTK_SOURCE_VIEW(sourceView), preferences.sourceViewAutoIndent);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(sourceView), preferences.sourceViewShowLineNumbers);
#endif
    if (preferences.useCustomEditorFont) {
        PangoFontDescription* description = pango_font_description_from_string(preferences.editorFont.c_str());
        const char* family = pango_font_description_get_family(description);
        const auto size = pango_font_description_get_size(description);
        if (family && size > 0) {
            const auto sizeUnit = pango_font_description_get_size_is_absolute(description) ? "px" : "pt";
            const auto sizeValue = static_cast<double>(size) / PANGO_SCALE;
            std::ostringstream css;
            css << ".latex-source-view { font-family: " << quotedCssString(family) << "; font-size: "
                << sizeValue << sizeUnit << "; font-weight: "
                << pango_font_description_get_weight(description) << "; font-style: "
                << (pango_font_description_get_style(description) == PANGO_STYLE_ITALIC ? "italic" : "normal")
                << "; }";
            sourceCssProvider = gtk_css_provider_new();
            gtk_css_provider_load_from_data(sourceCssProvider, css.str().c_str(), -1);
            gtk_widget_add_css_class(GTK_WIDGET(sourceView), "latex-source-view");
            gtk_style_context_add_provider(gtk_widget_get_style_context(GTK_WIDGET(sourceView)),
                                           GTK_STYLE_PROVIDER(sourceCssProvider),
                                           GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        pango_font_description_free(description);
    }
    gtk_text_buffer_set_text(sourceBuffer, initialSource.c_str(), static_cast<int>(initialSource.size()));
    if (selectInitialSource) {
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_bounds(sourceBuffer, &start, &end);
        gtk_text_buffer_select_range(sourceBuffer, &start, &end);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sourceScroll), GTK_WIDGET(sourceView));
    if (preferences.useExternalEditor) {
        gtk_widget_set_visible(sourceScroll, false);
        externalEditorPanel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_vexpand(externalEditorPanel, true);
        gtk_widget_set_valign(externalEditorPanel, GTK_ALIGN_CENTER);
        auto* externalEditorMessage = gtk_label_new(
                "The formula source opens in your external editor. The preview updates when that editor closes.");
        gtk_label_set_wrap(GTK_LABEL(externalEditorMessage), true);
        gtk_label_set_justify(GTK_LABEL(externalEditorMessage), GTK_JUSTIFY_CENTER);
        gtk_widget_set_halign(externalEditorMessage, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(externalEditorPanel), externalEditorMessage);
        externalEditorStatus = GTK_LABEL(gtk_label_new("Ready to open the source"));
        gtk_widget_add_css_class(GTK_WIDGET(externalEditorStatus), "dim-label");
        gtk_box_append(GTK_BOX(externalEditorPanel), GTK_WIDGET(externalEditorStatus));
        externalEditorButton = gtk_button_new_with_label("Open external editor");
        gtk_widget_add_css_class(externalEditorButton, "pill");
        gtk_widget_set_halign(externalEditorButton, GTK_ALIGN_CENTER);
        gtk_accessible_update_property(GTK_ACCESSIBLE(externalEditorButton), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Open the LaTeX source in the external editor", -1);
        g_signal_connect(externalEditorButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                             static_cast<LatexEditor*>(data)->launchExternalEditor();
                         }),
                         this);
        gtk_box_append(GTK_BOX(externalEditorPanel), externalEditorButton);
        gtk_box_append(GTK_BOX(sourceColumn), externalEditorPanel);
    } else {
        gtk_box_append(GTK_BOX(sourceColumn), sourceScroll);
    }
    gtk_paned_set_start_child(GTK_PANED(panes), sourceColumn);

    auto* previewColumn = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    auto* previewHeading = gtk_label_new("Live preview");
    gtk_widget_set_halign(previewHeading, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(previewColumn), previewHeading);
    previewArea = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_content_width(previewArea, 340);
    gtk_drawing_area_set_content_height(previewArea, 300);
    gtk_widget_set_hexpand(GTK_WIDGET(previewArea), true);
    gtk_widget_set_vexpand(GTK_WIDGET(previewArea), true);
    gtk_widget_add_css_class(GTK_WIDGET(previewArea), "card");
    gtk_drawing_area_set_draw_func(previewArea, drawPreview, this, nullptr);
    gtk_box_append(GTK_BOX(previewColumn), GTK_WIDGET(previewArea));
    gtk_paned_set_end_child(GTK_PANED(panes), previewColumn);
    gtk_paned_set_position(GTK_PANED(panes), 440);
    gtk_box_append(GTK_BOX(root), panes);

    auto* lower = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(lower, 16);
    gtk_widget_set_margin_end(lower, 16);
    gtk_widget_set_margin_bottom(lower, 14);
    auto* outputExpander = gtk_expander_new("Compiler output");
    auto* outputScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(outputScroll), 72);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(outputScroll), 120);
    outputBuffer = gtk_text_buffer_new(nullptr);
    auto* outputView = gtk_text_view_new_with_buffer(outputBuffer);
    g_object_unref(outputBuffer);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(outputView), false);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(outputView), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(outputView), true);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(outputView), GTK_WRAP_WORD_CHAR);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(outputScroll), outputView);
    gtk_expander_set_child(GTK_EXPANDER(outputExpander), outputScroll);
    gtk_box_append(GTK_BOX(lower), outputExpander);

    auto* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    statusLabel = GTK_LABEL(gtk_label_new("Ready to compile"));
    gtk_label_set_xalign(statusLabel, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(statusLabel), true);
    gtk_widget_add_css_class(GTK_WIDGET(statusLabel), "dim-label");
    gtk_box_append(GTK_BOX(footer), GTK_WIDGET(statusLabel));
    auto* cancelButton = gtk_button_new_with_label("Cancel");
    gtk_accessible_update_property(GTK_ACCESSIBLE(cancelButton), GTK_ACCESSIBLE_PROPERTY_LABEL, "Cancel LaTeX edit",
                                   -1);
    g_signal_connect(cancelButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<LatexEditor*>(data)->cancel();
                     }),
                     this);
    gtk_box_append(GTK_BOX(footer), cancelButton);
    insertButton = gtk_button_new_with_label("Insert");
    gtk_widget_add_css_class(insertButton, "suggested-action");
    gtk_widget_set_sensitive(insertButton, currentImage.has_value());
    g_signal_connect(insertButton, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
                         static_cast<LatexEditor*>(data)->accept();
                     }),
                     this);
    gtk_box_append(GTK_BOX(footer), insertButton);
    gtk_box_append(GTK_BOX(lower), footer);
    gtk_box_append(GTK_BOX(root), lower);
    gtk_window_set_child(window, root);

    g_signal_connect(sourceBuffer, "changed", G_CALLBACK(+[](GtkTextBuffer*, gpointer data) {
                         static_cast<LatexEditor*>(data)->compileCurrentSource();
                     }),
                     this);
    g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
                         static_cast<LatexEditor*>(data)->cancel();
                         return TRUE;
                     }),
                     this);
}

void LatexEditor::present() {
    if (closed || !window) {
        return;
    }
    if (currentImage) {
        gtk_widget_queue_draw(GTK_WIDGET(previewArea));
        setStatus("Existing formula is ready to insert", true);
    } else {
        compileCurrentSource();
    }
    gtk_window_present(window);
    if (preferences.useExternalEditor) {
        gtk_widget_grab_focus(externalEditorButton);
        launchExternalEditor();
    } else {
        gtk_widget_grab_focus(GTK_WIDGET(sourceView));
    }
}

bool LatexEditor::readyForInsertion() const {
    return !closed && !compileRunning && currentImage && insertButton && gtk_widget_get_sensitive(insertButton);
}

void LatexEditor::activateInsert() {
    if (readyForInsertion()) {
        g_signal_emit_by_name(insertButton, "clicked");
    }
}

void LatexEditor::ensureTempDirectory() {
    if (tempDirectory.empty()) {
        tempDirectory = createTempDirectory();
    }
}

void LatexEditor::launchExternalEditor() {
    if (closed || !preferences.useExternalEditor || externalEditorOpen) {
        return;
    }

    const auto& extension = preferences.temporaryFileExtension;
    if (extension.size() > 64 || extension.find('/') != std::string::npos ||
        extension.find('\\') != std::string::npos ||
        std::any_of(extension.begin(), extension.end(), [](unsigned char value) { return value < 0x20; })) {
        const std::string message = "The temporary LaTeX file extension must be a filename suffix";
        gtk_text_buffer_set_text(outputBuffer, message.c_str(), -1);
        if (externalEditorStatus) {
            gtk_label_set_text(externalEditorStatus, message.c_str());
        }
        setStatus(message);
        return;
    }

    try {
        ensureTempDirectory();
        externalEditorFile = tempDirectory / ("edit." + extension);
        const auto source = sourceText();
        if (source.size() > maxCanvasLatexSourceBytes) {
            throw std::runtime_error("LaTeX source exceeds the 1 MiB limit");
        }
        GError* error = nullptr;
        if (!g_file_set_contents(externalEditorFile.c_str(), source.data(), static_cast<gssize>(source.size()), &error)) {
            const std::string detail = error ? error->message : "Cannot write the temporary LaTeX source";
            g_clear_error(&error);
            throw std::runtime_error(detail);
        }

        std::string command = preferences.externalEditorCommand;
        if (command.empty()) {
            const char* editor = g_getenv("EDITOR");
            if (!editor || !*editor) {
                throw std::runtime_error(
                        "No external editor is configured and $EDITOR is unset. Set an editor command in LaTeX "
                        "preferences or turn off external editing.");
            }
            command = editor;
        }
        gchar* quotedPath = g_shell_quote(externalEditorFile.string().c_str());
        command.push_back(' ');
        command += quotedPath;
        g_free(quotedPath);

        gint argc = 0;
        gchar** argvRaw = nullptr;
        if (!g_shell_parse_argv(command.c_str(), &argc, &argvRaw, &error)) {
            const std::string detail = error ? error->message : "Invalid external editor command";
            g_clear_error(&error);
            throw std::runtime_error("Failed to parse external editor command: " + detail);
        }
        std::unique_ptr<gchar*, StrvDeleter> argv(argvRaw);
        if (argc < 1 || !argv.get()[0] || !argv.get()[0][0]) {
            throw std::runtime_error("The external editor command is empty");
        }

        GSubprocessLauncher* launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
        const auto color = hexColor(textColor);
        g_subprocess_launcher_setenv(launcher, "XPP_TEXT_COLOR", color.c_str(), true);
        externalEditorProcess = g_subprocess_launcher_spawnv(launcher, argv.get(), &error);
        g_object_unref(launcher);
        if (!externalEditorProcess) {
            const std::string detail = error ? error->message : "Cannot start the external editor";
            g_clear_error(&error);
            throw std::runtime_error("Could not start external editor: " + detail);
        }

        g_clear_object(&externalEditorCancellable);
        externalEditorCancellable = g_cancellable_new();
        externalEditorOpen = true;
        externalEditorCompleted = false;
        gtk_widget_set_sensitive(externalEditorButton, false);
        if (externalEditorStatus) {
            gtk_label_set_text(externalEditorStatus, "Waiting for the external editor to close…");
        }
        setStatus("Waiting for external editor…");
        auto* context = new std::weak_ptr<LatexEditor>(shared_from_this());
        g_subprocess_wait_async(externalEditorProcess, externalEditorCancellable, externalEditorComplete, context);
    } catch (const std::exception& error) {
        const std::string message = error.what();
        gtk_text_buffer_set_text(outputBuffer, message.c_str(), -1);
        if (externalEditorStatus) {
            gtk_label_set_text(externalEditorStatus, message.c_str());
        }
        gtk_widget_set_sensitive(externalEditorButton, true);
        setStatus(message);
    }
}

void LatexEditor::externalEditorComplete(GObject* sourceObject, GAsyncResult* result, gpointer data) {
    std::unique_ptr<std::weak_ptr<LatexEditor>> context(static_cast<std::weak_ptr<LatexEditor>*>(data));
    if (auto editor = context->lock()) {
        editor->finishExternalEditor(G_SUBPROCESS(sourceObject), result);
    }
}

void LatexEditor::finishExternalEditor(GSubprocess* completedProcess, GAsyncResult* result) {
    GError* error = nullptr;
    const gboolean waited = g_subprocess_wait_finish(completedProcess, result, &error);
    g_clear_object(&externalEditorProcess);
    g_clear_object(&externalEditorCancellable);
    externalEditorOpen = false;
    if (closed || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        return;
    }
    if (!waited || !g_subprocess_get_successful(completedProcess)) {
        const std::string detail = error ? error->message : "The external editor exited with an error";
        g_clear_error(&error);
        gtk_text_buffer_set_text(outputBuffer, detail.c_str(), -1);
        if (externalEditorStatus) {
            gtk_label_set_text(externalEditorStatus, detail.c_str());
        }
        gtk_widget_set_sensitive(externalEditorButton, true);
        setStatus(detail);
        return;
    }

    try {
        std::error_code fileError;
        const auto status = std::filesystem::symlink_status(externalEditorFile, fileError);
        if (fileError || !std::filesystem::is_regular_file(status)) {
            throw std::runtime_error("The external editor did not leave a regular source file");
        }
        const auto length = std::filesystem::file_size(externalEditorFile, fileError);
        if (fileError || length > maxCanvasLatexSourceBytes) {
            throw std::runtime_error("LaTeX source exceeds the 1 MiB limit");
        }
        std::ifstream stream(externalEditorFile, std::ios::binary);
        if (!stream) {
            throw std::runtime_error("Cannot read the source returned by the external editor");
        }
        std::string source(static_cast<std::size_t>(length), '\0');
        if (!stream.read(source.data(), static_cast<std::streamsize>(source.size()))) {
            throw std::runtime_error("Cannot read the source returned by the external editor");
        }
        if (!g_utf8_validate(source.data(), static_cast<gssize>(source.size()), nullptr)) {
            throw std::runtime_error("The external editor returned text that is not valid UTF-8");
        }
        if (!source.empty() && source.back() == '\n') {
            source.pop_back();
        }

        const auto previousSource = sourceText();
        externalEditorCompleted = true;
        if (externalEditorStatus) {
            gtk_label_set_text(externalEditorStatus, "Source loaded from the external editor");
        }
        gtk_widget_set_sensitive(externalEditorButton, true);
        gtk_text_buffer_set_text(sourceBuffer, source.c_str(), static_cast<int>(source.size()));
        if (source == previousSource) {
            compileCurrentSource();
        }
    } catch (const std::exception& failure) {
        const std::string detail = failure.what();
        gtk_text_buffer_set_text(outputBuffer, detail.c_str(), -1);
        if (externalEditorStatus) {
            gtk_label_set_text(externalEditorStatus, detail.c_str());
        }
        gtk_widget_set_sensitive(externalEditorButton, true);
        setStatus(detail);
    }
}

void LatexEditor::compileCurrentSource() {
    if (closed) {
        return;
    }
    ++requestedRevision;
    const auto source = sourceText();
    if (source.size() > maxCanvasLatexSourceBytes) {
        setStatus("LaTeX source exceeds the 1 MiB limit");
        return;
    }
    setStatus("Compiling…");
    if (compileRunning) {
        return;
    }
    startCompile(source, requestedRevision);
}

void LatexEditor::startCompile(std::string source, std::uint64_t revision) {
    if (closed || compileRunning) {
        return;
    }
    if (source.size() > maxCanvasLatexSourceBytes) {
        setStatus("LaTeX source exceeds the 1 MiB limit");
        return;
    }
    try {
        if (templateText.empty()) {
            templateText = readTemplate(templatePath);
        }
        ensureTempDirectory();
        const auto texContents = instantiateTemplate(source, templateText, textColor);
        process = spawnGenerator(tempDirectory, generatorCommand, texContents);
        g_clear_object(&cancellable);
        cancellable = g_cancellable_new();
        compileRunning = true;
        const auto weak = std::weak_ptr<LatexEditor>(shared_from_this());
        auto* context = new std::pair<std::weak_ptr<LatexEditor>, std::pair<std::uint64_t, std::string>>(
                weak, {revision, std::move(source)});
        g_subprocess_communicate_utf8_async(process, nullptr, cancellable, compileComplete, context);
    } catch (const std::exception& error) {
        compileRunning = false;
        setStatus(error.what());
        gtk_text_buffer_set_text(outputBuffer, error.what(), -1);
    }
}

void LatexEditor::compileComplete(GObject* sourceObject, GAsyncResult* result, gpointer data) {
    using CompileContext = std::pair<std::weak_ptr<LatexEditor>, std::pair<std::uint64_t, std::string>>;
    std::unique_ptr<CompileContext> context(static_cast<CompileContext*>(data));
    auto editor = context->first.lock();
    if (!editor) {
        return;
    }
    editor->finishCompile(G_SUBPROCESS(sourceObject), result, context->second.first, context->second.second);
}

void LatexEditor::finishCompile(GSubprocess* completedProcess, GAsyncResult* result, std::uint64_t revision,
                               const std::string& source) {
    GError* error = nullptr;
    gchar* output = nullptr;
    const gboolean communicated = g_subprocess_communicate_utf8_finish(completedProcess, result, &output, nullptr,
                                                                        &error);
    std::string compilerOutput = output ? output : "";
    g_free(output);
    const bool successful = communicated && !error && g_subprocess_get_successful(completedProcess);
    g_clear_object(&process);
    g_clear_object(&cancellable);
    compileRunning = false;

    if (closed || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        return;
    }
    if (!successful) {
        const std::string detail = error ? error->message : "The LaTeX generator returned an error";
        gtk_text_buffer_set_text(outputBuffer, compilerOutput.c_str(), -1);
        g_clear_error(&error);
        if (revision != requestedRevision) {
            startCompile(sourceText(), requestedRevision);
        } else {
            setStatus(detail);
        }
        return;
    }
    if (revision != requestedRevision) {
        startCompile(sourceText(), requestedRevision);
        return;
    }

    try {
        const auto pdfPath = tempDirectory / "tex.pdf";
        gchar* contents = nullptr;
        gsize length = 0;
        if (!g_file_get_contents(pdfPath.c_str(), &contents, &length, &error)) {
            const std::string detail = error ? error->message : "LaTeX did not create tex.pdf";
            g_clear_error(&error);
            throw std::runtime_error(detail);
        }
        CanvasImage image;
        image.kind = CanvasImageKind::Latex;
        image.encodedData.assign(contents, length);
        g_free(contents);
        image.latexSource = source;
        double width = 0;
        double height = 0;
        image.pdfDocument = decodeCanvasLatexPdf(image.encodedData, width, height);
        image.nativeWidth = width;
        image.nativeHeight = height;
        image.pixelWidth = std::max(1, static_cast<int>(std::ceil(width)));
        image.pixelHeight = std::max(1, static_cast<int>(std::ceil(height)));
        image.transform = {1, 0, 0, 1, 0, 0};
        if (currentImage) {
            image.page = currentImage->page;
            image.layerId = currentImage->layerId;
            image.layerOrder = currentImage->layerOrder;
            image.originLayer = currentImage->originLayer;
            image.transform = currentImage->transform;
        }
        if (!validCanvasImage(image)) {
            throw std::runtime_error("The generated LaTeX image has invalid dimensions");
        }
        currentImage = std::move(image);
        gtk_text_buffer_set_text(outputBuffer, compilerOutput.c_str(), -1);
        gtk_widget_queue_draw(GTK_WIDGET(previewArea));
        setStatus("Formula compiled", true);
        if (preferences.useExternalEditor && preferences.externalEditorAutoConfirm && externalEditorCompleted) {
            accept();
        }
    } catch (const std::exception& failure) {
        gtk_text_buffer_set_text(outputBuffer, compilerOutput.c_str(), -1);
        setStatus(failure.what());
    }
}

void LatexEditor::setStatus(std::string message, bool valid) {
    if (!statusLabel || !insertButton) {
        return;
    }
    gtk_label_set_text(statusLabel, message.c_str());
    gtk_widget_set_sensitive(insertButton, valid && currentImage.has_value());
    if (valid) {
        gtk_widget_add_css_class(GTK_WIDGET(statusLabel), "success");
        gtk_widget_remove_css_class(GTK_WIDGET(statusLabel), "error");
    } else if (message == "Compiling…") {
        gtk_widget_remove_css_class(GTK_WIDGET(statusLabel), "success");
        gtk_widget_remove_css_class(GTK_WIDGET(statusLabel), "error");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(statusLabel), "success");
        gtk_widget_add_css_class(GTK_WIDGET(statusLabel), "error");
    }
}

std::string LatexEditor::sourceText() const {
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(sourceBuffer, &start, &end);
    gchar* text = gtk_text_buffer_get_text(sourceBuffer, &start, &end, false);
    std::string result = text ? text : "";
    g_free(text);
    return result;
}

void LatexEditor::accept() {
    if (closed || !currentImage || !gtk_widget_get_sensitive(insertButton)) {
        return;
    }
    auto keepAlive = shared_from_this();
    currentImage->latexSource = sourceText();
    currentImage->kind = CanvasImageKind::Latex;
    if (completion) {
        completion(currentImage);
    }
    closeWindow();
}

void LatexEditor::cancel() {
    if (closed) {
        return;
    }
    auto keepAlive = shared_from_this();
    closeWindow();
    if (completion) {
        completion(std::nullopt);
    }
}

void LatexEditor::closeWindow() {
    if (closed) {
        return;
    }
    closed = true;
    if (cancellable) {
        g_cancellable_cancel(cancellable);
    }
    if (process) {
        g_subprocess_force_exit(process);
        g_clear_object(&process);
    }
    g_clear_object(&cancellable);
    if (externalEditorCancellable) {
        g_cancellable_cancel(externalEditorCancellable);
    }
    if (externalEditorProcess) {
#ifdef G_OS_UNIX
        g_subprocess_send_signal(externalEditorProcess, SIGTERM);
#else
        g_subprocess_force_exit(externalEditorProcess);
#endif
        g_clear_object(&externalEditorProcess);
    }
    g_clear_object(&externalEditorCancellable);
    if (window) {
        auto* closingWindow = window;
        window = nullptr;
        gtk_window_destroy(closingWindow);
    }
}

void LatexEditor::drawPreview(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    auto* self = static_cast<LatexEditor*>(data);
    const double luminance = 0.2126 * self->textColor[0] + 0.7152 * self->textColor[1] + 0.0722 * self->textColor[2];
    const bool lightBackground = luminance < 0.5;
    cairo_set_source_rgb(cr, lightBackground ? 1.0 : 0.12, lightBackground ? 1.0 : 0.12,
                         lightBackground ? 1.0 : 0.12);
    cairo_paint(cr);
    if (!self->currentImage) {
        return;
    }
    const auto& image = *self->currentImage;
    if (image.pdfDocument) {
        PopplerPage* page = poppler_document_get_page(image.pdfDocument.get(), 0);
        if (!page) {
            return;
        }
        double pageWidth = 0;
        double pageHeight = 0;
        poppler_page_get_size(page, &pageWidth, &pageHeight);
        const double scale = std::min(std::max(1.0, width - 24.0) / pageWidth,
                                      std::max(1.0, height - 24.0) / pageHeight);
        cairo_save(cr);
        cairo_translate(cr, (width - pageWidth * scale) / 2.0, (height - pageHeight * scale) / 2.0);
        cairo_scale(cr, scale, scale);
        poppler_page_render(page, cr);
        cairo_restore(cr);
        g_object_unref(page);
    } else if (image.surface) {
        const double scale = std::min(std::max(1.0, width - 24.0) / image.nativeWidth,
                                      std::max(1.0, height - 24.0) / image.nativeHeight);
        cairo_save(cr);
        cairo_translate(cr, (width - image.nativeWidth * scale) / 2.0,
                        (height - image.nativeHeight * scale) / 2.0);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, image.surface.get(), 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    }
}

}  // namespace prototype
