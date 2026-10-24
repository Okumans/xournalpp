#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include <adwaita.h>
#include <poppler.h>

#include "BehaviorDefaults.h"
#include "CanvasInput.h"
#include "CanvasState.h"
#include "DocumentSession.h"
#include "DocumentJob.h"
#include "DocumentInk.h"
#include "InkRenderer.h"
#include "StylusSamples.h"

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
    std::string smokeDirectory;
    bool dark = false;
    int width = 1100;
    int height = 820;
};

struct SurfaceDeleter {
    void operator()(cairo_surface_t* surface) const { cairo_surface_destroy(surface); }
};

class Prototype {
public:
    explicit Prototype(Options options): options(std::move(options)) {
        sourcePath = this->options.nativeDocument.empty() ? this->options.pdf : this->options.nativeDocument;
        if (!this->options.pdf.empty() && this->options.worker.empty()) {
            GError* error = nullptr;
            auto* uri = g_filename_to_uri(this->options.pdf.c_str(), nullptr, &error);
            if (!uri) {
                fail(error);
            }
            document = poppler_document_new_from_file(uri, nullptr, &error);
            g_free(uri);
            if (!document) {
                fail(error);
            }
            pageCount = poppler_document_get_n_pages(document);
            if (!pageCount) {
                throw std::runtime_error("PDF has no pages");
            }
            setPage(0);
        }
    }

