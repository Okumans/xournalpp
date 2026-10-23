#include "PdfFloatingToolbox.h"

#include <algorithm>  // for max, min
#include <array>
#include <cmath>    // for abs
#include <cstddef>  // for size_t
#include <memory>
#include <string>   // for string
#include <utility>  // for move
#include <vector>   // for vector

#include <glib-object.h>  // for G_CALLBACK, g_signal_connect
#include <gtk/gtk.h>

#include "control/Control.h"      // for Control
#include "control/ToolEnums.h"    // for ToolType, TOOL_SELECT_PDF_TEXT_LI...
#include "control/ToolHandler.h"  // for ToolHandler
#include "control/tools/EditSelection.h"
#include "control/tools/PdfElemSelection.h"
#include "gui/PageView.h"           // for XojPageView
#include "gui/XournalView.h"        // for XournalView
#include "model/Document.h"         // for Document
#include "model/Layer.h"            // for Layer
#include "model/PageRef.h"          // for PageRef
#include "model/Point.h"            // for Point
#include "model/Stroke.h"           // for Stroke, BUTT, StrokeTool::HIGHLIG...
#include "model/XojPage.h"          // for XojPage
#include "undo/ColorUndoAction.h"   // for ColorUndoAction
#include "undo/GroupUndoAction.h"   // for GroupUndoAction
#include "undo/InsertUndoAction.h"  // for InsertUndoAction
#include "undo/UndoAction.h"        // for UndoAction
#include "undo/UndoRedoHandler.h"   // for UndoRedoHandler
#include "util/Assert.h"            // for xoj_assert
#include "util/gtk4_helper.h"       // for gtk_widget_get_clipboard
#include "util/i18n.h"

#include "MainWindow.h"  // for MainWindow

namespace {

constexpr double PDF_HIGHLIGHT_MATCH_TOLERANCE = 1.0;

struct HighlightPreset {
    const char* widget;
    const char* label;
    GdkRGBA rgba;
};

constexpr std::array<HighlightPreset, 6> HIGHLIGHT_PRESETS{{
        {"pdfTbPresetYellow", N_("Yellow highlight"), {1.0, 224.0 / 255, 102.0 / 255, 1.0}},
        {"pdfTbPresetGreen", N_("Green highlight"), {140.0 / 255, 233.0 / 255, 154.0 / 255, 1.0}},
        {"pdfTbPresetBlue", N_("Blue highlight"), {116.0 / 255, 192.0 / 255, 252.0 / 255, 1.0}},
        {"pdfTbPresetPink", N_("Pink highlight"), {250.0 / 255, 162.0 / 255, 193.0 / 255, 1.0}},
        {"pdfTbPresetOrange", N_("Orange highlight"), {1.0, 192.0 / 255, 120.0 / 255, 1.0}},
        {"pdfTbPresetPurple", N_("Purple highlight"), {208.0 / 255, 191.0 / 255, 1.0, 1.0}},
}};

bool approximatelyEqual(double lhs, double rhs) { return std::abs(lhs - rhs) <= PDF_HIGHLIGHT_MATCH_TOLERANCE; }

bool isPdfHighlightForRect(const Stroke& stroke, const XojPdfRectangle& rect) {
    if (stroke.getToolType() != StrokeTool::HIGHLIGHTER || stroke.getStrokeCapStyle() != StrokeCapStyle::BUTT ||
        stroke.getPointCount() != 2) {
        return false;
    }

    const auto& points = stroke.getPointVector();
    if (points[0].z != Point::NO_PRESSURE || points[1].z != Point::NO_PRESSURE) {
        return false;
    }

    const double rectLeft = std::min(rect.x1, rect.x2);
    const double rectRight = std::max(rect.x1, rect.x2);
    const double rectTop = std::min(rect.y1, rect.y2);
    const double rectBottom = std::max(rect.y1, rect.y2);
    const double rectMiddle = (rectTop + rectBottom) / 2;
    const double rectHeight = rectBottom - rectTop;

    const double lineLeft = std::min(points[0].x, points[1].x);
    const double lineRight = std::max(points[0].x, points[1].x);
    const double lineMiddle = (points[0].y + points[1].y) / 2;

    return approximatelyEqual(points[0].y, points[1].y) && approximatelyEqual(lineLeft, rectLeft) &&
           approximatelyEqual(lineRight, rectRight) && approximatelyEqual(lineMiddle, rectMiddle) &&
           approximatelyEqual(stroke.getWidth(), rectHeight);
}

}  // namespace

