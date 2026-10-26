#include "SidebarIndexPage.h"

#include <cstring>  // for strlen

#include <glib-object.h>  // for g_object_unref
#include <pango/pango.h>  // for PangoLogAttr

#include "control/Control.h"        // for Control
#include "control/ScrollHandler.h"  // for ScrollHandler
#include "model/Document.h"         // for Document
#include "model/LinkDestination.h"  // for XojLinkDest
#include "util/Assert.h"            // for xoj_assert
#include "util/glib_casts.h"        // for wrap_v
#include "util/gtk4_helper.h"       //
#include "util/i18n.h"              // for _
#include "util/safe_casts.h"        // for as_unsigned
#include "util/Util.h"              // for npos

namespace {

auto createContentsModel(const Document& document) -> GtkTreeModel* {
    const auto& contents = document.getContents();
    if (contents.empty()) {
        return nullptr;
    }

    GtkTreeStore* store = gtk_tree_store_new(4, G_TYPE_STRING, G_TYPE_OBJECT, G_TYPE_BOOLEAN, G_TYPE_STRING);
    const auto append = [&document, store](auto&& self, const std::vector<DocumentOutlineNode>& nodes,
                                           GtkTreeIter* parent) -> void {
        for (const auto& node: nodes) {
            GtkTreeIter iter;
            auto* link = link_dest_new();
            link->dest = new LinkDestination(node.destination);
            auto* title = g_markup_escape_text(node.destination.getName().c_str(), -1);
            const auto page = document.findPdfPage(node.destination.getPdfPage());
            auto* pageLabel = page == npos ? nullptr : g_strdup_printf("%zu", page + 1);

            gtk_tree_store_append(store, &iter, parent);
            gtk_tree_store_set(store, &iter, DOCUMENT_LINKS_COLUMN_NAME, title, DOCUMENT_LINKS_COLUMN_LINK, link,
                               DOCUMENT_LINKS_COLUMN_EXPAND, node.destination.getExpand(),
                               DOCUMENT_LINKS_COLUMN_PAGE_NUMBER, pageLabel, -1);

            g_free(pageLabel);
            g_free(title);
            g_object_unref(link);
            self(self, node.children, &iter);
        }
    };

    append(append, contents, nullptr);
    return GTK_TREE_MODEL(store);
}

auto updateBookmarkPageNumber(GtkTreeModel* model, GtkTreePath*, GtkTreeIter* iter, gpointer data) -> gboolean {
    auto* document = static_cast<Document*>(data);
    XojLinkDest* link = nullptr;
    gtk_tree_model_get(model, iter, DOCUMENT_LINKS_COLUMN_LINK, &link, -1);
    if (!link || !link->dest) {
        g_clear_object(&link);
        return FALSE;
    }

    const auto page = document->findPdfPage(link->dest->getPdfPage());
    auto* pageLabel = page == npos ? nullptr : g_strdup_printf("%zu", page + 1);
    gtk_tree_store_set(GTK_TREE_STORE(model), iter, DOCUMENT_LINKS_COLUMN_PAGE_NUMBER, pageLabel, -1);
    g_free(pageLabel);
    g_object_unref(link);
    return FALSE;
}

}  // namespace

SidebarIndexPage::SidebarIndexPage(Control* control):
        AbstractSidebarPage(control), iconNameHelper(control->getSettings()) {
    this->treeViewBookmarks = gtk_tree_view_new();
    g_object_ref(this->treeViewBookmarks);

    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(treeViewBookmarks), true);
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(treeViewBookmarks), DOCUMENT_LINKS_COLUMN_NAME);
    gtk_tree_view_set_search_equal_func(GTK_TREE_VIEW(treeViewBookmarks),
                                        reinterpret_cast<GtkTreeViewSearchEqualFunc>(treeSearchFunction), this,
                                        nullptr);

    this->scrollBookmarks = gtk_scrolled_window_new();
    g_object_ref(this->scrollBookmarks);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrollBookmarks), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeViewBookmarks));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(treeViewBookmarks), false);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrollBookmarks), treeViewBookmarks);


    GtkTreeViewColumn* column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_expand(GTK_TREE_VIEW_COLUMN(column), true);
    gtk_tree_view_append_column(GTK_TREE_VIEW(treeViewBookmarks), column);

    auto* renderer = static_cast<GtkCellRenderer*>(
            g_object_new(GTK_TYPE_CELL_RENDERER_TEXT, "ellipsize", PANGO_ELLIPSIZE_END, nullptr));
    gtk_tree_view_column_pack_start(GTK_TREE_VIEW_COLUMN(column), renderer, true);
    gtk_tree_view_column_set_attributes(GTK_TREE_VIEW_COLUMN(column), renderer, "markup", DOCUMENT_LINKS_COLUMN_NAME,
                                        nullptr);

    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_end(GTK_TREE_VIEW_COLUMN(column), renderer, false);
    gtk_tree_view_column_set_attributes(GTK_TREE_VIEW_COLUMN(column), renderer, "text",
                                        DOCUMENT_LINKS_COLUMN_PAGE_NUMBER, nullptr);
    g_object_set(G_OBJECT(renderer), "style", PANGO_STYLE_ITALIC, nullptr);

    this->selectHandler = g_signal_connect(treeViewBookmarks, "cursor-changed", G_CALLBACK(treeBookmarkSelected), this);
    xoj_assert(this->selectHandler != 0);

    gtk_widget_show(this->treeViewBookmarks);

    registerListener(control);
}

