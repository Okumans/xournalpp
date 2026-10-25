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
#include <vector>

#include <adwaita.h>
#include <gio/gio.h>
#include <poppler.h>

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

}  // namespace

std::shared_ptr<LatexEditor> LatexEditor::create(GtkWindow* parent, std::filesystem::path templatePath,
                                                 std::string generatorCommand, std::string initialSource,
                                                 std::array<double, 3> textColor,
                                                 std::optional<CanvasImage> initialImage, bool selectInitialSource,
                                                 Completion completion) {
    auto editor = std::shared_ptr<LatexEditor>(new LatexEditor(parent, std::move(templatePath),
                                                               std::move(generatorCommand),
                                                               std::move(initialSource), textColor,
                                                               std::move(initialImage), selectInitialSource,
                                                               std::move(completion)));
    editor->buildWindow();
    return editor;
}

LatexEditor::LatexEditor(GtkWindow* parent, std::filesystem::path templatePath, std::string generatorCommand,
                         std::string initialSource, std::array<double, 3> textColor,
                         std::optional<CanvasImage> initialImage, bool selectInitialSource, Completion completion):
        parent(parent), templatePath(std::move(templatePath)), generatorCommand(std::move(generatorCommand)),
        initialSource(std::move(initialSource)), textColor(textColor), currentImage(std::move(initialImage)),
        completion(std::move(completion)), selectInitialSource(selectInitialSource) {}

LatexEditor::~LatexEditor() {
    if (cancellable) {
        g_cancellable_cancel(cancellable);
    }
    if (process) {
        g_subprocess_force_exit(process);
        g_clear_object(&process);
    }
    g_clear_object(&cancellable);
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
    gtk_box_append(GTK_BOX(sourceColumn), gtk_label_new("Formula"));
    auto* sourceScroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sourceScroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(sourceScroll, 300, 300);
    sourceView = GTK_TEXT_VIEW(gtk_text_view_new());
    sourceBuffer = gtk_text_view_get_buffer(sourceView);
    gtk_text_view_set_monospace(sourceView, true);
    gtk_text_view_set_wrap_mode(sourceView, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(sourceView, 12);
    gtk_text_view_set_right_margin(sourceView, 12);
    gtk_text_view_set_top_margin(sourceView, 10);
    gtk_text_view_set_bottom_margin(sourceView, 10);
    gtk_accessible_update_property(GTK_ACCESSIBLE(sourceView), GTK_ACCESSIBLE_PROPERTY_LABEL, "LaTeX source", -1);
    gtk_text_buffer_set_text(sourceBuffer, initialSource.c_str(), static_cast<int>(initialSource.size()));
    if (selectInitialSource) {
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_bounds(sourceBuffer, &start, &end);
        gtk_text_buffer_select_range(sourceBuffer, &start, &end);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sourceScroll), GTK_WIDGET(sourceView));
    gtk_box_append(GTK_BOX(sourceColumn), sourceScroll);
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
    gtk_widget_grab_focus(GTK_WIDGET(sourceView));
}

bool LatexEditor::readyForInsertion() const {
    return !closed && !compileRunning && currentImage && insertButton && gtk_widget_get_sensitive(insertButton);
}

void LatexEditor::activateInsert() {
    if (readyForInsertion()) {
        g_signal_emit_by_name(insertButton, "clicked");
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
        if (tempDirectory.empty()) {
            GError* error = nullptr;
            gchar* directory = g_dir_make_tmp("xournalpp-gtk4-latex-XXXXXX", &error);
            if (!directory) {
                const std::string detail = error ? error->message : "Cannot create a temporary directory";
                g_clear_error(&error);
                throw std::runtime_error(detail);
            }
            tempDirectory = directory;
            g_free(directory);
        }

        const auto texPath = tempDirectory / "tex.tex";
        const auto texContents = instantiateTemplate(source, templateText, textColor);
        GError* error = nullptr;
        if (!g_file_set_contents(texPath.c_str(), texContents.data(), static_cast<gssize>(texContents.size()), &error)) {
            const std::string detail = error ? error->message : "Cannot write the temporary TeX source";
            g_clear_error(&error);
            throw std::runtime_error(detail);
        }

        std::string command = generatorCommand;
        const auto texFilename = texPath.string();
        for (auto position = command.find("{}"); position != std::string::npos;
             position = command.find("{}", position + texFilename.size())) {
            command.replace(position, 2, texFilename);
        }
        gint argc = 0;
        gchar** argvRaw = nullptr;
        if (!g_shell_parse_argv(command.c_str(), &argc, &argvRaw, &error)) {
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

        g_clear_object(&cancellable);
        cancellable = g_cancellable_new();
        const auto flags = static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                        G_SUBPROCESS_FLAGS_STDERR_MERGE);
        GSubprocessLauncher* launcher = g_subprocess_launcher_new(flags);
        g_subprocess_launcher_set_cwd(launcher, tempDirectory.c_str());
        process = g_subprocess_launcher_spawnv(launcher, argv.get(), &error);
        g_object_unref(launcher);
        if (!process) {
            const std::string detail = error ? error->message : "Cannot start the LaTeX generator";
            g_clear_error(&error);
            throw std::runtime_error(detail);
        }
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
