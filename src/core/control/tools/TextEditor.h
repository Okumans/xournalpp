/*
 * Xournal++
 *
 * Text editor gui (for Text Tool)
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>  // for string
#include <unordered_map>
#include <vector>

#include <gdk/gdk.h>      // for GdkEventKey
#include <glib.h>         // for gint, gboolean, gchar
#include <gtk/gtk.h>      // for GtkIMContext, GtkTextIter, GtkWidget
#include <pango/pango.h>  // for PangoAttrList, PangoLayout

#include "model/Font.h"
#include "model/OverlayBase.h"
#include "model/PageRef.h"  // for PageRef
#include "util/Color.h"     // for Color
#include "util/Range.h"
#include "util/raii/CStringWrapper.h"
#include "util/raii/GObjectSPtr.h"
#include "util/raii/GSourceURef.h"
#include "util/raii/PangoSPtr.h"

class Text;
class Control;
class TextEditorCallbacks;
struct KeyEvent;
class FlyingClickableIcon;
class TextAlignment;

namespace xoj::util {
template <class T>
class DispatchPool;
template <class T>
struct Point;
};

namespace xoj::view {
class TextEditionView;
};

class TextEditor: public OverlayBase {
public:
    TextEditor(Control* control, const PageRef& page, GtkWidget* xournalWidget, double x, double y);
    virtual ~TextEditor();

    /** Represents the different kinds of text selection */
    enum class SelectType { WORD, PARAGRAPH, ALL };

    bool onKeyPressEvent(const KeyEvent& event);
    bool onKeyReleaseEvent(const KeyEvent& event);
    void mousePressed(double x, double y);  ///< Coordinates are in Page coordinates
    void mouseMoved(double x, double y);    ///< Coordinates are in Page coordinates
    void mouseReleased();

    /**
     * @brief Returns a pointer to the edited Text element.
     * Warning: The content of the Text element does not need to be up to date with the buffer's content
     * Use `updateTextElementContent` to sync them
     */
    Text* getTextElement() const;

    bool bufferEmpty() const;

    void setFont(XojFont font);
    void setBold(bool bold);
    void setItalic(bool italic);
    void setFontSize(double size);
    void adjustFontSize(double delta);
    void updateFormattingActions() const;
    void setColor(Color color);
    void setAlignment(TextAlignment al);
    void setJustify(bool justify);

    PangoLayout* getUpToDateLayout() const;

    const std::shared_ptr<xoj::util::DispatchPool<xoj::view::TextEditionView>>& getViewPool() const;

    Color getSelectionColor() const;

    const Range& getCursorBox() const;
    const Range& getContentBoundingBox() const;
    inline double getCurrentWrapWidth() const { return currentWrapWidth; }

    bool isCursorVisible() const;

    void deleteFromCursor(GtkDeleteType type, int count);
    void copyToClipboard() const;
    void cutToClipboard();
    void pasteFromClipboard();
    void selectAtCursor(TextEditor::SelectType ty);

    bool canUndoTextEdit() const;
    bool canRedoTextEdit() const;
    bool undoTextEdit();
    bool redoTextEdit();

    void onViewCreation() const;  ///< Call upon creation of a view

private:
    void toggleOverwrite();
    void toggleBoldFace();
    void toggleItalicFace();
    void increaseFontSize();
    void decreaseFontSize();
    void moveCursor(GtkMovementStep step, int count, bool extendSelection);
    void backspace();
    void linebreak();
    void tabulation();

    void afterFontChange();
    void replaceBufferContent(const std::string& text);

    GtkTextTag* getFontTag(const XojFont& font);
    GtkTextTag* getColorTag(Color color);
    XojFont getFontAtIterator(const GtkTextIter& iter) const;
    std::optional<Color> getColorAtIterator(const GtkTextIter& iter) const;
    void clearFontTags();
    void applyModelStylesToBuffer();
    void applyTypingFont(size_t start, size_t end);
    bool getSelectionByteRange(size_t& start, size_t& end) const;
    bool currentFontAttribute(bool bold) const;
    void formatSelection(const std::function<void(size_t, size_t)>& formatter);

    struct TextEditState {
        std::string text;
        std::string styleRuns;
        XojFont font;
        Color color;
        double wrapWidth = -1;
        int alignment = 0;
        bool justify = false;
        std::optional<XojFont> typingFont;
        std::optional<Color> typingColor;
        size_t cursorOffset = 0;
        size_t selectionBoundOffset = 0;
    };

    TextEditState captureTextEditState() const;
    void restoreTextEditState(const TextEditState& state);
    void initializeTextEditHistory();
    void prepareTextEditHistory();
    void recordTextEditHistory();
    void beginTextEditHistoryGroup();
    void endTextEditHistoryGroup();
    void updateTextEditUndoActions() const;

    void finalizeEdition();
    void initializeEditionAt(double x, double y);