SidebarIndexPage::~SidebarIndexPage() {
    if (this->searchTimeout) {
        g_source_remove(this->searchTimeout);
        this->searchTimeout = 0;
    }
    if (this->pageNumberUpdateSource) {
        g_source_remove(this->pageNumberUpdateSource);
        this->pageNumberUpdateSource = 0;
    }

    gtk_tree_view_set_model(GTK_TREE_VIEW(this->treeViewBookmarks), nullptr);
    g_clear_object(&this->contentsModel);
    g_object_unref(this->treeViewBookmarks);
    g_object_unref(this->scrollBookmarks);
}

void SidebarIndexPage::enableSidebar() {}

void SidebarIndexPage::disableSidebar() {
    // Nothing to do at the moment
}

void SidebarIndexPage::treeBookmarkSelected(GtkTreeView* treeview, SidebarIndexPage* sidebar) {
    if (sidebar->searchTimeout) {
        return;
    }

    gtk_widget_grab_focus(GTK_WIDGET(treeview));

    GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));

    if (selection) {
        GtkTreeModel* model = nullptr;
        GtkTreeIter iter = {0};

        if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
            XojLinkDest* link = nullptr;

            gtk_tree_model_get(model, &iter, DOCUMENT_LINKS_COLUMN_LINK, &link, -1);
            if (link && link->dest) {
                LinkDestination* dest = link->dest;

                sidebar->control->getScrollHandler()->scrollToLinkDest(*dest);
            }
            g_object_unref(link);
        }
    }
}

auto SidebarIndexPage::searchTimeoutFunc(SidebarIndexPage* sidebar) -> bool {
    sidebar->searchTimeout = 0;

    treeBookmarkSelected(GTK_TREE_VIEW(sidebar->treeViewBookmarks), sidebar);

    return false;
}

auto SidebarIndexPage::treeSearchFunction(GtkTreeModel* model, gint column, const gchar* key, GtkTreeIter* iter,
                                          SidebarIndexPage* sidebar) -> gboolean {
    if (sidebar->searchTimeout) {
        g_source_remove(sidebar->searchTimeout);
        sidebar->searchTimeout = 0;
    }
    sidebar->searchTimeout = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT_IDLE, 2,
                                                        xoj::util::wrap_v<searchTimeoutFunc>, sidebar, nullptr);

    // Source: Pidgin
    gchar* text = nullptr;
    gtk_tree_model_get(model, iter, DOCUMENT_LINKS_COLUMN_NAME, &text, -1);
    if (text == nullptr) {
        return true;
    }

    gchar* tmp = g_utf8_normalize(key, -1, G_NORMALIZE_DEFAULT);
    gchar* enteredstring = g_utf8_casefold(tmp, -1);
    g_free(tmp);

    tmp = g_utf8_normalize(text, -1, G_NORMALIZE_DEFAULT);
    gchar* normalized = g_utf8_casefold(tmp, -1);
    g_free(tmp);

    if (g_str_has_prefix(normalized, enteredstring)) {
        g_free(enteredstring);
        g_free(normalized);
        return false;
    }

    /* Use Pango to separate by words. */
    size_t len = as_unsigned(g_utf8_strlen(normalized, -1));
    PangoLogAttr* log_attrs = g_new(PangoLogAttr, len + 1);

    pango_get_log_attrs(normalized, static_cast<int>(strlen(normalized)), -1, nullptr, log_attrs,
                        static_cast<int>(len + 1));

    gchar* word = normalized;
    gboolean result = true;
    for (size_t i = 0; i < (len - 1); i++) {
        if (log_attrs[i].is_word_start && g_str_has_prefix(word, enteredstring)) {
            result = false;
            break;
        }
        word = g_utf8_next_char(word);
    }
    g_free(log_attrs);

    g_free(enteredstring);
    g_free(normalized);

    return result;
}

auto SidebarIndexPage::getName() -> std::string { return _("Contents"); }

auto SidebarIndexPage::getIconName() -> std::string { return this->iconNameHelper.iconName("sidebar-index"); }

auto SidebarIndexPage::hasData() -> bool { return this->hasContents; }

auto SidebarIndexPage::getWidget() -> GtkWidget* { return this->scrollBookmarks; }