PdfFloatingToolbox::PdfFloatingToolbox(MainWindow* theMainWindow, GtkOverlay* overlay):
        theMainWindow(theMainWindow), overlay(overlay, xoj::util::ref), position({0, 0}) {
    this->floatingToolbox = theMainWindow->get("pdfFloatingToolbox");

    gtk_overlay_add_overlay(overlay, this->floatingToolbox);
    gtk_overlay_set_overlay_pass_through(overlay, this->floatingToolbox, true);

    g_signal_connect(overlay, "get-child-position", G_CALLBACK(this->getOverlayPosition), this);

    g_signal_connect(theMainWindow->get("pdfTbHighlight"), "clicked", G_CALLBACK(this->highlightCb), this);
    g_signal_connect(theMainWindow->get("pdfTbCopyText"), "clicked", G_CALLBACK(this->copyTextCb), this);
    g_signal_connect(theMainWindow->get("pdfTbUnderline"), "clicked", G_CALLBACK(this->underlineCb), this);
    g_signal_connect(theMainWindow->get("pdfTbStrikethrough"), "clicked", G_CALLBACK(this->strikethroughCb), this);
    g_signal_connect(theMainWindow->get("pdfTbChangeType"), "clicked", G_CALLBACK(this->switchSelectTypeCb), this);

    g_signal_connect(theMainWindow->get("pdfTbHighlightColor"), "color-set", G_CALLBACK(highlightColorCb), this);
    g_signal_connect(theMainWindow->get("pdfTbTextColor"), "color-set", G_CALLBACK(textColorCb), this);
    for (const auto* name: {"pdfTbCopyText", "pdfTbHighlight", "pdfTbUnderline", "pdfTbStrikethrough", "pdfTbTextColor",
                            "pdfTbChangeType", "pdfTbHighlightColor"}) {
        auto* widget = theMainWindow->get(name);
        auto* description = gtk_widget_get_tooltip_text(widget);
        atk_object_set_name(gtk_widget_get_accessible(widget), description);
        g_free(description);
    }
    for (size_t i = 0; i < HIGHLIGHT_PRESETS.size(); ++i) {
        auto* button = theMainWindow->get(HIGHLIGHT_PRESETS[i].widget);
        g_object_set_data(G_OBJECT(button), "pdf-highlight-preset", GINT_TO_POINTER(int(i)));
        atk_object_set_name(gtk_widget_get_accessible(button), _(HIGHLIGHT_PRESETS[i].label));
        g_signal_connect(button, "clicked", G_CALLBACK(highlightPresetCb), this);
    }
    this->clearSelection();
    this->hide();
}

PdfFloatingToolbox::~PdfFloatingToolbox() = default;

PdfElemSelection* PdfFloatingToolbox::getSelection() const { return this->pdfElemSelection.get(); }
bool PdfFloatingToolbox::hasSelection() const { return this->getSelection() != nullptr; }

void PdfFloatingToolbox::clearSelection() { this->pdfElemSelection.reset(); }

auto PdfFloatingToolbox::newSelection(double x, double y) -> const PdfElemSelection* {
    this->pdfElemSelection = std::make_unique<PdfElemSelection>(x, y, this->theMainWindow->getControl());
    return this->pdfElemSelection.get();
}

void PdfFloatingToolbox::show(int x, int y) {
    xoj_assert(this->getSelection());
    this->objectSelection = false;
    this->position = {x, y};

    // Use the persistent color of the regular Highlighter tool. PDF text selection tools have their own transient
    // marker color, which should not override the user's selected highlighter color.
    auto* toolHandler = theMainWindow->getXournal()->getControl()->getToolHandler();
    this->color = toolHandler->getTool(TOOL_HIGHLIGHTER).getColor();
    const auto& textTool = toolHandler->getTool(TOOL_TEXT);
    this->textColor =
            textTool.hasCapability(TOOL_CAP_COLOR) ? textTool.getColor() : toolHandler->getTool(TOOL_PEN).getColor();
    updateColorButtons();
    this->show();
}

void PdfFloatingToolbox::updateColorButtons() {
    const auto highlight = Util::rgb_to_GdkRGBA(this->color);
    const auto text = Util::rgb_to_GdkRGBA(this->textColor);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(theMainWindow->get("pdfTbHighlightColor")), &highlight);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(theMainWindow->get("pdfTbTextColor")), &text);
    for (const auto& preset: HIGHLIGHT_PRESETS) {
        auto* context = gtk_widget_get_style_context(theMainWindow->get(preset.widget));
        const auto presetColor = Util::GdkRGBA_to_rgb(preset.rgba);
        if (color.red == presetColor.red && color.green == presetColor.green && color.blue == presetColor.blue) {
            gtk_style_context_add_class(context, "selected");
        } else {
            gtk_style_context_remove_class(context, "selected");
        }
    }
}

