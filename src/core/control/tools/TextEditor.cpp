#include "TextEditor.h"

#include <algorithm>
#include <cmath>
#include <cstring>  // for strcmp, size_t
#include <memory>   // for allocator, make_unique, __shared_p...
#include <string>   // for std::string()
#include <utility>  // for move
#include <vector>

#include <gdk/gdkkeysyms.h>  // for GDK_KEY_B, GDK_KEY_ISO_Enter, GDK_...
#include <glib-object.h>     // for g_object_get, g_object_unref, G_CA...

#include "control/AudioController.h"
#include "control/Control.h"  // for Control
#include "control/actions/ActionDatabase.h"
#include "control/settings/Settings.h"
#include "gui/FlyingClickableIcon.h"
#include "gui/XournalppCursor.h"  // for XournalppCursor
#include "model/Document.h"       // for Document
#include "model/Font.h"           // for XojFont
#include "model/Text.h"           // for Text
#include "model/TextAlignment.h"  // for TextAlignment
#include "model/XojPage.h"        // for XojPage
#include "undo/DeleteUndoAction.h"
#include "undo/InsertUndoAction.h"
#include "undo/TextBoxUndoAction.h"
#include "undo/UndoRedoHandler.h"  // for UndoRedoHandler
#include "util/Assert.h"
#include "util/DispatchPool.h"
#include "util/Range.h"
#include "util/glib_casts.h"  // for wrap_for_once_v
#include "util/gtk4_helper.h"
#include "util/raii/CStringWrapper.h"
#include "util/safe_casts.h"  // for round_cast, as_unsigned
#include "view/overlays/TextEditionView.h"

#include "TextEditorKeyBindings.h"

class UndoAction;

static constexpr auto MOVE_ICON_NAME = "xopp-move";
static constexpr auto EXTEND_ICON_NAME = "xopp-wrap";
static constexpr size_t MAX_TEXT_EDIT_HISTORY = 256;

/** GtkTextBuffer helper functions **/
static auto getIteratorAtCursor(GtkTextBuffer* buffer) -> GtkTextIter {
    GtkTextIter cursorIter = {nullptr};
    gtk_text_buffer_get_iter_at_mark(buffer, &cursorIter, gtk_text_buffer_get_insert(buffer));
    return cursorIter;
}

/**
 * @brief Compute the byte offset of an iterator in the GtkTextBuffer
 *
 * NB: This is much faster than relying on g_utf8_offset_to_pointer
 */
static auto getByteOffsetOfIterator(GtkTextIter it) -> int {
    // Bytes from beginning of line to iterator
    int pos = gtk_text_iter_get_line_index(&it);
    gtk_text_iter_set_line_index(&it, 0);
    // Count bytes of previous lines
    while (gtk_text_iter_backward_line(&it)) {
        pos += gtk_text_iter_get_bytes_in_line(&it);
    }
    return pos;
}

static auto getByteOffsetOfCursor(GtkTextBuffer* buffer) -> int {
    return getByteOffsetOfIterator(getIteratorAtCursor(buffer));
}

/**
 * @brief Get an iterator at the prescribed byte index.
 *
 * NB: This is much faster than relying on g_utf8_pointer_to_offset for long texts
 */
static auto getIteratorAtByteOffset(GtkTextBuffer* buf, int byteIndex) {
    xoj_assert(byteIndex >= 0);
    GtkTextIter it = {nullptr};
    gtk_text_buffer_get_start_iter(buf, &it);

    // Fast forward to the beginning of the line containing our target destination
    for (int linelength = gtk_text_iter_get_bytes_in_line(&it);
         linelength <= byteIndex && gtk_text_iter_forward_line(&it);
         byteIndex -= std::exchange(linelength, gtk_text_iter_get_bytes_in_line(&it))) {}

    if (!gtk_text_iter_is_end(&it)) {
        gtk_text_iter_set_line_index(&it, byteIndex);
    }
    // else { // byteIndex was either past-the-end or pointed to the end }

    return it;
}

static auto cloneToCString(GtkTextBuffer* buf) {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    return xoj::util::OwnedCString::assumeOwnership(gtk_text_iter_get_text(&start, &end));
}

/**
 * @brief Clone the buffer's content
 *
 * This is pretty inefficient: the text gets copied twice
 */
static auto cloneToStdString(GtkTextBuffer* buf) -> std::string { return cloneToCString(buf).get(); }

/**
 * @brief Clone the buffer's content and insert a string into the clone
 * This makes one less copy operation over using cloneToStdString followed by insert()
 * (The text is copied "only" twice, and not three times)
 */
static auto cloneWithInsertToStdString(GtkTextBuffer* buf, std::string_view insertedStr) -> std::string {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    GtkTextIter insertionPoint = getIteratorAtCursor(buf);

    auto firstHalf = xoj::util::OwnedCString::assumeOwnership(gtk_text_iter_get_text(&start, &insertionPoint));
    auto secondHalf = xoj::util::OwnedCString::assumeOwnership(gtk_text_iter_get_text(&insertionPoint, &end));

    std::string_view str1(firstHalf);
    std::string_view str2(secondHalf);

    std::string res;
    res.reserve(str1.length() + str2.length() + insertedStr.length());
    res += str1;
    res += insertedStr;
    res += str2;

    return res;
}

TextEditor::TextEditor(Control* control, const PageRef& page, GtkWidget* xournalWidget, double x, double y):
        control(control),
        page(page),
        xournalWidget(xournalWidget),
        imContext(gtk_im_multicontext_new(), xoj::util::adopt),
        buffer(gtk_text_buffer_new(nullptr), xoj::util::adopt),
        viewPool(std::make_shared<xoj::util::DispatchPool<xoj::view::TextEditionView>>()) {
    // Informs the windowing system of the selection -- i.e. for accessibility purposes
    gtk_text_buffer_add_selection_clipboard(buffer.get(), gtk_clipboard_get(GDK_SELECTION_PRIMARY));

    this->initializeEditionAt(x, y);
    g_signal_connect(this->buffer.get(), "paste-done", G_CALLBACK(bufferPasteDoneCallback), this);

    {  // Get cursor blinking settings
        GtkSettings* settings = gtk_widget_get_settings(this->xournalWidget);
        g_object_get(settings, "gtk-cursor-blink", &this->cursorBlink, nullptr);
        if (this->cursorBlink) {
            int tmp = 0;
            g_object_get(settings, "gtk-cursor-blink-time", &tmp, nullptr);
            xoj_assert(tmp >= 0);
            auto cursorBlinkingPeriod = static_cast<unsigned int>(tmp);
            this->cursorBlinkingTimeOn = cursorBlinkingPeriod * CURSOR_ON_MULTIPLIER / CURSOR_DIVIDER;
            this->cursorBlinkingTimeOff = cursorBlinkingPeriod - this->cursorBlinkingTimeOn;
        }
    }

    gtk_im_context_set_client_widget(this->imContext.get(), this->xournalWidget);
    gtk_im_context_focus_in(this->imContext.get());

    g_signal_connect(this->imContext.get(), "commit", G_CALLBACK(iMCommitCallback), this);
    g_signal_connect(this->imContext.get(), "preedit-changed", G_CALLBACK(iMPreeditChangedCallback), this);
    g_signal_connect(this->imContext.get(), "retrieve-surrounding", G_CALLBACK(iMRetrieveSurroundingCallback), this);
    g_signal_connect(this->imContext.get(), "delete-surrounding", G_CALLBACK(imDeleteSurroundingCallback), this);

    if (this->originalTextElement) {
        // If editing a preexisting text, put the cursor at the right location
        this->mousePressed(x, y);
    } else if (this->cursorBlink) {
        blinkCallback(this);
    } else {
        this->cursorVisible = true;
    }

    this->initializeTextEditHistory();

    this->moveIcon = [&]() {
        auto icon = std::make_unique<FlyingClickableIcon>(control->getWindow(), MOVE_ICON_NAME,
                                                          FlyingClickableIcon::Anchor::SOUTH_EAST);

        GtkWidget* w = icon->getWidget();

#if GTK_MAJOR_VERSION == 3
        gtk_widget_add_css_class(gtk_bin_get_child(GTK_BIN(w)), "TL");
        GtkGesture* drag = gtk_gesture_drag_new(w);
#else
        gtk_widget_add_css_class(w, "TL");
        GtkGesture* drag = gtk_gesture_drag_new();
        gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(drag));
#endif

        icon->addSignal(
                G_OBJECT(drag),
                g_signal_connect(drag, "drag-update",
                                 G_CALLBACK(+[](GtkGestureDrag*, gdouble offsetX, gdouble offsetY, gpointer p) {
                                     // Warning: because the icon is moved, the parameters offsetX and offsetY
                                     // are NOT relative to the starting point, but rather to the last update's
                                     // position.
                                     auto* self = static_cast<TextEditor*>(p);
                                     if (!self->viewPool->empty()) {
                                         // We use the first view as the main view
                                         auto* text = self->getTextElement();
                                         const auto zoom = self->viewPool->front().getZoom();
                                         const xoj::util::Point<double> move(offsetX / zoom, offsetY / zoom);
                                         const auto newOrigin = text->getOrigin() + move;
                                         const double width = self->currentWrapWidth == Text::NO_WRAP ?
                                                                      self->getContentBoundingBox().getWidth() :
                                                                      self->currentWrapWidth;
                                         if (newOrigin.x > 0 && newOrigin.x + width < self->page->getWidth() &&
                                             newOrigin.y > 0 &&
                                             newOrigin.y + self->getContentBoundingBox().getHeight() <
                                                     self->page->getHeight()) {
                                             // The text stays entirely in the page
                                             text->move(move.x, move.y);
                                             self->repaintEditor(true);
                                         }
                                     }
                                 }),
                                 this));
        // Should we implement signals drag-end/cancel here?
        return icon;
    }();
    this->extendIcon = [&]() {
        auto icon = std::make_unique<FlyingClickableIcon>(control->getWindow(), EXTEND_ICON_NAME,
                                                          FlyingClickableIcon::Anchor::SOUTH_WEST);

        GtkWidget* w = icon->getWidget();

#if GTK_MAJOR_VERSION == 3
        gtk_widget_add_css_class(gtk_bin_get_child(GTK_BIN(w)), "TR");
        GtkGesture* drag = gtk_gesture_drag_new(w);
#else
        gtk_widget_add_css_class(w, "TR");
        GtkGesture* drag = gtk_gesture_drag_new();
        gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(drag));