auto SidebarIndexPage::expandOpenLinks(GtkTreeModel* model, GtkTreeIter* parent) -> int {
    GtkTreeIter iter = {0};
    XojLinkDest* link = nullptr;
    if (model == nullptr) {
        return 0;
    }

    int count = 0;

    if (gtk_tree_model_iter_children(model, &iter, parent)) {
        do {
            gtk_tree_model_get(model, &iter, DOCUMENT_LINKS_COLUMN_LINK, &link, -1);

            if (link->dest->getExpand()) {
                GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
                gtk_tree_view_expand_row(GTK_TREE_VIEW(treeViewBookmarks), path, false);
                gtk_tree_path_free(path);
            }

            g_object_unref(link);

            count++;

            count += expandOpenLinks(model, &iter);
        } while (gtk_tree_model_iter_next(model, &iter));
    }

    return count;
}

void SidebarIndexPage::selectPageNr(size_t page, size_t pdfPage) { selectPageNr(page, pdfPage, nullptr); }

auto SidebarIndexPage::selectPageNr(size_t page, size_t pdfPage, GtkTreeIter* parent) -> bool {
    GtkTreeIter iter;

    GtkTreeModel* model = this->contentsModel;
    if (model == nullptr) {
        return false;
    }

    g_object_ref(model);

    if (parent == nullptr) {

        // check if there is already the current page selected
        GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeViewBookmarks));

        if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
            XojLinkDest* link = nullptr;

            gtk_tree_model_get(model, &iter, DOCUMENT_LINKS_COLUMN_LINK, &link, -1);

            if (link && link->dest) {
                LinkDestination* dest = link->dest;

                if (dest->getPdfPage() == pdfPage) {

                    g_object_unref(model);
                    g_object_unref(link);

                    // already a bookmark from this page selected
                    return true;
                }
            }

            g_object_unref(link);
        }
    }

    gboolean valid = gtk_tree_model_iter_children(model, &iter, parent);

    while (valid) {
        XojLinkDest* link = nullptr;

        gtk_tree_model_get(model, &iter, DOCUMENT_LINKS_COLUMN_LINK, &link, -1);

        if (link->dest->getPdfPage() == pdfPage) {
            GtkTreeSelection* selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeViewBookmarks));
            gtk_tree_selection_select_iter(selection, &iter);

            g_object_unref(link);
            g_object_unref(model);
            return true;
        }


        g_object_unref(link);

        if (selectPageNr(page, pdfPage, &iter)) {
            g_object_unref(model);
            return true;
        }


        valid = gtk_tree_model_iter_next(model, &iter);
    }

    g_object_unref(model);
    return false;
}

void SidebarIndexPage::documentChanged(DocumentChangeType type) {
    if (type == DOCUMENT_CHANGE_CLEARED) {
        gtk_tree_view_set_model(GTK_TREE_VIEW(this->treeViewBookmarks), nullptr);
        g_clear_object(&this->contentsModel);
        hasContents = false;
    } else if (type == DOCUMENT_CHANGE_PDF_BOOKMARKS || type == DOCUMENT_CHANGE_COMPLETE) {

        Document* doc = this->control->getDocument();

        //  Block the cursor-change signal when the document changes, otherwise
        //  there will be a deadlock: both the selectHandler and this code will
        //  lock the document.
        g_signal_handler_block(this->treeViewBookmarks, this->selectHandler);
        doc->lock_shared();
        GtkTreeModel* model = createContentsModel(*doc);
        doc->unlock_shared();
        gtk_tree_view_set_model(GTK_TREE_VIEW(this->treeViewBookmarks), model);
        g_clear_object(&this->contentsModel);
        this->contentsModel = model;
        int count = expandOpenLinks(model, nullptr);
        g_signal_handler_unblock(this->treeViewBookmarks, this->selectHandler);
        this->treeBookmarkSelected(GTK_TREE_VIEW(this->treeViewBookmarks), this);

        hasContents = (count != 0);
    }
}

void SidebarIndexPage::pageInserted(size_t) { updatePageNumbers(); }

void SidebarIndexPage::pageDeleted(size_t) {
    if (!this->pageNumberUpdateSource) {
        this->pageNumberUpdateSource = g_idle_add(updatePageNumbersIdle, this);
    }
}

auto SidebarIndexPage::updatePageNumbersIdle(gpointer data) -> gboolean {
    auto* self = static_cast<SidebarIndexPage*>(data);
    self->pageNumberUpdateSource = 0;
    self->updatePageNumbers();
    return G_SOURCE_REMOVE;
}

void SidebarIndexPage::updatePageNumbers() {
    if (!this->contentsModel) {
        return;
    }

    auto* document = this->control->getDocument();
    document->lock_shared();
    gtk_tree_model_foreach(this->contentsModel, updateBookmarkPageNumber, document);
    document->unlock_shared();
}

void SidebarIndexPage::layout() {}