void PdfFloatingToolbox::highlightPresetCb(GtkButton* button, PdfFloatingToolbox* pft) {
    const auto index = size_t(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "pdf-highlight-preset")));
    xoj_assert(index < HIGHLIGHT_PRESETS.size());
    auto* chooser = GTK_COLOR_CHOOSER(pft->theMainWindow->get("pdfTbHighlightColor"));
    gtk_color_chooser_set_rgba(chooser, &HIGHLIGHT_PRESETS[index].rgba);
    highlightColorCb(GTK_COLOR_BUTTON(chooser), pft);
}

void PdfFloatingToolbox::showForObjectSelection(int x, int y) {
    auto* selection = theMainWindow->getXournal()->getSelection();
    if (!selection) {
        return;
    }
    bool found = false;
    for (const auto* element: selection->getElementsView()) {
        const auto* stroke = dynamic_cast<const Stroke*>(element);
        if (!stroke || stroke->getToolType() != StrokeTool::HIGHLIGHTER) {
            return;
        }
        this->color = stroke->getColor();
        found = true;
    }
    if (!found) {
        return;
    }
    this->objectSelection = true;
    this->position = {x, y};
    updateColorButtons();
    this->show();
}

void PdfFloatingToolbox::highlightColorCb(GtkColorButton* button, PdfFloatingToolbox* pft) {
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    pft->color = Util::GdkRGBA_to_rgb(rgba);
    auto* control = pft->theMainWindow->getControl();
    auto& tool = control->getToolHandler()->getTool(TOOL_HIGHLIGHTER);
    const auto alpha = tool.getColor().alpha;
    tool.setColor(Color(pft->color.red, pft->color.green, pft->color.blue, alpha));
    if (pft->objectSelection) {
        if (auto* selection = pft->theMainWindow->getXournal()->getSelection()) {
            control->getUndoRedoHandler()->addUndoAction(selection->setColor(pft->color));
        }
        pft->hide();
        pft->theMainWindow->getXournal()->requestFocus();
    } else if (pft->hasSelection()) {
        highlightCb(nullptr, pft);
    }
}

void PdfFloatingToolbox::textColorCb(GtkColorButton* button, PdfFloatingToolbox* pft) {
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(button), &rgba);
    pft->textColor = Util::GdkRGBA_to_rgb(rgba);
    pft->theMainWindow->getControl()->getToolHandler()->getTool(TOOL_TEXT).setColor(pft->textColor);
    pft->theMainWindow->getXournal()->requestFocus();
}

void PdfFloatingToolbox::hide() {
    if (isHidden())
        return;

    gtk_widget_hide(this->floatingToolbox);
}

auto PdfFloatingToolbox::getOverlayPosition(GtkOverlay* overlay, GtkWidget* widget, GdkRectangle* allocation,
                                            PdfFloatingToolbox* self) -> gboolean {
    if (widget == self->floatingToolbox) {
        // Get existing width and height
        GtkRequisition natural;
        gtk_widget_get_preferred_size(widget, nullptr, &natural);
        allocation->width = natural.width;
        allocation->height = natural.height;

        // Make sure the "pdfFloatingToolbox" is fully displayed.
        const int gap = 8;

        // Prefer the space above the selection so the toolbar does not cover the next line of text.
        // Flip horizontally near the right edge and fall below the selection near the top edge.

        GtkWidget* scrolledWindow =
                gtk_widget_get_ancestor(self->theMainWindow->getXournal()->getWidget(), GTK_TYPE_SCROLLED_WINDOW);

        bool rightOK = self->position.x + allocation->width + gap <= gtk_widget_get_allocated_width(scrolledWindow);
        bool aboveOK = self->position.y >= allocation->height + gap;

        allocation->x = rightOK ? self->position.x + gap : self->position.x - allocation->width - gap;
        allocation->y = aboveOK ? self->position.y - allocation->height - gap : self->position.y + gap;

        // Long translated labels and selections near an edge must not push controls outside the viewport.
        allocation->x = std::clamp(allocation->x, 0,
                                   std::max(0, gtk_widget_get_allocated_width(scrolledWindow) - allocation->width));
        allocation->y = std::clamp(allocation->y, 0,
                                   std::max(0, gtk_widget_get_allocated_height(scrolledWindow) - allocation->height));

        gtk_widget_translate_coordinates(scrolledWindow, GTK_WIDGET(overlay), allocation->x, allocation->y,
                                         &allocation->x, &allocation->y);

        return true;
    }

    return false;
}