#endif

        icon->addSignal(G_OBJECT(drag),
                        g_signal_connect(drag, "drag-begin",
                                         G_CALLBACK(+[](GtkGestureDrag*, gdouble startX, gdouble startY, gpointer p) {
                                             auto* self = static_cast<TextEditor*>(p);
                                             if (self->currentWrapWidth == Text::NO_WRAP) {
                                                 self->currentWrapWidth = self->getContentBoundingBox().getWidth();
                                             }
                                         }),
                                         this));
        icon->addSignal(G_OBJECT(drag),
                        g_signal_connect(drag, "drag-update",
                                         G_CALLBACK(+[](GtkGestureDrag*, gdouble offsetX, gdouble offsetY, gpointer p) {
                                             // Warning: because the icon is moved, the parameters offsetX and offsetY
                                             // are NOT relative to the starting point, but rather to the last update's
                                             // position.
                                             auto* self = static_cast<TextEditor*>(p);
                                             if (!self->viewPool->empty()) {
                                                 // We use the first view as the main view
                                                 if (double newVal = self->currentWrapWidth +
                                                                     offsetX / self->viewPool->front().getZoom();
                                                     newVal > 0 &&
                                                     newVal < self->page->getWidth() -
                                                                      self->getTextElement()->getOrigin().x) {
                                                     // The new width does not overflow out of the page
                                                     self->currentWrapWidth = newVal;
                                                     self->layoutStatus = LayoutStatus::NEEDS_PARAMETERS_UPDATE;
                                                     self->repaintEditor(true);
                                                 }
                                             }
                                         }),
                                         this));
        icon->addSignal(G_OBJECT(drag),
                        g_signal_connect(drag, "drag-end",
                                         G_CALLBACK(+[](GtkGestureDrag*, gdouble offsetX, gdouble offsetY, gpointer p) {
                                             auto* self = static_cast<TextEditor*>(p);
                                             self->textElement->setWrap(self->currentWrapWidth);
                                         }),
                                         this));
        icon->addSignal(G_OBJECT(drag),
                        g_signal_connect(drag, "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer p) {
                                             auto* self = static_cast<TextEditor*>(p);
                                             self->currentWrapWidth = self->textElement->getWrap();
                                             self->layoutStatus = LayoutStatus::NEEDS_PARAMETERS_UPDATE;
                                             self->repaintEditor(true);
                                         }),
                                         this));
        // Move both icons when scrolling/zooming
        auto cb = G_CALLBACK(+[](GtkAdjustment*, gpointer p) { static_cast<TextEditor*>(p)->updateDraggableIcons(); });
        auto* hadj = G_OBJECT(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(xournalWidget)));
        icon->addSignal(hadj, g_signal_connect(hadj, "value-changed", cb, this));
        icon->addSignal(hadj, g_signal_connect(hadj, "changed", cb, this));
        auto* vadj = G_OBJECT(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(xournalWidget)));
        icon->addSignal(vadj, g_signal_connect(vadj, "value-changed", cb, this));
        icon->addSignal(vadj, g_signal_connect(vadj, "changed", cb, this));
        return icon;
    }();
}

TextEditor::~TextEditor() {
    gtk_im_context_focus_out(this->imContext.get());

    this->xournalWidget = nullptr;
    control->setCopyCutEnabled(false);

    this->contentsChanged(true);

    finalizeEdition();
    this->control->undoRedoChanged();
}

auto TextEditor::getViewPool() const -> const std::shared_ptr<xoj::util::DispatchPool<xoj::view::TextEditionView>>& {
    return viewPool;
}

void TextEditor::onViewCreation() const {
    this->updateDraggableIcons();  // The icons are placed on the first view. The view must have been created for that
}

auto TextEditor::getTextElement() const -> Text* { return this->textElement.get(); }

bool TextEditor::bufferEmpty() const { return gtk_text_buffer_get_char_count(this->buffer.get()) == 0; }

void TextEditor::replaceBufferContent(const std::string& text) {
    gtk_text_buffer_set_text(this->buffer.get(), text.c_str(), -1);
    this->applyModelStylesToBuffer();
    this->typingFont.reset();

    GtkTextIter first = {nullptr};
    gtk_text_buffer_get_iter_at_offset(this->buffer.get(), &first, 0);
    gtk_text_buffer_place_cursor(this->buffer.get(), &first);
    this->layoutStatus = LayoutStatus::NEEDS_COMPLETE_UPDATE;
    this->cursorBox = computeCursorBox();
    this->updateFormattingActions();
}

auto TextEditor::captureTextEditState() const -> TextEditState {
    TextEditState state;
    state.text = this->textElement->getText();
    state.styleRuns = this->textElement->serializeStyleRuns();
    state.font = this->textElement->getFont();
    state.color = this->textElement->getColor();
    state.wrapWidth = this->textElement->getWrap();
    state.alignment = static_cast<int>(static_cast<TextAlignment::Value>(this->textElement->getAlign()));
    state.justify = this->textElement->getJustify();
    state.typingFont = this->typingFont;

    const GtkTextIter cursor = getIteratorAtCursor(this->buffer.get());
    GtkTextIter selectionBound;
    gtk_text_buffer_get_iter_at_mark(this->buffer.get(), &selectionBound,
                                     gtk_text_buffer_get_selection_bound(this->buffer.get()));
    state.cursorOffset = static_cast<size_t>(getByteOffsetOfIterator(cursor));
    state.selectionBoundOffset = static_cast<size_t>(getByteOffsetOfIterator(selectionBound));
    return state;
}

void TextEditor::restoreTextEditState(const TextEditState& state) {
    this->restoringTextEditHistory = true;

    this->textElement->setText(state.text);
    this->textElement->setFont(state.font);
    if (state.styleRuns.empty()) {
        this->textElement->setStyleRuns({});
    } else {
        this->textElement->deserializeStyleRuns(state.styleRuns);
    }
    this->textElement->setColor(state.color);
    this->textElement->setWrap(state.wrapWidth);
    this->textElement->setAlignment(TextAlignment(static_cast<TextAlignment::Value>(state.alignment)));
    this->textElement->setJustify(state.justify);

    gtk_text_buffer_set_text(this->buffer.get(), state.text.c_str(), -1);
    this->applyModelStylesToBuffer();
    this->typingFont = state.typingFont;

    const auto clampOffset = [&state](size_t offset) {
        return static_cast<int>(std::min(offset, state.text.size()));
    };
    GtkTextIter cursor = getIteratorAtByteOffset(this->buffer.get(), clampOffset(state.cursorOffset));
    GtkTextIter selectionBound = getIteratorAtByteOffset(this->buffer.get(), clampOffset(state.selectionBoundOffset));
    gtk_text_buffer_select_range(this->buffer.get(), &cursor, &selectionBound);

    this->control->setCopyCutEnabled(gtk_text_buffer_get_has_selection(this->buffer.get()));
    this->layoutStatus = LayoutStatus::NEEDS_COMPLETE_UPDATE;
    this->computeVirtualCursorPosition();
    this->updateFormattingActions();
    this->repaintEditor(true);

    this->restoringTextEditHistory = false;
}