    ~Prototype() {
        if (heartbeatSource) {
            g_source_remove(heartbeatSource);
        }
        if (paintClock) {
            g_signal_handlers_disconnect_by_data(paintClock, this);
            g_clear_object(&paintClock);
        }
        g_clear_object(&windowPaintable);
        if (page) {
            g_object_unref(page);
        }
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

    bool modified() const { return state.revision() != savedRevision; }

    void closeWindow() {
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
        } else if (modified()) {
            status = "Unsaved changes";
        } else if (!savedCopyPath.empty()) {
            status = "Copy saved: " + std::filesystem::path(savedCopyPath).filename().string();
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
            if (!closed) {
                update();
            }
            g_application_release(application);
        });
    }

    void replaceDocument(const std::shared_ptr<prototype::bridge::DocumentSession>& session,
                         const std::string& source) {
        GError* error = nullptr;
        const auto path = session->previewPath().string();
        auto* uri = g_filename_to_uri(path.c_str(), nullptr, &error);
        if (!uri) {
            fail(error);
        }
        auto* candidate = poppler_document_new_from_file(uri, nullptr, &error);
        g_free(uri);
        if (!candidate) {
            fail(error);
        }
        const auto count = poppler_document_get_n_pages(candidate);
        if (!count) {
            g_object_unref(candidate);
            throw std::runtime_error("Document has no pages");
        }
        // Commit the replacement only after the new native snapshot and preview
        // are valid. Failed opens leave the old document and undo history intact.
        input.reset();
        const auto brush = state.brush;
        state = CanvasState{};
        state.brush = brush;
        savedRevision = 0;
        savedCopyPath.clear();
        g_clear_object(&page);
        g_clear_object(&document);
        document = candidate;
        pageCount = count;
        nativeSession = session;
        sourcePath = source;
        cache.reset();
        thumbnails.clear();
        thumbnailWidgets.clear();
        pageList = nullptr;
        adw_overlay_split_view_set_sidebar(splitView, createSidebar());
        fitMode = true;
        setPage(0);
    }

    void openDocument(const std::string& path, bool initial = false) {
        auto loaded = std::make_shared<std::shared_ptr<prototype::bridge::DocumentSession>>();
        const auto worker = options.worker;
        runDocumentJob("Opening document…", [loaded, worker, path] {
            *loaded = std::make_shared<prototype::bridge::DocumentSession>(worker, path);
        }, [this, loaded, path] { replaceDocument(*loaded, path); }, initial);
    }

    void saveCopyTo(const std::string& path, std::function<void()> after = {}) {
        if (!nativeSession || busy) {
            return;
        }
        resetInput(true);
        const auto revision = state.revision();
        const auto packets = prototype::bridge::transferInk(state.ink);
        const auto session = nativeSession;
        const auto worker = options.worker;
        runDocumentJob("Saving copy…", [session, worker, path, packets] { session->saveCopy(worker, path, packets); },
                       [this, revision, path, after = std::move(after)] {
            savedRevision = revision;
            savedCopyPath = path;
            if (after) {
                after();
            }
        });
    }

    void chooseOpen() {
        if (busy || choosingFile || options.worker.empty()) {
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
            self->finishFileChoice(file, error, [self](const std::string& path) { self->openDocument(path); });
        }, this);
        g_object_unref(dialog);
    }

    void chooseSave(std::function<void()> after = {}) {
        if (busy || choosingFile || !nativeSession) {
            return;
        }
        resetInput(true);
        if (!options.smokeDirectory.empty() && !smokeSaveChoice.empty()) {
            const auto path = std::exchange(smokeSaveChoice, {});
            saveCopyTo(path, std::move(after));
            return;
        }
        choosingFile = true;
        struct Choice {
            Prototype* self;
            std::function<void()> after;
        };
        auto* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Save a copy");
        const auto name = sourcePath.empty() ? std::string("Notebook.xopp") :
                                              std::filesystem::path(sourcePath).stem().string() + "-copy.xopp";
        gtk_file_dialog_set_initial_name(dialog, name.c_str());
        gtk_file_dialog_save(dialog, GTK_WINDOW(window), nullptr, +[](GObject* source, GAsyncResult* result,
                                                                      gpointer data) {
            std::unique_ptr<Choice> choice(static_cast<Choice*>(data));
            auto* self = choice->self;
            self->choosingFile = false;
            GError* error = nullptr;
            auto* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
            self->finishFileChoice(file, error, [self, after = std::move(choice->after)](const std::string& path) {
                self->saveCopyTo(path, after);
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
        resetInput(true);
        if (!modified()) {
            after();
            return;
        }
        afterConfirmation = std::move(after);
        messageDialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Save changes?",
                                       "This document has unsaved changes. Save a copy before continuing?"));
        adw_alert_dialog_add_responses(messageDialog, "cancel", "Cancel", "discard", "Discard", "save",
                                         "Save a Copy…", nullptr);
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
                after();
            } else if (std::string(response) == "save") {
                self->chooseSave(std::move(after));
            }
        }), this);
        adw_dialog_present(ADW_DIALOG(messageDialog), window);
    }

    void setPage(int number) {
        if (number < 0 || number >= pageCount) {
            return;
        }
        resetInput();
        state.page = number;
        if (document) {
            if (page) {
                g_object_unref(page);
            }
            page = poppler_document_get_page(document, number);
            poppler_page_get_size(page, &state.pageWidth, &state.pageHeight);
        }
        cache.reset();
        fitted = false;
        if (canvas) {
            update();
        }
    }

    GtkWidget* button(const char* icon, const char* tooltip, GCallback callback) {
        auto* result = gtk_button_new_from_icon_name(icon);
        gtk_widget_set_tooltip_text(result, tooltip);
        g_signal_connect(result, "clicked", callback, this);
        return result;
    }

    void selectTool(Tool tool) {
        if (state.brush.tool == Tool::Pen) {
            penBrush = state.brush;
        } else if (state.brush.tool == Tool::Highlighter) {
            highlightBrush = state.brush;
        }
        input.cancel();
        state.brush = tool == Tool::Highlighter ? highlightBrush : penBrush;
        state.brush.tool = tool;
        syncingBrush = true;
        const auto& color = state.brush.color;
        const GdkRGBA rgba{static_cast<float>(color[0]), static_cast<float>(color[1]), static_cast<float>(color[2]), 1};
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &rgba);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(widthButton), state.brush.width);
        syncingBrush = false;
        for (auto* control: {colorButton, paletteButton, widthButton}) {
            gtk_widget_set_sensitive(control, tool != Tool::Eraser);
        }
        if (canvas) {
            update();
        }
    }

    GtkWidget* createPalette() {
        paletteButton = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(paletteButton), "view-more-symbolic");
        gtk_widget_set_tooltip_text(paletteButton, "Quick colors");
        palettePopover = gtk_popover_new();
        auto* colors = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        const std::array names = {"Black", "Blue", "Red", "Green", "Yellow", "Purple"};
        const std::array values = {"#242424", "#2459c7", "#e33b47", "#218359", "#ffe066", "#9556cb"};
        for (size_t i = 0; i < values.size(); ++i) {
            auto* swatch = gtk_button_new();
            auto* label = gtk_label_new(nullptr);
            auto* markup = g_markup_printf_escaped("<span foreground=\"%s\" size=\"x-large\">●</span>", values[i]);
            gtk_label_set_markup(GTK_LABEL(label), markup);
            g_free(markup);
            gtk_button_set_child(GTK_BUTTON(swatch), label);
            gtk_widget_set_tooltip_text(swatch, names[i]);
            gtk_accessible_update_property(GTK_ACCESSIBLE(swatch), GTK_ACCESSIBLE_PROPERTY_LABEL, names[i], -1);
            g_object_set_data(G_OBJECT(swatch), "color", const_cast<char*>(values[i]));
            g_signal_connect(swatch, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
                                 auto* self = static_cast<Prototype*>(data);
                                 GdkRGBA color;
                                 gdk_rgba_parse(&color,
                                                static_cast<const char*>(g_object_get_data(G_OBJECT(button), "color")));
                                 gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(self->colorButton), &color);
                                 gtk_popover_popdown(GTK_POPOVER(self->palettePopover));
                                 gtk_widget_grab_focus(self->canvas);
                             }),
                             this);
            gtk_box_append(GTK_BOX(colors), swatch);
        }
        gtk_popover_set_child(GTK_POPOVER(palettePopover), colors);
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
                             if (row && gtk_list_box_row_get_index(row) != self->state.page) {
                                 self->setPage(gtk_list_box_row_get_index(row));
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
            double scale = std::min(112.0 / state.pageWidth, 150.0 / state.pageHeight);
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
        for (const auto& ink: state.ink) {
            if (ink.page == number) {
                prototype::drawInk(cr, ink);
            }
        }
        if (state.current && state.current->page == number) {
            prototype::drawInk(cr, *state.current);
        }
        cairo_restore(cr);
    }

    void activate(GtkApplication* app) {
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                           options.dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
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
        saveButton = button("document-save-as-symbolic", "Save a copy (Ctrl+Shift+S)",
                            G_CALLBACK(+[](GtkButton*, gpointer data) { static_cast<Prototype*>(data)->chooseSave(); }));
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), saveButton);
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

        auto* toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(toolbar, "toolbar");
        gtk_widget_set_halign(toolbar, GTK_ALIGN_CENTER);
        const std::array names = {"Pen", "Highlight", "Eraser"};
        for (size_t i = 0; i < toolButtons.size(); ++i) {
            toolButtons[i] = gtk_toggle_button_new_with_label(names[i]);
            if (i) {
                gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(toolButtons[i]), GTK_TOGGLE_BUTTON(toolButtons[0]));
            }
            g_object_set_data(G_OBJECT(toolButtons[i]), "tool", GUINT_TO_POINTER(i));
            g_signal_connect(toolButtons[i], "toggled", G_CALLBACK(+[](GtkToggleButton* button, gpointer data) {
                                 if (gtk_toggle_button_get_active(button)) {
                                     static_cast<Prototype*>(data)->selectTool(static_cast<Tool>(
                                             GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "tool"))));
                                 }
                             }),
                             this);
            gtk_box_append(GTK_BOX(toolbar), toolButtons[i]);
        }
        auto* colorDialog = gtk_color_dialog_new();
        gtk_color_dialog_set_with_alpha(colorDialog, false);
        // The button constructor takes ownership of the dialog.
        colorButton = gtk_color_dialog_button_new(colorDialog);
        gtk_widget_set_tooltip_text(colorButton, "Ink color");
        g_signal_connect(colorButton, "notify::rgba", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
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
        gtk_box_append(GTK_BOX(toolbar), colorButton);
        gtk_box_append(GTK_BOX(toolbar), createPalette());
        gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
        gtk_box_append(GTK_BOX(toolbar), gtk_label_new("Width"));
        widthButton = gtk_spin_button_new_with_range(0.5, 36, 0.5);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widthButton), 1);
        gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widthButton), true);
        gtk_widget_set_tooltip_text(widthButton, "Stroke width in document points");
        g_signal_connect(widthButton, "value-changed", G_CALLBACK(+[](GtkSpinButton* button, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             if (!self->syncingBrush) {
                                 self->input.cancel();
                                 self->state.brush.width = gtk_spin_button_get_value(button);
                                 self->update();
                             }
                         }),
                         this);
        gtk_box_append(GTK_BOX(toolbar), widthButton);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), toolbar);

        auto* navigation = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(navigation, "toolbar");
        gtk_widget_set_halign(navigation, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(navigation), button("go-previous-symbolic", "Previous page (Page Up)",
                                                   G_CALLBACK(+[](GtkButton*, gpointer data) {
                                                       auto* self = static_cast<Prototype*>(data);
                                                       self->setPage(self->state.page - 1);
                                                   })));
        pageLabel = gtk_label_new("");
        gtk_box_append(GTK_BOX(navigation), pageLabel);
        gtk_box_append(GTK_BOX(navigation),
                       button("go-next-symbolic", "Next page (Page Down)", G_CALLBACK(+[](GtkButton*, gpointer data) {
                                  auto* self = static_cast<Prototype*>(data);
                                  self->setPage(self->state.page + 1);
                              })));
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
        adw_overlay_split_view_set_content(splitView, canvas);
        adw_overlay_split_view_set_sidebar(splitView, createSidebar());
        adw_overlay_split_view_set_min_sidebar_width(splitView, 160);
        adw_overlay_split_view_set_max_sidebar_width(splitView, 180);
        // Sidebar edge swipes could steal drawing gestures. Its button is explicit.
        adw_overlay_split_view_set_enable_show_gesture(splitView, false);
        adw_overlay_split_view_set_enable_hide_gesture(splitView, false);
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
            if (self->busy || self->choosingFile || self->messageDialog) {
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
            openDocument(sourcePath, true);
        }
        if (!options.smokeDirectory.empty()) {
            heartbeatSource = g_timeout_add(10, +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                if (self->busy) {
                    ++self->busyHeartbeats;
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
                                if (self->documentSmokeStep >= 0) {
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
        updateTitle();
        gtk_widget_set_sensitive(openButton, !options.worker.empty());
        gtk_widget_set_sensitive(saveButton, bool(nativeSession));
        const auto label = std::to_string(state.page + 1) + " / " + std::to_string(pageCount);
        gtk_label_set_text(GTK_LABEL(pageLabel), label.c_str());
        gtk_label_set_text(GTK_LABEL(zoomLabel), (std::to_string(static_cast<int>(state.zoom * 100)) + "%").c_str());
        gtk_widget_set_sensitive(undoButton, state.canUndo());
        gtk_widget_set_sensitive(redoButton, state.canRedo());
        if (pageList) {
            gtk_list_box_select_row(pageList, gtk_list_box_get_row_at_index(pageList, state.page));
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
               bool eraserEnd = false) {
        gtk_widget_grab_focus(canvas);
        input.begin(device, {x, y, pressure}, eraserEnd);
        if (input.panning()) {
            gtk_widget_set_cursor_from_name(canvas, "grabbing");
        }
        update();
    }

    void motion(Device device, double x, double y, std::optional<double> pressure = std::nullopt) {
        input.motion(device, {x, y, pressure});
        if (input.panning()) {
            fitted = true;
            fitMode = false;
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
        motion(Device::Stylus, x, y, pressure(gesture));
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
                             if (mouseEvent(GTK_EVENT_CONTROLLER(gesture))) {
                                 auto* self = static_cast<Prototype*>(data);
                                 self->dragX = x;
                                 self->dragY = y;
                                 self->begin(Device::Mouse, x, y);
                             }
                         }),
                         this);
        g_signal_connect(mouse, "drag-update",
                         G_CALLBACK(+[](GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
                             if (mouseEvent(GTK_EVENT_CONTROLLER(gesture))) {
                                 auto* self = static_cast<Prototype*>(data);
                                 self->motion(Device::Mouse, self->dragX + dx, self->dragY + dy);
                             }
                         }),
                         this);
        g_signal_connect(mouse, "drag-end", G_CALLBACK(+[](GtkGestureDrag* gesture, double, double, gpointer data) {
                             if (mouseEvent(GTK_EVENT_CONTROLLER(gesture))) {
                                 auto* self = static_cast<Prototype*>(data);
                                 self->finish(Device::Mouse);
                                 self->update();
                             }
                         }),
                         this);
        addGesture(mouse, Device::Mouse);

        stylus = gtk_gesture_stylus_new();
        gtk_gesture_stylus_set_stylus_only(GTK_GESTURE_STYLUS(stylus), true);
        g_signal_connect(stylus, "down", G_CALLBACK(+[](GtkGestureStylus* gesture, double x, double y, gpointer data) {
                             auto* tool = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture)) ?
                                                  gtk_gesture_stylus_get_device_tool(gesture) :
                                                  nullptr;
                             const bool eraser = tool && prototype::isEraserEnd(gdk_device_tool_get_tool_type(tool));
                             static_cast<Prototype*>(data)->begin(Device::Stylus, x, y, pressure(gesture), eraser);
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
                            self->zoom(prototype::behavior::steppedZoom(self->state.zoom, dy < 0, true), anchor.x,
                                       anchor.y);
                        }
                    } else {
                        self->state.offsetX -= dx * 35;
                        self->state.offsetY -= dy * 35;
                        self->fitted = true;
                        self->fitMode = false;
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
                    if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_o || key == GDK_KEY_O)) {
                        if (!self->options.worker.empty()) {
                            self->protectDocument([self] { self->chooseOpen(); });
                        }
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
                        (key == GDK_KEY_s || key == GDK_KEY_S)) {
                        self->chooseSave();
                        return true;
                    }
                    if (key == GDK_KEY_Escape) {
                        self->resetInput();
                    } else if (GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(self->window)))) {
                        return false;
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
                        self->zoom(prototype::behavior::steppedZoom(self->state.zoom, key != GDK_KEY_minus, false),
                                   gtk_widget_get_width(self->canvas) / 2.0, gtk_widget_get_height(self->canvas) / 2.0);
                    } else if (key == GDK_KEY_Page_Down || key == GDK_KEY_Page_Up) {
                        self->setPage(self->state.page + (key == GDK_KEY_Page_Down ? 1 : -1));
                    } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                               (key == GDK_KEY_p || key == GDK_KEY_h || key == GDK_KEY_e)) {
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
                         G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType, gpointer data) {
                             if (key == GDK_KEY_space) {
                                 auto* self = static_cast<Prototype*>(data);
                                 self->input.spaceHeld = false;
                                 gtk_widget_set_cursor_from_name(self->canvas,
                                                                 self->input.panning() ? "grabbing" : nullptr);
                             }
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
                             if (owner) {
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

    void ensureCache(int deviceScale) {
        // Bound both dimensions and memory, even when zoomed into a huge PDF page.
        const auto scale = std::min({state.zoom * deviceScale, 4096.0 / state.pageWidth, 4096.0 / state.pageHeight});
        if (cache && std::abs(cacheScale - scale) < 0.0001) {
            return;
        }
        cacheScale = scale;
        const int width = std::max(1, static_cast<int>(std::ceil(state.pageWidth * scale)));
        const int height = std::max(1, static_cast<int>(std::ceil(state.pageHeight * scale)));
        cache.reset(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height));
        if (cairo_surface_status(cache.get()) != CAIRO_STATUS_SUCCESS) {
            throw std::runtime_error("Cannot allocate page surface");
        }
        auto* cr = cairo_create(cache.get());
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_paint(cr);
        cairo_scale(cr, scale, scale);
        if (page) {
            poppler_page_render(page, cr);
        }
        cairo_destroy(cr);
        ++rasterizations;
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
        ensureCache(gtk_widget_get_scale_factor(canvas));
        const double background = options.dark ? 0.10 : 0.88;
        cairo_set_source_rgb(cr, background, background, background);
        cairo_paint(cr);
        cairo_translate(cr, state.offsetX, state.offsetY);
        cairo_scale(cr, state.zoom, state.zoom);
        cairo_rectangle(cr, 0, 0, state.pageWidth, state.pageHeight);
        cairo_clip(cr);
        cairo_save(cr);
        cairo_scale(cr, 1 / cacheScale, 1 / cacheScale);
        cairo_set_source_surface(cr, cache.get(), 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
        for (const auto& ink: state.ink) {
            if (ink.page == state.page) {
                prototype::drawInk(cr, ink);
            }
        }
        if (state.current) {
            prototype::drawInk(cr, *state.current);
        }
        ++frames;
    }

    void capture(const std::string& name, GtkWidget* target = nullptr) {
        // Snapshot the window so a crop retains ancestor backgrounds and styling.
        auto* paintable = GDK_PAINTABLE(g_object_ref(windowPaintable));
        auto* snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(window),
                               gtk_widget_get_height(window));
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

        begin(Device::Stylus, x, y, 0.5, true);
        g_signal_emit_by_name(stylus, "up", x, y);
        require(state.ink.size() == count - 1 && state.brush.tool == Tool::Pen,
                "Eraser end changed toolbar selection or failed to erase");
        state.undo();

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

    gboolean smoke() {
        if (busy) {
            require(++documentWaits < 120, "Document operation did not finish");
            return G_SOURCE_CONTINUE;
        }
        documentWaits = 0;
        if (paints <= lastSmokePaint) {
            if (++paintWaits < 60) {
                return G_SOURCE_CONTINUE;
            }
            throw std::runtime_error("Window did not finish painting the requested state");
        }
        paintWaits = 0;
        if (!frames || !cache) {
            require(++smokeWaits < 60, "Canvas has not rendered");
            return G_SOURCE_CONTINUE;
        }
        if (documentSmokeStep >= 0) {
            return smokeDocuments();
        }
        const auto x = state.offsetX + 80 * state.zoom;
        const auto y = state.offsetY + 80 * state.zoom;
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
                const GdkRGBA red{1, 0, 0, 1};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &red);
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(widthButton), 6);
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
                require(state.brush.tool == Tool::Highlighter && state.brush.width == 14,
                        "Highlighter did not restore its preset");
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
                require(state.brush.width == 6 && state.brush.color[0] == 1, "Pen settings were lost on tool switch");
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
                require(!gtk_widget_get_sensitive(colorButton) && !gtk_widget_get_sensitive(widthButton),
                        "Eraser exposes irrelevant brush settings");
                g_signal_emit_by_name(mouse, "drag-begin", x, y);
                g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
                require(state.ink.size() == 1, "Eraser control did not remove pen ink");
                g_signal_emit_by_name(undoButton, "clicked");
                require(state.ink.size() == 2, "Undo button did not restore erased ink");
                g_signal_emit_by_name(redoButton, "clicked");
                require(state.ink.size() == 1, "Redo button did not erase ink again");
                g_signal_emit_by_name(undoButton, "clicked");
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
            case 3:
                require(rasterizations > baselineRasters, "Zoom did not refresh PDF resolution");
                capture("zoom");
                ensureCache(1);
                {
                    const auto width = cairo_image_surface_get_width(cache.get());
                    ensureCache(2);
                    require(cairo_image_surface_get_width(cache.get()) >= width * 2 - 1,
                            "2x cache must preserve logical page size and increase resolution");
                }
                require(cairo_image_surface_get_width(cache.get()) <= 4096 &&
                                cairo_image_surface_get_height(cache.get()) <= 4096,
                        "Cache exceeds dimension limit");
                if (pageCount > 1) {
                    gtk_list_box_select_row(pageList, gtk_list_box_get_row_at_index(pageList, 1));
                    require(state.page == 1 && state.ink.size() == 2, "Sidebar navigation lost session ink");
                }
                gtk_widget_set_halign(contentView, GTK_ALIGN_CENTER);
                gtk_widget_set_size_request(contentView, 520, -1);
                fitted = false;
                update();
                break;
            case 4:
                require(adw_overlay_split_view_get_collapsed(splitView), "Narrow sidebar did not collapse");
                require(gtk_widget_get_width(canvas) < 720, "Narrow viewport did not resize");
                capture("narrow", contentView);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebarButton), true);
                require(adw_overlay_split_view_get_show_sidebar(splitView), "Sidebar toggle did not open overlay");
                break;
            case 5:
                break;  // Let the overlay animation finish before capturing it.
            default: {
                const auto cacheBytes =
                        cairo_image_surface_get_stride(cache.get()) * cairo_image_surface_get_height(cache.get());
                capture("sidebar", contentView);
                if (pageCount > 1) {
                    gtk_list_box_select_row(pageList, gtk_list_box_get_row_at_index(pageList, 0));
                    require(state.page == 0 && !adw_overlay_split_view_get_show_sidebar(splitView),
                            "Selecting a page did not dismiss the overlay");
                }
                require(state.ink.size() == 2, "Session ink did not survive sidebar navigation");
                begin(Device::Mouse, x, y);
                gtk_widget_set_visible(window, false);
                require(!state.current && !state.pinching && !input.active() && !input.spaceHeld &&
                                state.ink.size() == 3,
                        "Unmap dropped delivered ink or left temporary input active");
                std::cout << "PASS: GTK4 Cairo canvas, real PDF, colors, widths, highlight, undo/redo, eraser, "
                             "sidebar navigation/overlay, stylus/backlog/Space adapters, cancel, pinch, narrow "
                             "viewport, 2x raster cache"
                          << "\nframes=" << frames << " rasterizations=" << rasterizations
                          << " device_scale=" << gtk_widget_get_scale_factor(canvas)
                          << " viewport_width=" << gtk_widget_get_width(canvas) << " cache_bytes=" << cacheBytes
                          << '\n';
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
                break;
            }
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
                require(nativeSession && sourcePath == first && state.ink.empty() && !modified() && pageCount == 2,
                        "Reopen did not replace the native document and reset its session history");
                capture("reopened");
                const auto x = state.offsetX + 60 * state.zoom;
                const auto y = state.offsetY + 60 * state.zoom;
                input.begin(Device::Stylus, {x, y, 0.25});
                input.motion(Device::Stylus, {x + 40 * state.zoom, y + 20 * state.zoom, 0.75});
                input.finish(Device::Stylus);
                protectDocument([this] { smokeContinued = true; });
                require(messageDialog && state.ink[0].pressureSensitive, "Pressure ink not retained for saving");
                break;
            }
            case 6:
                capture("save-prompt");
                smokeSaveChoice = second;
                answerDialog("save");
                require(busy && !smokeContinued, "Save continuation ran before completion");
                break;
            case 7:
                require(smokeContinued && !modified() && std::filesystem::is_regular_file(second),
                        "Save from the unsaved prompt failed to finish before continuing");
                capture("saved-again");
                state.undo();
                smokeContinued = false;
                protectDocument([this] { smokeContinued = true; });
                break;
            case 8:
                capture("discard-prompt");
                answerDialog("discard");
                require(smokeContinued && !messageDialog, "Discard did not continue the requested action");
                gtk_window_close(GTK_WINDOW(window));
                require(messageDialog && !closed, "Close did not protect unsaved changes");
                answerDialog("cancel");
                require(!closed && gtk_widget_get_visible(window), "Cancel did not keep the window open");
                break;
            case 9:
                chooseOpen();
                require(choosingFile, "Open action did not request a native file chooser");
                break;
            case 10:
                if (!cancelPrivateChooser()) {
                    require(++chooserWaits < 60, "Native Open chooser did not appear");
                    --documentSmokeStep;
                }
                break;
            case 11:
                require(!choosingFile && !messageDialog && sourcePath == first && modified(),
                        "Cancelling Open changed the current document");
                chooseSave();
                require(choosingFile, "Save action did not request a native file chooser");
                break;
            case 12:
                if (!cancelPrivateChooser()) {
                    require(++chooserWaits < 60, "Native Save chooser did not appear");
                    --documentSmokeStep;
                }
                break;
            case 13:
                require(!choosingFile && !messageDialog && modified() && !closed,
                        "Cancelling Save discarded changes or closed the document");
                break;
            default:
                require(busyHeartbeats > 0, "The UI main loop did not run during document I/O");
                std::cout << "PASS: document actions, async open/save, saved revisions, native reopen, "
                             "pressure transfer, failed-open/save retention, Save/Discard/Cancel, close protection, "
                             "native Open/Save chooser cancellation\n"
                          << "busy_main_loop_ticks=" << busyHeartbeats << '\n';
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
    std::shared_ptr<prototype::bridge::DocumentSession> nativeSession;
    std::string sourcePath;
    std::string savedCopyPath;
    std::string busyText;
    size_t savedRevision = 0;
    bool busy = false;
    bool closed = false;
    bool choosingFile = false;
    int ioErrors = 0;
    AdwAlertDialog* messageDialog = nullptr;
    std::function<void()> afterConfirmation;
    GtkWidget* heading = nullptr;
    GtkWidget* openButton = nullptr;
    GtkWidget* saveButton = nullptr;
    int documentSmokeStep = -1;
    int documentWaits = 0;
    int chooserWaits = 0;
    guint heartbeatSource = 0;
    int busyHeartbeats = 0;
    bool smokeContinued = false;
    std::string smokeSaveChoice;
    CanvasState state;
    prototype::CanvasInput input{state};
    PopplerDocument* document = nullptr;
    PopplerPage* page = nullptr;
    int pageCount = 1;
    GtkWidget* window = nullptr;
    GdkPaintable* windowPaintable = nullptr;
    GdkFrameClock* paintClock = nullptr;
    int paints = 0;
    int lastSmokePaint = 0;
    int paintWaits = 0;
    GtkWidget* contentView = nullptr;
    GtkWidget* canvas = nullptr;
    GtkWidget* pageLabel = nullptr;
    GtkWidget* zoomLabel = nullptr;
    GtkWidget* undoButton = nullptr;
    GtkWidget* redoButton = nullptr;
    GtkWidget* sidebarButton = nullptr;
    GtkWidget* colorButton = nullptr;
    GtkWidget* paletteButton = nullptr;
    GtkWidget* palettePopover = nullptr;
    GtkWidget* widthButton = nullptr;
    std::array<GtkWidget*, 3> toolButtons{};
    AdwOverlaySplitView* splitView = nullptr;
    GtkListBox* pageList = nullptr;
    std::vector<GtkWidget*> thumbnailWidgets;
    std::map<int, std::pair<std::unique_ptr<cairo_surface_t, SurfaceDeleter>, double>> thumbnails;
    prototype::Brush penBrush;
    prototype::Brush highlightBrush{Tool::Highlighter, {1, 0.88, 0.4}, 14};
    bool syncingBrush = false;
    GtkGesture* mouse = nullptr;
    GtkGesture* stylus = nullptr;
    std::vector<GtkGesture*> panGestures;
    GtkGesture* pinch = nullptr;
    GtkEventController* keys = nullptr;
    std::unique_ptr<cairo_surface_t, SurfaceDeleter> cache;
    double cacheScale = 0;
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
    int snapshotWaits = 0;
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
                                         "[--dark] [--smoke-dir DIRECTORY]");
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