void PdfFloatingToolbox::userCancelSelection() {
    this->pdfElemSelection.reset();
    this->hide();
    this->theMainWindow->getXournal()->requestFocus();
}

void PdfFloatingToolbox::highlightCb(GtkButton* button, PdfFloatingToolbox* pft) {
    int markerOpacity = pft->theMainWindow->getControl()->getToolHandler()->getSelectPDFTextMarkerOpacity();
    pft->createStrokes(PdfMarkerStyle::POS_TEXT_MIDDLE, PdfMarkerStyle::WIDTH_TEXT_HEIGHT, markerOpacity);
    pft->userCancelSelection();
}

void PdfFloatingToolbox::copyTextCb(GtkButton* button, PdfFloatingToolbox* pft) {
    pft->copyTextToClipboard();
    pft->userCancelSelection();
}

void PdfFloatingToolbox::underlineCb(GtkButton* button, PdfFloatingToolbox* pft) {
    pft->createStrokes(PdfMarkerStyle::POS_TEXT_BOTTOM, PdfMarkerStyle::WIDTH_TEXT_LINE, 230);
    pft->userCancelSelection();
}

void PdfFloatingToolbox::strikethroughCb(GtkButton* button, PdfFloatingToolbox* pft) {
    pft->createStrokes(PdfMarkerStyle::POS_TEXT_MIDDLE, PdfMarkerStyle::WIDTH_TEXT_LINE, 230);
    pft->userCancelSelection();
}

void PdfFloatingToolbox::show() {
    gtk_widget_hide(this->floatingToolbox);  // force showing in new position
    gtk_widget_show_all(this->floatingToolbox);
    gtk_widget_set_visible(theMainWindow->get("pdfTbTextActions"), !this->objectSelection);
    auto* paletteIcon = theMainWindow->get("pdfTbPaletteIcon");
    const auto* description = objectSelection ? _("Recolor selected highlight") : _("Highlight colors");
    gtk_widget_set_tooltip_text(paletteIcon, description);
    atk_object_set_name(gtk_widget_get_accessible(paletteIcon), description);
}

void PdfFloatingToolbox::copyTextToClipboard() {
    GtkClipboard* clipboard = gtk_widget_get_clipboard(this->theMainWindow->getWindow());
    if (const std::string& text = this->pdfElemSelection->getSelectedText(); !text.empty()) {
        gtk_clipboard_set_text(clipboard, text.c_str(), -1);
    }
}