void TextEditor::initializeTextEditHistory() {
    this->textEditHistory.clear();
    this->textEditHistory.emplace_back(this->captureTextEditState());
    this->textEditHistoryIndex = 0;
    this->updateTextEditUndoActions();
}

void TextEditor::prepareTextEditHistory() {
    if (this->restoringTextEditHistory || this->textEditHistory.empty()) {
        return;
    }

    const auto current = this->captureTextEditState();
    auto& currentHistoryState = this->textEditHistory[this->textEditHistoryIndex];
    currentHistoryState.cursorOffset = current.cursorOffset;
    currentHistoryState.selectionBoundOffset = current.selectionBoundOffset;
}

void TextEditor::recordTextEditHistory() {
    if (this->restoringTextEditHistory || this->textEditHistory.empty()) {
        return;
    }

    const auto current = this->captureTextEditState();
    const auto sameFont = [](const XojFont& lhs, const XojFont& rhs) {
        return lhs.getName() == rhs.getName() && lhs.getSize() == rhs.getSize();
    };
    const auto sameOptionalFont = [&sameFont](const std::optional<XojFont>& lhs,
                                               const std::optional<XojFont>& rhs) {
        if (lhs.has_value() != rhs.has_value()) {
            return false;
        }
        return !lhs || sameFont(*lhs, *rhs);
    };
    const auto sameState = [&sameFont, &sameOptionalFont](const TextEditState& lhs, const TextEditState& rhs) {
        return lhs.text == rhs.text && lhs.styleRuns == rhs.styleRuns && sameFont(lhs.font, rhs.font) &&
               lhs.color == rhs.color && lhs.wrapWidth == rhs.wrapWidth && lhs.alignment == rhs.alignment &&
               lhs.justify == rhs.justify && sameOptionalFont(lhs.typingFont, rhs.typingFont) &&
               lhs.cursorOffset == rhs.cursorOffset && lhs.selectionBoundOffset == rhs.selectionBoundOffset;
    };

    if (sameState(this->textEditHistory[this->textEditHistoryIndex], current)) {
        this->updateTextEditUndoActions();
        return;
    }

    if (this->textEditHistoryIndex + 1 < this->textEditHistory.size()) {
        this->textEditHistory.erase(this->textEditHistory.begin() +
                                            static_cast<std::ptrdiff_t>(this->textEditHistoryIndex + 1),
                                    this->textEditHistory.end());
    }

    this->textEditHistory.emplace_back(current);
    this->textEditHistoryIndex = this->textEditHistory.size() - 1;

    if (this->textEditHistory.size() > MAX_TEXT_EDIT_HISTORY) {
        const auto removeCount = this->textEditHistory.size() - MAX_TEXT_EDIT_HISTORY;
        this->textEditHistory.erase(this->textEditHistory.begin(),
                                    this->textEditHistory.begin() + static_cast<std::ptrdiff_t>(removeCount));
        this->textEditHistoryIndex -= removeCount;
    }

    this->updateTextEditUndoActions();
}

void TextEditor::beginTextEditHistoryGroup() {
    this->prepareTextEditHistory();
    ++this->textEditHistoryGroupDepth;
}

void TextEditor::endTextEditHistoryGroup() {
    if (this->textEditHistoryGroupDepth == 0) {
        return;
    }

    --this->textEditHistoryGroupDepth;
    if (this->textEditHistoryGroupDepth == 0) {
        this->recordTextEditHistory();
    }
}

void TextEditor::updateTextEditUndoActions() const {
    auto* undoRedo = this->control->getUndoRedoHandler();
    auto* actionDb = this->control->getActionDatabase();
    actionDb->enableAction(Action::UNDO, undoRedo->canUndo() || this->canUndoTextEdit());
    actionDb->enableAction(Action::REDO, undoRedo->canRedo() || this->canRedoTextEdit());
}

auto TextEditor::canUndoTextEdit() const -> bool {
    return !this->textEditHistory.empty() && this->textEditHistoryIndex > 0;
}

auto TextEditor::canRedoTextEdit() const -> bool {
    return !this->textEditHistory.empty() && this->textEditHistoryIndex + 1 < this->textEditHistory.size();
}

auto TextEditor::undoTextEdit() -> bool {
    if (!this->canUndoTextEdit()) {
        return false;
    }

    --this->textEditHistoryIndex;
    this->restoreTextEditState(this->textEditHistory[this->textEditHistoryIndex]);
    this->updateTextEditUndoActions();
    return true;
}

auto TextEditor::redoTextEdit() -> bool {
    if (!this->canRedoTextEdit()) {
        return false;
    }

    ++this->textEditHistoryIndex;
    this->restoreTextEditState(this->textEditHistory[this->textEditHistoryIndex]);
    this->updateTextEditUndoActions();
    return true;
}

GtkTextTag* TextEditor::getFontTag(const XojFont& font) {
    const std::string key = font.asString();
    if (const auto it = this->fontTags.find(key); it != this->fontTags.end()) {
        return it->second;
    }

    GtkTextTag* tag = gtk_text_buffer_create_tag(this->buffer.get(), nullptr, "font", key.c_str(), nullptr);
    xoj_assert(tag != nullptr);
    this->fontTags.emplace(key, tag);
    return tag;
}

auto TextEditor::getFontAtIterator(const GtkTextIter& iter) const -> XojFont {
    GtkTextIter lookup = iter;
    if (gtk_text_iter_is_end(&lookup) && !gtk_text_iter_is_start(&lookup)) {
        gtk_text_iter_backward_char(&lookup);
    }

    XojFont result = this->textElement->getFont();
    GSList* tags = gtk_text_iter_get_tags(&lookup);
    for (GSList* item = tags; item != nullptr; item = item->next) {
        auto* tag = GTK_TEXT_TAG(item->data);
        bool recognized = false;
        for (const auto& [fontDescription, knownTag]: this->fontTags) {
            if (knownTag == tag) {
                result = XojFont(fontDescription.c_str());
                recognized = true;
                break;
            }
        }

        if (!recognized) {
            gchar* fontDescription = nullptr;
            g_object_get(tag, "font", &fontDescription, nullptr);
            if (fontDescription != nullptr) {
                XojFont tagFont(fontDescription);
                if (!tagFont.getName().empty() && std::isfinite(tagFont.getSize()) && tagFont.getSize() > 0) {
                    result = std::move(tagFont);
                }
                g_free(fontDescription);
            }
        }
    }
    g_slist_free(tags);
    return result;
}

void TextEditor::clearFontTags() {
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(this->buffer.get(), &start, &end);
    for (const auto& [fontDescription, tag]: this->fontTags) {
        gtk_text_buffer_remove_tag(this->buffer.get(), tag, &start, &end);
    }
}

void TextEditor::applyModelStylesToBuffer() {
    this->clearFontTags();

    for (const auto& run: this->textElement->getStyleRuns()) {
        GtkTextIter start = getIteratorAtByteOffset(this->buffer.get(), static_cast<int>(run.start));
        GtkTextIter end = getIteratorAtByteOffset(this->buffer.get(), static_cast<int>(run.end));
        gtk_text_buffer_apply_tag(this->buffer.get(), this->getFontTag(run.font), &start, &end);
    }
}

void TextEditor::applyTypingFont(size_t start, size_t end) {
    if (!this->typingFont || start >= end) {
        return;
    }

    GtkTextIter begin = getIteratorAtByteOffset(this->buffer.get(), static_cast<int>(start));
    GtkTextIter finish = getIteratorAtByteOffset(this->buffer.get(), static_cast<int>(end));
    gtk_text_buffer_apply_tag(this->buffer.get(), this->getFontTag(*this->typingFont), &begin, &finish);
}

auto TextEditor::getSelectionByteRange(size_t& start, size_t& end) const -> bool {
    GtkTextIter selectionStart;
    GtkTextIter selectionEnd;
    if (!gtk_text_buffer_get_selection_bounds(this->buffer.get(), &selectionStart, &selectionEnd)) {
        return false;
    }

    // GtkTextIter line indexes are byte indexes, but computing the absolute
    // position by walking lines is easy to get wrong at an end iterator. The
    // selection is formatted infrequently, so use the exact UTF-8 slices here
    // and keep the faster iterator conversion for the per-character editor
    // synchronization path.
    GtkTextIter bufferStart;
    gtk_text_buffer_get_start_iter(this->buffer.get(), &bufferStart);
    const auto beforeSelection =
            xoj::util::OwnedCString::assumeOwnership(gtk_text_iter_get_slice(&bufferStart, &selectionStart));
    const auto selectedText =
            xoj::util::OwnedCString::assumeOwnership(gtk_text_iter_get_slice(&selectionStart, &selectionEnd));
    const auto byteLength = [](const xoj::util::OwnedCString& text) -> size_t {
        return text.get() == nullptr ? 0 : std::strlen(text.get());
    };
    start = byteLength(beforeSelection);
    end = start + byteLength(selectedText);
    return start < end;
}