private:
    /**
     * @brief Add the text to the provided Pango layout.
     * The added text contains both this->text, and the preedit string of the Input Method (this->preeditstring)
     * This function also sets up the attributes of the preedit string (typically underlined)
     */
    void setTextToPangoLayout(PangoLayout* pl) const;

    void setSelectionAttributesToPangoLayout(PangoLayout* pl) const;

    Range computeBoundingBox() const;
    void repaintEditor(bool sizeChanged = true);

    /**
     * @brief Compute the cursor's location
     * @return The bounding box of the cursor, in TextBox coordinates (i.e relative to the text's getOrigin())
     *          The bounding box is returned even if the cursor is currently not visible (blinking...)
     * WARNING: The returned box may have width == 0 (if in insertion mode or at the end of a line). In this case, the
     *          width of the displayed cursor should be decided by the view class (depending on zoom for instance)
     */
    Range computeCursorBox() const;

    void repaintCursorAfterChange();
    void resetImContext();

    static void bufferPasteDoneCallback(GtkTextBuffer* buffer, GtkClipboard* clipboard, TextEditor* te);

    static void iMCommitCallback(GtkIMContext* context, const gchar* str, TextEditor* te);
    static void iMPreeditChangedCallback(GtkIMContext* context, TextEditor* te);
    static bool iMRetrieveSurroundingCallback(GtkIMContext* context, TextEditor* te);
    static bool imDeleteSurroundingCallback(GtkIMContext* context, gint offset, gint n_chars, TextEditor* te);

    void moveCursorIterator(const GtkTextIter* newLocation, gboolean extendSelection);

    void computeVirtualCursorPosition();
    void jumpALine(GtkTextIter* textIter, int count);

    void findPos(GtkTextIter* iter, double x, double y) const;
    void markPos(double x, double y, bool extendSelection);

    void contentsChanged(bool forceCreateUndoAction = false);
    void updateCursorBox();
    void updateDraggableIcons() const;  ///< Update the position of the handles

    void updateTextElementContent();

    static void blinkCallback(TextEditor* te);

private:
    Control* control;
    PageRef page;

    /**
     * @brief Pointer to the main window's widget. Used for fetching settings and clipboards, and ringing the bell.
     */
    GtkWidget* xournalWidget;

    /**
     * @brief Text element under edition, clone of the original Text element (if any)
     */
    std::unique_ptr<Text> textElement;
    Text* originalTextElement;

    xoj::util::GObjectSPtr<GtkIMContext> imContext;
    xoj::util::GObjectSPtr<GtkTextBuffer> buffer;
    xoj::util::GObjectSPtr<PangoLayout> layout;

    std::unordered_map<std::string, GtkTextTag*> fontTags;
    std::unordered_map<uint32_t, GtkTextTag*> colorTags;
    std::optional<XojFont> typingFont;
    std::optional<Color> typingColor;

    std::vector<TextEditState> textEditHistory;
    size_t textEditHistoryIndex = 0;
    size_t textEditHistoryGroupDepth = 0;
    bool restoringTextEditHistory = false;

    enum class LayoutStatus { UP_TO_DATE, NEEDS_ATTRIBUTES_UPDATE, NEEDS_PARAMETERS_UPDATE, NEEDS_COMPLETE_UPDATE };
    mutable LayoutStatus layoutStatus;

    // InputMethod preedit data
    int preeditCursor;
    xoj::util::PangoAttrListSPtr preeditAttrList;
    xoj::util::OwnedCString preeditString;

    /**
     * @brief Tracks the bounding box of the editor from the last render.
     *
     * Because adding or deleting lines may cause the size of the bounding box to change,
     * we need to repaint the union of the current and previous bboxes.
     */
    Range previousBoundingBox;
    Range cursorBox;

    std::shared_ptr<xoj::util::DispatchPool<xoj::view::TextEditionView>> viewPool;

    std::unique_ptr<FlyingClickableIcon> moveIcon;
    std::unique_ptr<FlyingClickableIcon> extendIcon;

    double currentWrapWidth;  ///< Wrap width. May differ from textElement->getWrap() while resizing the text area

    /**
     * (The virtual cursor is used when moving the cursor vertically (e.g. pressing up arrow), to get a good "vertical
     * move" feeling, even if we pass by (say) an empty line)
     */
    struct VirtualCursorPosition {
        int pangoLineNumber = 0;  ///< Line number in displayed text
        int abscissa = 0;         ///< In Pango coordinates
    } virtualCursorPosition;

    // cursor blinking timings. In millisecond.
    unsigned int cursorBlinkingTimeOn = 0;
    unsigned int cursorBlinkingTimeOff = 0;
    xoj::util::GSourceURef blinkTimer;
    bool cursorBlink = true;

    bool needImReset = false;
    bool mouseDown = false;
    bool cursorOverwrite = false;
    bool cursorVisible = false;

    // In a blinking period, how much time is the cursor visible vs not visible
    static constexpr unsigned int CURSOR_ON_MULTIPLIER = 2;
    static constexpr unsigned int CURSOR_OFF_MULTIPLIER = 1;
    static constexpr unsigned int CURSOR_DIVIDER = CURSOR_ON_MULTIPLIER + CURSOR_OFF_MULTIPLIER;

    struct KeyBindings;
    static const KeyBindings keyBindings;
};