void PdfFloatingToolbox::createStrokes(PdfMarkerStyle position, PdfMarkerStyle width, int markerOpacity) {
    const size_t pdfPageNo = this->pdfElemSelection->getSelectionPageNr();
    const size_t currentPage = theMainWindow->getXournal()->getCurrentPage();

    // Get the PDF page that the current page corresponds to.
    // It should be the same as the PDF page of the selection.
    auto doc = this->theMainWindow->getControl()->getDocument();
    doc->lock_shared();
    const size_t pdfPageOfCurrentPage = doc->getPage(currentPage)->getPdfPageNr();
    doc->unlock_shared();

    if (pdfPageOfCurrentPage != pdfPageNo) {
        // There's probably a bug that violates our assumptions, so no-op.
        g_warning("The current page's PDF page is not the same as the PDF page of the selection!");
        return;
    }

    const auto textRects = this->pdfElemSelection->getSelectedTextRects();
    if (textRects.empty()) {
        return;
    }

    auto* control = this->theMainWindow->getControl();
    PageRef page = control->getCurrentPage();
    Layer* layer = page->getSelectedLayer();

    Range dirtyRange;
    Range recolorRange;
    std::vector<ElementPtr> strokes;
    auto colorUndo = std::make_unique<ColorUndoAction>(page, layer);
    bool recolored = false;

    const bool canUpdateExistingHighlight =
            position == PdfMarkerStyle::POS_TEXT_MIDDLE && width == PdfMarkerStyle::WIDTH_TEXT_HEIGHT;
    const Color strokeColor = canUpdateExistingHighlight ? this->color : this->textColor;

    doc->lock();
    for (XojPdfRectangle rect: textRects) {
        const double topOfLine = std::min(rect.y1, rect.y2);
        const double middleOfLine = (rect.y1 + rect.y2) / 2;
        const double bottomOfLine = std::max(rect.y1, rect.y2);
        const double rectWidth = std::abs(rect.y2 - rect.y1);

        // the center line position of stroke
        const double h = position == PdfMarkerStyle::POS_TEXT_BOTTOM ? bottomOfLine :
                         position == PdfMarkerStyle::POS_TEXT_MIDDLE ? middleOfLine :
                                                                       topOfLine;
        // the width of stroke
        const double w = width == PdfMarkerStyle::WIDTH_TEXT_LINE ? 1 : rectWidth;

        bool foundExistingHighlight = false;
        if (canUpdateExistingHighlight) {
            for (auto& element: layer->getElements()) {
                auto* existingStroke = dynamic_cast<Stroke*>(element.get());
                if (!existingStroke || !isPdfHighlightForRect(*existingStroke, rect)) {
                    continue;
                }

                const Color oldColor = existingStroke->getColor();
                if (oldColor != strokeColor) {
                    existingStroke->setColor(strokeColor);
                    colorUndo->addStroke(existingStroke, oldColor, strokeColor);
                    recolorRange = recolorRange.unite(Range(existingStroke->getBoundingBox()));
                    recolored = true;
                }
                foundExistingHighlight = true;
                break;
            }
        }

        if (foundExistingHighlight) {
            continue;
        }

        auto stroke = std::make_unique<Stroke>();
        stroke->setColor(strokeColor);
        stroke->setFill(markerOpacity);
        stroke->setToolType(StrokeTool::HIGHLIGHTER);
        stroke->setWidth(w);
        stroke->addPoint(Point(rect.x1, h, -1));
        stroke->addPoint(Point(rect.x2, h, -1));
        stroke->setStrokeCapStyle(StrokeCapStyle::BUTT);

        dirtyRange.addPoint(rect.x1, h - 0.5 * w);
        dirtyRange.addPoint(rect.x2, h + 0.5 * w);

        strokes.push_back(std::move(stroke));
    }

    std::vector<const Element*> strokePtrs(strokes.size());
    std::transform(strokes.begin(), strokes.end(), strokePtrs.begin(), [](auto& e) { return e.get(); });

    for (auto&& s: strokes) {
        layer->addElement(std::move(s));
    }
    doc->unlock();

    if (!strokePtrs.empty()) {
        page->fireElementsChanged(strokePtrs, dirtyRange);
    }
    if (recolored) {
        page->fireRangeChanged(recolorRange);
    }

    auto undoAct = std::make_unique<GroupUndoAction>();
    if (recolored) {
        undoAct->addAction(std::move(colorUndo));
    }
    for (auto* stroke: strokePtrs) {
        undoAct->addAction(std::make_unique<InsertUndoAction>(page, layer, stroke));
    }
    if (recolored || !strokePtrs.empty()) {
        control->getUndoRedoHandler()->addUndoAction(std::move(undoAct));
    }
}

void PdfFloatingToolbox::switchSelectTypeCb(GtkButton* button, PdfFloatingToolbox* pft) {
    auto* toolHandler = pft->theMainWindow->getControl()->getToolHandler();
    const ToolType activeType = toolHandler->getToolType();
    ToolType type = activeType;

    if (activeType == ToolType::TOOL_SMART_SELECT) {
        type = pft->selectionStyle == XojPdfPageSelectionStyle::Linear ? ToolType::TOOL_SELECT_PDF_TEXT_RECT :
                                                                         ToolType::TOOL_SELECT_PDF_TEXT_LINEAR;
    } else {
        type = activeType == ToolType::TOOL_SELECT_PDF_TEXT_LINEAR ? ToolType::TOOL_SELECT_PDF_TEXT_RECT :
                                                                     ToolType::TOOL_SELECT_PDF_TEXT_LINEAR;
    }

    // Smart Select remains the active tool while the toolbox changes only
    // the style used to recompute the current PDF selection.
    if (activeType != ToolType::TOOL_SMART_SELECT) {
        pft->theMainWindow->getControl()->selectTool(type);
    }

    pft->selectionStyle = PdfElemSelection::selectionStyleForToolType(type);
    pft->pdfElemSelection->setToolType(type);
    pft->pdfElemSelection->finalizeSelection(pft->selectionStyle);
}

bool PdfFloatingToolbox::isHidden() const { return !gtk_widget_is_visible(this->floatingToolbox); }