auto TextEditor::currentFontAttribute(bool bold) const -> bool {
    GtkTextIter start;
    GtkTextIter end;
    if (!gtk_text_buffer_get_selection_bounds(this->buffer.get(), &start, &end)) {
        const auto font = this->typingFont.value_or(this->getFontAtIterator(getIteratorAtCursor(this->buffer.get())));
        return bold ? Text::isBold(font) : Text::isItalic(font);
    }

    bool hasCharacters = false;
    bool allMatch = true;
    GtkTextIter iter = start;
    while (!gtk_text_iter_equal(&iter, &end)) {
        hasCharacters = true;
        const auto font = this->getFontAtIterator(iter);
        allMatch = allMatch && (bold ? Text::isBold(font) : Text::isItalic(font));

        if (!gtk_text_iter_forward_char(&iter)) {
            break;
        }
    }
    return hasCharacters && allMatch;
}

void TextEditor::updateFormattingActions() const {
    auto* db = this->control->getActionDatabase();
    db->setActionState(Action::TEXT_BOLD, this->currentFontAttribute(true));
    db->setActionState(Action::TEXT_ITALIC, this->currentFontAttribute(false));
}

void TextEditor::formatSelection(const std::function<void(size_t, size_t)>& formatter) {
    this->prepareTextEditHistory();

    size_t start = 0;
    size_t end = 0;
    if (!getSelectionByteRange(start, end)) {
        return;
    }

    this->updateTextElementContent();
    formatter(start, end);
    this->applyModelStylesToBuffer();
    this->typingFont.reset();
    this->updateFormattingActions();
    this->layoutStatus = LayoutStatus::NEEDS_COMPLETE_UPDATE;
    this->repaintEditor(true);
    this->recordTextEditHistory();
}

void TextEditor::setColor(Color color) {
    this->prepareTextEditHistory();
    this->textElement->setColor(color);
    repaintEditor(false);
    this->recordTextEditHistory();
}

void TextEditor::setFont(XojFont font) {
    this->prepareTextEditHistory();

    size_t start = 0;
    size_t end = 0;
    if (getSelectionByteRange(start, end)) {
        formatSelection([&](size_t selectionStart, size_t selectionEnd) {
            this->textElement->setFontRange(selectionStart, selectionEnd, font);
        });
        return;
    }

    this->textElement->setFont(font);
    this->typingFont = std::move(font);
    this->applyModelStylesToBuffer();
    this->updateFormattingActions();
    afterFontChange();
    this->recordTextEditHistory();
}

void TextEditor::setBold(bool bold) {
    this->prepareTextEditHistory();

    size_t start = 0;
    size_t end = 0;
    if (getSelectionByteRange(start, end)) {
        formatSelection([&](size_t selectionStart, size_t selectionEnd) {
            this->textElement->setBold(selectionStart, selectionEnd, bold);
        });
        return;
    }

    this->typingFont = Text::withBold(this->getFontAtIterator(getIteratorAtCursor(this->buffer.get())), bold);
    this->updateFormattingActions();
    this->recordTextEditHistory();
}

void TextEditor::setItalic(bool italic) {
    this->prepareTextEditHistory();

    size_t start = 0;
    size_t end = 0;
    if (getSelectionByteRange(start, end)) {
        formatSelection([&](size_t selectionStart, size_t selectionEnd) {
            this->textElement->setItalic(selectionStart, selectionEnd, italic);
        });
        return;
    }

    this->typingFont = Text::withItalic(this->getFontAtIterator(getIteratorAtCursor(this->buffer.get())), italic);
    this->updateFormattingActions();
    this->recordTextEditHistory();
}

void TextEditor::setFontSize(double size) {
    if (!std::isfinite(size) || size <= 0) {
        return;
    }

    this->prepareTextEditHistory();

    size_t start = 0;
    size_t end = 0;
    if (getSelectionByteRange(start, end)) {
        formatSelection([&](size_t selectionStart, size_t selectionEnd) {
            this->textElement->setFontSize(selectionStart, selectionEnd, size);
        });
        return;
    }

    this->typingFont = Text::withSize(this->getFontAtIterator(getIteratorAtCursor(this->buffer.get())), size);
    this->updateFormattingActions();
    this->recordTextEditHistory();
}

void TextEditor::adjustFontSize(double delta) {
    if (!std::isfinite(delta)) {
        return;
    }

    this->prepareTextEditHistory();

    size_t start = 0;
    size_t end = 0;
    if (getSelectionByteRange(start, end)) {
        formatSelection([&](size_t selectionStart, size_t selectionEnd) {
            this->textElement->adjustFontSize(selectionStart, selectionEnd, delta);
        });
        return;
    }

    const auto currentFont = this->getFontAtIterator(getIteratorAtCursor(this->buffer.get()));
    this->typingFont = Text::withSize(currentFont, std::max(1.0, currentFont.getSize() + delta));
    this->updateFormattingActions();
    this->recordTextEditHistory();
}

void TextEditor::setAlignment(TextAlignment al) {
    this->prepareTextEditHistory();
    this->textElement->setAlignment(al);
    this->layoutStatus = LayoutStatus::NEEDS_PARAMETERS_UPDATE;
    repaintEditor(true);  // The size may change if the text overflows
    this->recordTextEditHistory();
}

void TextEditor::setJustify(bool justify) {
    this->prepareTextEditHistory();
    this->textElement->setJustify(justify);
    this->layoutStatus = LayoutStatus::NEEDS_PARAMETERS_UPDATE;
    repaintEditor(true);
    this->recordTextEditHistory();
}

void TextEditor::afterFontChange() {
    this->textElement->updatePangoFont(this->layout.get());
    this->layoutStatus = LayoutStatus::NEEDS_COMPLETE_UPDATE;
    this->computeVirtualCursorPosition();
    this->repaintEditor();
}

void TextEditor::iMCommitCallback(GtkIMContext* context, const gchar* str, TextEditor* te) {
    te->beginTextEditHistoryGroup();
    gtk_text_buffer_begin_user_action(te->buffer.get());

    bool hadSelection = gtk_text_buffer_get_has_selection(te->buffer.get());
    if (hadSelection) {
        gtk_text_buffer_delete_selection(te->buffer.get(), true, true);
        te->control->setCopyCutEnabled(false);
    }

    const size_t insertionStart = static_cast<size_t>(getByteOffsetOfCursor(te->buffer.get()));
    bool inserted = false;

    if (!strcmp(str, "\n")) {
        if (!gtk_text_buffer_insert_interactive_at_cursor(te->buffer.get(), "\n", 1, true)) {
            gtk_widget_error_bell(te->xournalWidget);
        } else {
            inserted = true;
        }
    } else {
        if (!hadSelection && te->cursorOverwrite) {
            auto insert = getIteratorAtCursor(te->buffer.get());
            if (!gtk_text_iter_ends_line(&insert)) {
                te->deleteFromCursor(GTK_DELETE_CHARS, 1);
            }
        }

        if (!gtk_text_buffer_insert_interactive_at_cursor(te->buffer.get(), str, -1, true)) {
            gtk_widget_error_bell(te->xournalWidget);
        } else {
            inserted = true;
        }
    }

    if (inserted) {
        te->applyTypingFont(insertionStart, insertionStart + strlen(str));
    }

    gtk_text_buffer_end_user_action(te->buffer.get());
    te->contentsChanged();
    te->endTextEditHistoryGroup();
    te->repaintEditor();
}

void TextEditor::iMPreeditChangedCallback(GtkIMContext* context, TextEditor* te) {
    xoj::util::OwnedCString str;
    gint cursor_pos = 0;
    GtkTextIter iter = getIteratorAtCursor(te->buffer.get());

    {
        PangoAttrList* attrs = nullptr;
        gtk_im_context_get_preedit_string(context, str.contentReplacer(), &attrs, &cursor_pos);
        if (attrs == nullptr) {
            attrs = pango_attr_list_new();
        }
        te->preeditAttrList.reset(attrs, xoj::util::adopt);
    }

    if (str && str[0] && !gtk_text_iter_can_insert(&iter, true)) {
        /*
         * Keypress events are passed to input method even if cursor position is
         * not editable; so beep here if it's multi-key input sequence, input
         * method will be reset in key-press-event handler.
         */
        gtk_widget_error_bell(te->xournalWidget);
        return;
    }

    te->preeditString = std::move(str);
    te->preeditCursor = cursor_pos;
    te->contentsChanged();
    te->repaintEditor();
}

auto TextEditor::iMRetrieveSurroundingCallback(GtkIMContext* context, TextEditor* te) -> bool {
    GtkTextIter start = getIteratorAtCursor(te->buffer.get());
    GtkTextIter end = start;

    gint pos = gtk_text_iter_get_line_index(&start);
    gtk_text_iter_set_line_offset(&start, 0);
    gtk_text_iter_forward_to_line_end(&end);

    auto text = xoj::util::OwnedCString::assumeOwnership(gtk_text_iter_get_slice(&start, &end));
    gtk_im_context_set_surrounding(context, text.get(), -1, pos);

    return true;
}

auto TextEditor::imDeleteSurroundingCallback(GtkIMContext* context, gint offset, gint n_chars, TextEditor* te) -> bool {
    te->prepareTextEditHistory();
    GtkTextIter start = getIteratorAtCursor(te->buffer.get());
    GtkTextIter end = start;

    gtk_text_iter_forward_chars(&start, offset);
    gtk_text_iter_forward_chars(&end, offset + n_chars);

    gtk_text_buffer_delete_interactive(te->buffer.get(), &start, &end, true);

    te->contentsChanged();
    te->repaintEditor();

    return true;
}

auto TextEditor::onKeyPressEvent(const KeyEvent& event) -> bool {

    // IME needs to handle the input first so the candidate window works correctly
    if (gtk_im_context_filter_keypress(this->imContext.get(), event.sourceEvent)) {
        this->needImReset = true;

        GtkTextIter iter = getIteratorAtCursor(this->buffer.get());
        bool canInsert = gtk_text_iter_can_insert(&iter, true);

        if (canInsert) {
            control->getCursor()->setInvisible(true);
        } else {
            this->resetImContext();
        }
        return true;
    }

    return keyBindings.processEvent(this, event);
}

auto TextEditor::onKeyReleaseEvent(const KeyEvent& event) -> bool {
    GtkTextIter iter = getIteratorAtCursor(this->buffer.get());

    if (gtk_text_iter_can_insert(&iter, true) &&
        gtk_im_context_filter_keypress(this->imContext.get(), event.sourceEvent)) {
        this->needImReset = true;
        return true;
    }
    return false;
}

void TextEditor::toggleOverwrite() {
    this->cursorOverwrite = !this->cursorOverwrite;
    repaintCursorAfterChange();
}

/**
 * I know it's a bit rough and duplicated
 * Improve that later on...
 */
void TextEditor::decreaseFontSize() {
    adjustFontSize(-1);
}

void TextEditor::increaseFontSize() {
    adjustFontSize(1);
}

void TextEditor::toggleBoldFace() {
    setBold(!currentFontAttribute(true));
}

void TextEditor::toggleItalicFace() {
    setItalic(!currentFontAttribute(false));
}

void TextEditor::selectAtCursor(TextEditor::SelectType ty) {
    const auto searchFlag = GTK_TEXT_SEARCH_TEXT_ONLY;  // To be used to find double newlines

    // Start from the insertion mark rather than from the current selection
    // bounds.  The latter are not useful for a collapsed selection and, more
    // importantly, can leave the end iterator one character short when a
    // double-click lands at the end of a word.
    GtkTextIter startPos = getIteratorAtCursor(this->buffer.get());
    GtkTextIter endPos = startPos;

    switch (ty) {
        case TextEditor::SelectType::WORD: {
            auto currentPos = getIteratorAtCursor(this->buffer.get());
            if (!gtk_text_iter_inside_word(&currentPos)) {
                // A cursor at the buffer end is not considered "inside" the
                // preceding word by GTK, although it is a valid double-click
                // position for the word immediately before it.
                auto previousPos = currentPos;
                if (gtk_text_iter_backward_char(&previousPos) && gtk_text_iter_inside_word(&previousPos)) {
                    currentPos = previousPos;
                } else {
                    // Do nothing if the cursor is over whitespace.
                    return;
                }
            }

            startPos = currentPos;
            endPos = currentPos;
            gtk_text_iter_backward_word_start(&startPos);
            gtk_text_iter_forward_word_end(&endPos);
            break;
        }
        case TextEditor::SelectType::PARAGRAPH:
            gtk_text_buffer_get_selection_bounds(this->buffer.get(), &startPos, &endPos);
            // Note that a GTK "paragraph" is a line, so there's no nice one-liner.
            // We define a paragraph as text separated by double newlines.
            while (!gtk_text_iter_is_start(&startPos)) {
                // There's no GTK function to go to line start, so do it manually.
                while (!gtk_text_iter_starts_line(&startPos)) {
                    if (!gtk_text_iter_backward_word_start(&startPos)) {
                        break;
                    }
                }
                // Check for paragraph start
                GtkTextIter searchPos = startPos;
                gtk_text_iter_backward_chars(&searchPos, 2);
                if (gtk_text_iter_backward_search(&startPos, "\n\n", searchFlag, nullptr, nullptr, &searchPos)) {
                    break;
                }
                gtk_text_iter_backward_line(&startPos);
            }
            while (!gtk_text_iter_ends_line(&endPos)) {
                gtk_text_iter_forward_to_line_end(&endPos);
                // Check for paragraph end
                GtkTextIter searchPos = endPos;
                gtk_text_iter_forward_chars(&searchPos, 2);
                if (gtk_text_iter_forward_search(&endPos, "\n\n", searchFlag, nullptr, nullptr, &searchPos)) {
                    break;
                }
                gtk_text_iter_forward_line(&endPos);
            }
            break;
        case TextEditor::SelectType::ALL:
            gtk_text_buffer_get_bounds(this->buffer.get(), &startPos, &endPos);
            break;
    }

    gtk_text_buffer_select_range(this->buffer.get(), &startPos, &endPos);

    control->setCopyCutEnabled(gtk_text_buffer_get_has_selection(this->buffer.get()));

    // Selection highlighting is handled through Pango attributes
    this->layoutStatus = LayoutStatus::NEEDS_ATTRIBUTES_UPDATE;
    this->repaintEditor(false);
    this->updateFormattingActions();
}

void TextEditor::moveCursor(GtkMovementStep step, int count, bool extendSelection) {
    resetImContext();

    GtkTextIter insert = getIteratorAtCursor(this->buffer.get());
    GtkTextIter newplace = insert;

    bool updateVirtualCursor = true;

    switch (step) {
        case GTK_MOVEMENT_LOGICAL_POSITIONS:  // not used!?
            gtk_text_iter_forward_visible_cursor_positions(&newplace, count);
            break;
        case GTK_MOVEMENT_VISUAL_POSITIONS:
            if (count < 0) {
                gtk_text_iter_backward_cursor_position(&newplace);
            } else {
                gtk_text_iter_forward_cursor_position(&newplace);
            }
            break;

        case GTK_MOVEMENT_WORDS:
            if (count < 0) {
                gtk_text_iter_backward_visible_word_starts(&newplace, -count);
            } else if (count > 0) {
                if (!gtk_text_iter_forward_visible_word_ends(&newplace, count)) {
                    gtk_text_iter_forward_to_line_end(&newplace);
                }
            }
            break;

        case GTK_MOVEMENT_DISPLAY_LINES:
            updateVirtualCursor = false;
            jumpALine(&newplace, count);
            break;

        case GTK_MOVEMENT_PARAGRAPHS:
            if (count > 0) {
                if (!gtk_text_iter_ends_line(&newplace)) {
                    gtk_text_iter_forward_to_line_end(&newplace);
                    --count;
                }
                gtk_text_iter_forward_visible_lines(&newplace, count);
                gtk_text_iter_forward_to_line_end(&newplace);
            } else if (count < 0) {
                if (gtk_text_iter_get_line_offset(&newplace) > 0) {
                    gtk_text_iter_set_line_offset(&newplace, 0);
                }
                gtk_text_iter_forward_visible_lines(&newplace, count);
                gtk_text_iter_set_line_offset(&newplace, 0);
            }
            break;

        case GTK_MOVEMENT_DISPLAY_LINE_ENDS:
        case GTK_MOVEMENT_PARAGRAPH_ENDS:
            if (count > 0) {
                if (!gtk_text_iter_ends_line(&newplace)) {
                    gtk_text_iter_forward_to_line_end(&newplace);
                }
            } else if (count < 0) {
                gtk_text_iter_set_line_offset(&newplace, 0);
            }
            break;

        case GTK_MOVEMENT_BUFFER_ENDS:
            if (count > 0) {
                gtk_text_buffer_get_end_iter(this->buffer.get(), &newplace);
            } else if (count < 0) {
                gtk_text_buffer_get_iter_at_offset(this->buffer.get(), &newplace, 0);
            }
            break;

        default:
            break;
    }

    // call moveCursorIterator() even if the cursor hasn't moved, since it cancels the selection
    moveCursorIterator(&newplace, extendSelection);

    if (updateVirtualCursor) {
        computeVirtualCursorPosition();
    }

    if (gtk_text_iter_equal(&insert, &newplace)) {
        gtk_widget_error_bell(this->xournalWidget);
    }
}

void TextEditor::findPos(GtkTextIter* iter, double xPos, double yPos) const {
    int index = 0;
    int trailing = 0;
    const bool insideLayout = pango_layout_xy_to_index(this->getUpToDateLayout(), round_cast<int>(xPos * PANGO_SCALE),
                                                       round_cast<int>(yPos * PANGO_SCALE), &index, &trailing);
    if (!insideLayout) {
        // Pango reports a miss when the mouse is just beyond the final glyph.
        // Do not reuse its unspecified index/trailing outputs: selecting by
        // dragging past the last glyph must produce the buffer end iterator.
        if (xPos <= 0) {
            gtk_text_buffer_get_start_iter(this->buffer.get(), iter);
        } else {
            gtk_text_buffer_get_end_iter(this->buffer.get(), iter);
        }
        return;
    }
    /*
     * trailing is non-zero iff the abscissa is past the middle of the grapheme.
     * In this case, it contains the length of the grapheme in utf8 char count.
     * This way, we put the cursor after the grapheme when clicking past the middle of the grapheme.
     */
    *iter = getIteratorAtByteOffset(this->buffer.get(), index);
    gtk_text_iter_forward_chars(iter, trailing);
}

void TextEditor::updateTextElementContent() {
    const std::string content = cloneToStdString(this->buffer.get());
    this->textElement->setText(content);

    std::vector<Text::StyleRun> runs;
    GtkTextIter iter;
    GtkTextIter end;
    gtk_text_buffer_get_start_iter(this->buffer.get(), &iter);
    gtk_text_buffer_get_end_iter(this->buffer.get(), &end);

    while (!gtk_text_iter_equal(&iter, &end)) {
        GtkTextIter next = iter;
        // GTK returns false when moving from the final character to the end
        // iterator, but the iterator is still advanced.  Do not discard that
        // final character: its tag is exactly the state we need to preserve
        // when synchronizing inline styles back to the model.
        gtk_text_iter_forward_char(&next);
        if (gtk_text_iter_equal(&next, &iter)) {
            break;
        }

        const XojFont currentFont = getFontAtIterator(iter);
        const auto& defaultFont = this->textElement->getFont();
        if (currentFont.getName() != defaultFont.getName() || currentFont.getSize() != defaultFont.getSize()) {
            const size_t start = static_cast<size_t>(getByteOffsetOfIterator(iter));
            const size_t finish = static_cast<size_t>(getByteOffsetOfIterator(next));
            if (!runs.empty() && runs.back().end == start &&
                runs.back().font.getName() == currentFont.getName() &&
                runs.back().font.getSize() == currentFont.getSize()) {
                runs.back().end = finish;
            } else {
                runs.push_back({start, finish, currentFont});
            }
        }

        iter = next;
    }

    this->textElement->setStyleRuns(std::move(runs));
}

void TextEditor::contentsChanged(bool forceCreateUndoAction) {
    (void)forceCreateUndoAction;
    this->updateTextElementContent();
    this->updateFormattingActions();
    this->layoutStatus = LayoutStatus::NEEDS_COMPLETE_UPDATE;
    this->computeVirtualCursorPosition();
    if (this->textEditHistoryGroupDepth == 0) {
        this->recordTextEditHistory();
    }
}

void TextEditor::markPos(double x, double y, bool extendSelection) {
    GtkTextIter newplace = getIteratorAtCursor(this->buffer.get());

    findPos(&newplace, x, y);

    // call moveCursorIterator() even if the cursor hasn't moved, since it cancels the selection
    moveCursorIterator(&newplace, extendSelection);
    computeVirtualCursorPosition();
}

void TextEditor::mousePressed(double x, double y) {
    this->mouseDown = true;
    // Todo select if SHIFT is pressed
    const auto& origin = textElement->getOrigin();
    this->markPos(x - origin.x, y - origin.y, false);
}

void TextEditor::mouseMoved(double x, double y) {
    if (this->mouseDown) {
        const auto& origin = textElement->getOrigin();
        this->markPos(x - origin.x, y - origin.y, true);
    }
}

void TextEditor::mouseReleased() { this->mouseDown = false; }

void TextEditor::jumpALine(GtkTextIter* textIter, int count) {
    count += this->virtualCursorPosition.pangoLineNumber;
    if (count < 0) {
        return;
    }

    PangoLayoutLine* line = pango_layout_get_line_readonly(this->layout.get(), count);
    if (line == nullptr) {
        return;
    }
    this->virtualCursorPosition.pangoLineNumber = count;

    int index = 0;
    int trailing = 0;
    pango_layout_line_x_to_index(line, this->virtualCursorPosition.abscissa, &index, &trailing);
    /*
     * trailing is non-zero iff the abscissa is past the middle of the grapheme.
     * In this case, it contains the length of the grapheme in utf8 char count.
     */
    *textIter = getIteratorAtByteOffset(this->buffer.get(), index);
    gtk_text_iter_forward_chars(textIter, trailing);
}

void TextEditor::computeVirtualCursorPosition() {
    int offset = getByteOffsetOfCursor(this->buffer.get());

    pango_layout_index_to_line_x(this->getUpToDateLayout(), offset, 0, &this->virtualCursorPosition.pangoLineNumber,
                                 &this->virtualCursorPosition.abscissa);
}

void TextEditor::moveCursorIterator(const GtkTextIter* newLocation, gboolean extendSelection) {
    bool selectionChanged = true;
    if (extendSelection) {
        if (auto oldLoc = getIteratorAtCursor(this->buffer.get()); gtk_text_iter_equal(newLocation, &oldLoc)) {
            // Nothing changed
            return;
        }
        gtk_text_buffer_move_mark_by_name(this->buffer.get(), "insert", newLocation);
        control->setCopyCutEnabled(gtk_text_buffer_get_has_selection(this->buffer.get()));
    } else {
        // if !extendSelection, we clear the selection even if the cursor does not move
        selectionChanged = gtk_text_buffer_get_has_selection(this->buffer.get());
        gtk_text_buffer_place_cursor(this->buffer.get(), newLocation);
        control->setCopyCutEnabled(false);
    }

    if (this->cursorBlink) {
        // Whenever the cursor moves, the blinking cycle restarts from the start (i.e. the cursor is first shown).
        this->cursorVisible = false;  // Will be toggled to true by BlinkTimer::callback before the repaint
        blinkCallback(this);
    }

    if (selectionChanged) {
        // The selection background color is set through Pango attributes
        this->layoutStatus = LayoutStatus::NEEDS_ATTRIBUTES_UPDATE;
        // Repaint the entire box. Computing the exact area that was (un)selected would be better but complicated
        this->repaintEditor(false);
    } else {
        repaintCursorAfterChange();
    }
    this->updateFormattingActions();
}

void TextEditor::updateCursorBox() {
    this->cursorBox = computeCursorBox();

    if (!viewPool->empty()) {
        // Inform the IM of the cursor location (for word selection popup's location)
        // We use the first view as the main view, as far as the IM is concerned
        const auto& origin = textElement->getOrigin();
        auto box = viewPool->front().toWidgetCoordinates(
                xoj::util::Rectangle<double>(this->cursorBox).translated(origin.x, origin.y));

        GdkRectangle cursorRect;  // cursor position in window coordinates
        cursorRect.x = static_cast<int>(box.x);
        cursorRect.y = static_cast<int>(box.y);
        cursorRect.height = static_cast<int>(box.height);
        cursorRect.width = static_cast<int>(box.width);
        gtk_im_context_set_cursor_location(this->imContext.get(), &cursorRect);
    }
}

void TextEditor::updateDraggableIcons() const {
    if (!viewPool->empty()) {
        // We use the first view as the main view
        Range range = this->getContentBoundingBox();
        range.minX = textElement->getSnappedBounds().x;
        auto box = viewPool->front().toWidgetCoordinates(xoj::util::Rectangle<double>(range));
        auto zoom = viewPool->front().getZoom();
        double extendIconPos = this->currentWrapWidth == Text::NO_WRAP ? box.width : this->currentWrapWidth * zoom;
        moveIcon->setPosition({floor_cast<int>(box.x), floor_cast<int>(box.y)});
        extendIcon->setPosition({ceil_cast<int>(box.x + extendIconPos), floor_cast<int>(box.y)});
    }
}

static auto whitespace(gunichar ch, gpointer user_data) -> gboolean { return (ch == ' ' || ch == '\t'); }

static auto not_whitespace(gunichar ch, gpointer user_data) -> gboolean { return !whitespace(ch, user_data); }

static auto find_whitepace_region(const GtkTextIter* center, GtkTextIter* start, GtkTextIter* end) -> gboolean {
    *start = *center;
    *end = *center;

    if (gtk_text_iter_backward_find_char(start, not_whitespace, nullptr, nullptr)) {
        gtk_text_iter_forward_char(start); /* we want the first whitespace... */
    }
    if (whitespace(gtk_text_iter_get_char(end), nullptr)) {
        gtk_text_iter_forward_find_char(end, not_whitespace, nullptr, nullptr);
    }

    return !gtk_text_iter_equal(start, end);
}

void TextEditor::deleteFromCursor(GtkDeleteType type, int count) {

    this->prepareTextEditHistory();
    this->resetImContext();

    if (type == GTK_DELETE_CHARS) {
        // Char delete deletes the selection, if one exists
        if (gtk_text_buffer_delete_selection(this->buffer.get(), true, true)) {
            control->setCopyCutEnabled(false);
            this->contentsChanged(true);
            this->repaintEditor();
            return;
        }
    }

    GtkTextIter insert = getIteratorAtCursor(this->buffer.get());

    GtkTextIter start = insert;
    GtkTextIter end = insert;

    switch (type) {
        case GTK_DELETE_CHARS:
            gtk_text_iter_forward_cursor_positions(&end, count);
            break;

        case GTK_DELETE_WORD_ENDS:
            if (count > 0) {
                gtk_text_iter_forward_word_ends(&end, count);
            } else if (count < 0) {
                gtk_text_iter_backward_word_starts(&start, 0 - count);
            }
            break;

        case GTK_DELETE_WORDS:
            break;

        case GTK_DELETE_DISPLAY_LINE_ENDS:
            break;

        case GTK_DELETE_DISPLAY_LINES:
            break;

        case GTK_DELETE_PARAGRAPH_ENDS:
            if (count > 0) {
                /* If we're already at a newline, we need to
                 * simply delete that newline, instead of
                 * moving to the next one.
                 */
                if (gtk_text_iter_ends_line(&end)) {
                    gtk_text_iter_forward_line(&end);
                    --count;
                }

                while (count > 0) {
                    if (!gtk_text_iter_forward_to_line_end(&end)) {
                        break;
                    }

                    --count;
                }
            } else if (count < 0) {
                if (gtk_text_iter_starts_line(&start)) {
                    gtk_text_iter_backward_line(&start);
                    if (!gtk_text_iter_ends_line(&end)) {
                        gtk_text_iter_forward_to_line_end(&start);
                    }
                } else {
                    gtk_text_iter_set_line_offset(&start, 0);
                }
                ++count;

                gtk_text_iter_backward_lines(&start, -count);
            }
            break;

        case GTK_DELETE_PARAGRAPHS:
            if (count > 0) {
                gtk_text_iter_set_line_offset(&start, 0);
                gtk_text_iter_forward_to_line_end(&end);

                /* Do the lines beyond the first. */
                while (count > 1) {
                    gtk_text_iter_forward_to_line_end(&end);
                    --count;
                }
            }

            break;

        case GTK_DELETE_WHITESPACE: {
            find_whitepace_region(&insert, &start, &end);
        } break;

        default:
            break;
    }

    if (!gtk_text_iter_equal(&start, &end)) {
        gtk_text_buffer_begin_user_action(this->buffer.get());

        if (!gtk_text_buffer_delete_interactive(this->buffer.get(), &start, &end, true)) {
            gtk_widget_error_bell(this->xournalWidget);
        }

        gtk_text_buffer_end_user_action(this->buffer.get());
    } else {
        gtk_widget_error_bell(this->xournalWidget);
    }

    this->contentsChanged();
    this->repaintEditor();
}

void TextEditor::backspace() {

    this->prepareTextEditHistory();
    resetImContext();

    // Backspace deletes the selection, if one exists
    if (gtk_text_buffer_delete_selection(this->buffer.get(), true, true)) {
        control->setCopyCutEnabled(false);
        this->contentsChanged();
        this->repaintEditor();
        return;
    }

    GtkTextIter insert = getIteratorAtCursor(this->buffer.get());

    if (gtk_text_buffer_backspace(this->buffer.get(), &insert, true, true)) {
        this->contentsChanged();
        this->repaintEditor();
    } else {
        gtk_widget_error_bell(this->xournalWidget);
    }
}

void TextEditor::linebreak() {
    this->resetImContext();
    iMCommitCallback(nullptr, "\n", this);

    control->getCursor()->setInvisible(true);
}

void TextEditor::tabulation() {
    resetImContext();
    Settings* settings = control->getSettings();
    if (!settings->getUseSpacesAsTab()) {
        iMCommitCallback(nullptr, "\t", this);
    } else {
        std::string indent(static_cast<size_t>(settings->getNumberOfSpacesForTab()), ' ');
        iMCommitCallback(nullptr, indent.c_str(), this);
    }

    control->getCursor()->setInvisible(true);
}


void TextEditor::copyToClipboard() const {
    auto* clipboard = gtk_widget_get_clipboard(this->xournalWidget);
    gtk_text_buffer_copy_clipboard(this->buffer.get(), clipboard);
}

void TextEditor::cutToClipboard() {
    this->prepareTextEditHistory();
    auto* clipboard = gtk_widget_get_clipboard(this->xournalWidget);
    gtk_text_buffer_cut_clipboard(this->buffer.get(), clipboard, true);

    this->contentsChanged(true);
    this->repaintEditor();
}

void TextEditor::pasteFromClipboard() {
    this->beginTextEditHistoryGroup();
    auto* clipboard = gtk_widget_get_clipboard(this->xournalWidget);
    gtk_text_buffer_paste_clipboard(this->buffer.get(), clipboard, nullptr, true);
}

void TextEditor::bufferPasteDoneCallback(GtkTextBuffer* buffer, GtkClipboard* clipboard, TextEditor* te) {
    te->contentsChanged(true);
    te->repaintEditor();

    if (te->textElement->getWrap() == Text::NO_WRAP && te->getContentBoundingBox().maxX > te->page->getWidth()) {
        te->textElement->setWrap(te->page->getWidth() - te->getContentBoundingBox().minX);
        te->currentWrapWidth = te->textElement->getWrap();
        te->layoutStatus = LayoutStatus::NEEDS_PARAMETERS_UPDATE;
        te->repaintEditor(true);
    }

    te->endTextEditHistoryGroup();
}

void TextEditor::resetImContext() {
    if (this->needImReset) {
        this->needImReset = false;
        gtk_im_context_reset(this->imContext.get());
    }
}

/*
 * Blink!
 */
void TextEditor::blinkCallback(TextEditor* te) {
    te->cursorVisible = !te->cursorVisible;
    auto time = te->cursorVisible ? te->cursorBlinkingTimeOn : te->cursorBlinkingTimeOff;
    te->blinkTimer = g_timeout_add(time, xoj::util::wrap_for_once_v<blinkCallback>, te);

    Range dirtyRange = te->cursorBox;
    const auto& origin = te->textElement->getOrigin();
    dirtyRange.translate(origin.x, origin.y);
    te->viewPool->dispatch(xoj::view::TextEditionView::FLAG_DIRTY_REGION, dirtyRange);
}

void TextEditor::setTextToPangoLayout(PangoLayout* pl) const {
    std::string_view preed(preeditString);

    if (!preed.empty()) {
        // When using an Input Method, we need to insert the preeditString into the text at the cursor location
        std::string txt = cloneWithInsertToStdString(this->buffer.get(), preed);

        int pos = getByteOffsetOfCursor(this->buffer.get());
        auto attrlist = this->textElement->createPangoAttrList();
        pango_attr_list_splice(attrlist.get(), this->preeditAttrList.get(), pos, static_cast<int>(preed.length()));

        pango_layout_set_text(pl, txt.c_str(), static_cast<int>(txt.length()));
        pango_layout_set_attributes(pl, attrlist.get());
    } else {
        setSelectionAttributesToPangoLayout(pl);
        pango_layout_set_text(pl, cloneToCString(this->buffer.get()).get(), -1);
    }
}

Color TextEditor::getSelectionColor() const { return this->control->getSettings()->getSelectionColor(); }

void TextEditor::setSelectionAttributesToPangoLayout(PangoLayout* pl) const {
    auto attrlist = this->textElement->createPangoAttrList();

    GtkTextIter start;
    GtkTextIter end;
    bool hasSelection = gtk_text_buffer_get_selection_bounds(this->buffer.get(), &start, &end);

    if (hasSelection) {
        auto selectionColorU16 = Util::argb_to_ColorU16(this->getSelectionColor());
        PangoAttribute* attrib =
                pango_attr_background_new(selectionColorU16.red, selectionColorU16.green, selectionColorU16.blue);
        attrib->start_index = static_cast<unsigned int>(getByteOffsetOfIterator(start));
        attrib->end_index = static_cast<unsigned int>(getByteOffsetOfIterator(end));

        pango_attr_list_insert(attrlist.get(), attrib);  // attrlist takes ownership of attrib
    }

    pango_layout_set_attributes(pl, attrlist.get());
}

auto TextEditor::computeBoundingBox() const -> Range {
    /*
     * NB: we cannot rely on Text::calcSize directly, since it would not take the size changes due to the IM
     * preeditString into account.
     */
    auto boxes = Text::computeBoxesForLayout(getUpToDateLayout(), textElement->getOrigin(), this->currentWrapWidth);
    return Range(boxes.bounds);
}

auto TextEditor::getUpToDateLayout() const -> PangoLayout* {
    switch (layoutStatus) {
        case LayoutStatus::NEEDS_COMPLETE_UPDATE:
            setTextToPangoLayout(this->layout.get());
            break;
        case LayoutStatus::NEEDS_ATTRIBUTES_UPDATE:
            setSelectionAttributesToPangoLayout(this->layout.get());
            break;
        case LayoutStatus::NEEDS_PARAMETERS_UPDATE:
            pango_layout_set_width(this->layout.get(), round_cast<int>(this->currentWrapWidth * PANGO_SCALE));
            pango_layout_set_justify(layout.get(), this->textElement->getJustify());
            pango_layout_set_alignment(layout.get(), this->textElement->getAlign().toPango());
            break;
        case LayoutStatus::UP_TO_DATE:
            break;
    }
    layoutStatus = LayoutStatus::UP_TO_DATE;
    return this->layout.get();
}

auto TextEditor::getCursorBox() const -> const Range& { return this->cursorBox; }

auto TextEditor::getContentBoundingBox() const -> const Range& { return this->previousBoundingBox; }

bool TextEditor::isCursorVisible() const { return cursorVisible; }

auto TextEditor::computeCursorBox() const -> Range {
    // Compute the bounding box of the active grapheme (i.e. the one just after the cursor)
    int offset = getByteOffsetOfCursor(this->buffer.get());
    if (this->preeditString && this->preeditCursor != 0) {
        const gchar* preeditText = this->preeditString.get();
        offset += static_cast<int>(g_utf8_offset_to_pointer(preeditText, preeditCursor) - preeditText);
    }
    PangoRectangle rect = {0};
    pango_layout_index_to_pos(getUpToDateLayout(), offset, &rect);
    const double ratio = 1.0 / PANGO_SCALE;

    // Warning: rect.width could be negative (e.g. for languages written from right to left).
    Range res(rect.x * ratio, rect.y * ratio);
    res.addPoint((rect.x + (cursorOverwrite ? rect.width : 0.0)) * ratio, (rect.y + rect.height) * ratio);
    return res;
}

void TextEditor::repaintEditor(bool sizeChanged) {
    Range dirtyRange(this->previousBoundingBox);
    if (sizeChanged) {
        this->previousBoundingBox = this->computeBoundingBox();
        dirtyRange = dirtyRange.unite(this->previousBoundingBox);
    }
    this->updateCursorBox();
    this->updateDraggableIcons();
    this->viewPool->dispatch(xoj::view::TextEditionView::FLAG_DIRTY_REGION, dirtyRange);
}

void TextEditor::repaintCursorAfterChange() {
    Range dirtyRange = this->cursorBox;
    this->updateCursorBox();
    dirtyRange = dirtyRange.unite(this->cursorBox);
    const auto& origin = this->textElement->getOrigin();

    dirtyRange.translate(origin.x, origin.y);
    this->viewPool->dispatch(xoj::view::TextEditionView::FLAG_DIRTY_REGION, dirtyRange);
}

void TextEditor::finalizeEdition() {

    auto* db = this->control->getActionDatabase();
    auto* th = this->control->getToolHandler();
    db->setActionState(Action::FONT, this->control->getSettings()->getFont().asString().c_str());
    db->setActionState(Action::TEXT_ALIGNMENT, th->getTextAlignment());
    db->setActionState(Action::TEXT_JUSTIFY, th->getTextJustify());
    db->setActionState(Action::TOOL_COLOR, th->getColorMaskAlpha());

    auto* doc = this->control->getDocument();
    UndoRedoHandler* undo = this->control->getUndoRedoHandler();

    if (this->bufferEmpty()) {
        // Delete the edited element from layer
        if (originalTextElement) {
            auto eraseDeleteUndoAction = std::make_unique<DeleteUndoAction>(page, true);
            doc->lock();
            Layer* layer = this->page->getSelectedLayer();
            auto [orig, elementIndex] = layer->removeElement(originalTextElement);
            doc->unlock();
            if (elementIndex != Element::InvalidIndex) [[likely]] {
                eraseDeleteUndoAction->addElement(layer, std::move(orig), elementIndex);
                undo->addUndoAction(std::move(eraseDeleteUndoAction));
            }  // A warning has already been issued otherwise
            originalTextElement = nullptr;
        }
        this->viewPool->dispatchAndClear(xoj::view::TextEditionView::FINALIZATION_REQUEST, this->previousBoundingBox);
        return;
    }

    this->updateTextElementContent();
    if (originalTextElement) {
        // Modifying a preexisting element
        this->viewPool->dispatchAndClear(xoj::view::TextEditionView::FINALIZATION_REQUEST, this->previousBoundingBox);

        doc->lock();
        Layer* layer = this->page->getSelectedLayer();
        auto [orig, _] = layer->removeElement(this->originalTextElement);
        auto ptr = this->textElement.get();
        layer->addElement(std::move(this->textElement));
        doc->unlock();

        this->page->fireElementChanged(ptr);

        if (orig) [[likely]] {
            xoj_assert(orig.get() == this->originalTextElement);
            this->originalTextElement->setInEditing(false);
            undo->addUndoAction(std::make_unique<TextBoxUndoAction>(this->page, layer, ptr, std::move(orig)));
        } else {
            // A warning has already been issued
            undo->addUndoAction(std::make_unique<InsertUndoAction>(this->page, layer, ptr));
        }
        originalTextElement = nullptr;
    } else {
        // Creating a new element
        auto ptr = this->textElement.get();
        doc->lock();
        Layer* layer = this->page->getSelectedLayer();
        layer->addElement(std::move(this->textElement));
        doc->unlock();
        this->viewPool->dispatchAndClear(xoj::view::TextEditionView::FINALIZATION_REQUEST, this->previousBoundingBox);
        this->page->fireElementChanged(ptr);
        undo->addUndoAction(std::make_unique<InsertUndoAction>(page, layer, ptr));
    }
}

void TextEditor::initializeEditionAt(double x, double y) {
    // Is there already a textfield?
    Text* text = nullptr;
    std::shared_lock lock(*this->control->getDocument());

    // Should we reverse this loop to select the most recent text rather than the oldest?
    for (auto&& e: this->page->getSelectedLayer()->getElements()) {
        if (e->getType() == ELEMENT_TEXT && e->hasBoundingBoxContaining(x, y)) {
            text = dynamic_cast<Text*>(e.get());
            break;
        }
    }

    if (text == nullptr) {
        lock.unlock();
        ToolHandler* h = this->control->getToolHandler();
        this->textElement = std::make_unique<Text>();
        this->textElement->setColor(h->getColor());
        this->textElement->setFont(control->getSettings()->getFont());
        this->textElement->setOrigin(x, y - this->textElement->getBoundingBox().height / 2);
        this->textElement->setAlignment(h->getTextAlignment());
        this->textElement->setJustify(h->getTextJustify());

#ifdef ENABLE_AUDIO
        if (auto audioController = control->getAudioController(); audioController && audioController->isRecording()) {
            fs::path audioFilename = audioController->getAudioFilename();
            size_t sttime = audioController->getStartTime();
            size_t milliseconds = (as_unsigned(g_get_monotonic_time() / 1000) - sttime);
            this->textElement->setTimestamp(milliseconds);
            this->textElement->setAudioFilename(audioFilename);
        }
#endif
        this->originalTextElement = nullptr;
    } else {
        this->originalTextElement = text;
        this->textElement = text->cloneText();
        text->setInEditing(true);
        lock.unlock();

        auto* db = this->control->getActionDatabase();
        db->setActionState(Action::FONT, this->textElement->getFont().asString().c_str());
        db->setActionState(Action::TEXT_ALIGNMENT, this->textElement->getAlign());
        db->setActionState(Action::TEXT_JUSTIFY, this->textElement->getJustify());
        Color c = this->textElement->getColor();
        c.alpha = 0xff;
        db->setActionState(Action::TOOL_COLOR, c);

        this->page->fireElementChanged(text);
    }
    this->currentWrapWidth = this->textElement->getWrap();
    this->layout = this->textElement->createPangoLayout();
    this->replaceBufferContent(this->textElement->getText());
    this->previousBoundingBox = this->computeBoundingBox();
}
