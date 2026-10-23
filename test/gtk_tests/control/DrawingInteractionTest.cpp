#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include <cairo-pdf.h>
#include <config-test.h>
#include <glib/gstdio.h>
#include <gtest/gtest.h>
#include <gtk/gtk.h>

#include "control/Control.h"
#include "control/ToolHandler.h"
#include "control/jobs/XournalScheduler.h"
#include "control/settings/Settings.h"
#include "control/tools/ArrowHandler.h"
#include "control/tools/EditSelection.h"
#include "control/tools/EllipseHandler.h"
#include "control/tools/PdfElemSelection.h"
#include "control/tools/RectangleHandler.h"
#include "control/tools/RulerHandler.h"
#include "control/tools/StrokeHandler.h"
#include "control/zoom/ZoomControl.h"
#include "gui/GladeSearchpath.h"
#include "gui/MainWindow.h"
#include "gui/PageView.h"
#include "gui/PdfFloatingToolbox.h"
#include "gui/XournalView.h"
#include "gui/XournalppCursor.h"
#include "gui/inputdevices/MouseInputHandler.h"
#include "gui/inputdevices/PositionInputData.h"
#include "gui/scroll/ScrollHandling.h"
#include "gui/widgets/XournalWidget.h"
#include "model/Document.h"
#include "model/Stroke.h"
#include "model/XojPage.h"
#include "undo/UndoRedoHandler.h"

namespace {

class InjectedMouse: public MouseInputHandler {
public:
    using MouseInputHandler::MouseInputHandler;
    void press(InputEvent event) {
        deviceClassPressed = true;
        actionStart(event);
    }
    void move(InputEvent event) { actionMotion(event); }
    void release(InputEvent event) {
        deviceClassPressed = false;
        actionEnd(event);
    }
};

class DrawingInteractionTest: public ::testing::Test {
protected:
    void SetUp() override {
        if (!gtk_init_check(nullptr, nullptr)) {
            GTEST_SKIP() << "Requires a GTK display";
        }
        paths.addSearchDirectory(fs::path(PROJECT_BINARY_DIR) / "ui");
        paths.addSearchDirectory(fs::path(PROJECT_SOURCE_DIR) / "ui");
        app = gtk_application_new("com.github.xournalpp.DrawingInteractionTest", G_APPLICATION_NON_UNIQUE);
        ASSERT_TRUE(g_application_register(G_APPLICATION(app), nullptr, nullptr));
        control = std::make_unique<Control>(G_APPLICATION(app), &paths, true);
        control->getSettings()->setAutosaveEnabled(false);
        control->getSettings()->setDrawDirModsEnabled(false);
        control->getSettings()->setSnapGrid(false);
        control->getSettings()->setSnapRotation(false);
        control->getSettings()->setStrokeFilterEnabled(false);
        window = std::make_unique<MainWindow>(&paths, control.get(), app);
        control->initWindow(window.get());
        window->populate(&paths);
        control->openFileWithoutSavingTheCurrentDocument({}, false, -1, [](bool) {});
        page = control->getCurrentPage();
        ASSERT_TRUE(page);
        control->selectTool(TOOL_PEN);
        window->show(nullptr);
        while (g_main_context_iteration(nullptr, false)) {}
        control->getZoomControl()->setZoom(1.0);
        control->getScheduler()->start();
    }

    void TearDown() override {
        if (control) {
            control->clearSelection();
            control->getScheduler()->stop();
            while (g_main_context_iteration(nullptr, false)) {}
        }
        // Keep Control alive while the window's views unregister their listeners.
        window.reset();
        control.reset();
        if (app) {
            g_object_unref(app);
        }
    }

    PositionInputData pos(double x, double y, GdkModifierType state = GdkModifierType(0)) {
        PositionInputData p{};
        p.x = x;
        p.y = y;
        p.pressure = Point::NO_PRESSURE;
        p.state = state;
        return p;
    }

    KeyEvent key(guint keyval, GdkModifierType state = GdkModifierType(0)) { return {keyval, state, nullptr}; }

    GladeSearchpath paths;
    GtkApplication* app = nullptr;
    std::unique_ptr<Control> control;
    std::unique_ptr<MainWindow> window;
    PageRef page;
};

TEST_F(DrawingInteractionTest, rectangleModifiersChangeTheLivePreview) {
    RectangleHandler shape(control.get(), page);
    shape.onButtonPressEvent(pos(100, 100), 1);
    shape.onMotionNotifyEvent(pos(140, 120), 1);
    EXPECT_DOUBLE_EQ(shape.getShape()[2].x, 140);
    EXPECT_DOUBLE_EQ(shape.getShape()[2].y, 120);
    shape.onKeyPressEvent(key(GDK_KEY_Shift_L));
    EXPECT_DOUBLE_EQ(shape.getShape()[2].y, 140);
    shape.onKeyPressEvent(key(GDK_KEY_Alt_L, GDK_SHIFT_MASK));
    EXPECT_DOUBLE_EQ(shape.getShape().front().x, 60);
    EXPECT_DOUBLE_EQ(shape.getShape().front().y, 60);
    shape.onKeyReleaseEvent(key(GDK_KEY_Shift_L, GdkModifierType(GDK_SHIFT_MASK | GDK_MOD1_MASK)));
    EXPECT_DOUBLE_EQ(shape.getShape().front().y, 80);
    shape.onSequenceCancelEvent();
}

TEST_F(DrawingInteractionTest, centeredCircleFitsItsDragBox) {
    EllipseHandler shape(control.get(), page);
    shape.onButtonPressEvent(pos(100, 100), 1);
    shape.onMotionNotifyEvent(pos(140, 120, GdkModifierType(GDK_SHIFT_MASK | GDK_MOD1_MASK)), 1);
    const auto& points = shape.getShape();
    EXPECT_DOUBLE_EQ(points.front().x, 140);
    EXPECT_DOUBLE_EQ(points.front().y, 100);
    auto ymin = points.front().y, ymax = ymin;
    for (const auto& p: points) {
        ymin = std::min(ymin, p.y);
        ymax = std::max(ymax, p.y);
    }
    EXPECT_DOUBLE_EQ(ymin, 60);
    EXPECT_DOUBLE_EQ(ymax, 140);
    shape.onSequenceCancelEvent();
}

TEST_F(DrawingInteractionTest, linesSnapToDiagonalAndSupportCenteredDrawing) {
    RulerHandler shape(control.get(), page);
    shape.onButtonPressEvent(pos(100, 100), 1);
    shape.onMotionNotifyEvent(pos(140, 130, GdkModifierType(GDK_SHIFT_MASK | GDK_MOD1_MASK)), 1);
    const auto& points = shape.getShape();
    EXPECT_NEAR(points[1].x - 100, points[1].y - 100, 1e-8);
    EXPECT_NEAR((points[0].x + points[1].x) / 2, 100, 1e-8);
    EXPECT_NEAR((points[0].y + points[1].y) / 2, 100, 1e-8);
    shape.onSequenceCancelEvent();
}

TEST_F(DrawingInteractionTest, spaceMovesTheUnfinishedRectangleWithoutResizing) {
    RectangleHandler shape(control.get(), page);
    shape.onButtonPressEvent(pos(100, 100), 1);
    shape.onMotionNotifyEvent(pos(140, 120), 1);
    ASSERT_TRUE(shape.onKeyPressEvent(key(GDK_KEY_space)));
    shape.onMotionNotifyEvent(pos(150, 135), 1);
    EXPECT_DOUBLE_EQ(shape.getShape().front().x, 110);
    EXPECT_DOUBLE_EQ(shape.getShape().front().y, 115);
    EXPECT_DOUBLE_EQ(shape.getShape()[2].x, 150);
    EXPECT_DOUBLE_EQ(shape.getShape()[2].y, 135);
    shape.onKeyReleaseEvent(key(GDK_KEY_space));
    shape.onMotionNotifyEvent(pos(160, 145), 1);
    EXPECT_DOUBLE_EQ(shape.getShape().front().x, 110);
    EXPECT_DOUBLE_EQ(shape.getShape()[2].x, 160);
    shape.onSequenceCancelEvent();
}

TEST_F(DrawingInteractionTest, escapeCancelsWithoutAnUndoEntryEvenAfterPointerRelease) {
    RectangleHandler shape(control.get(), page);
    shape.onButtonPressEvent(pos(100, 100), 1);
    shape.onMotionNotifyEvent(pos(140, 120), 1);
    ASSERT_TRUE(shape.onKeyPressEvent(key(GDK_KEY_Escape)));
    shape.onMotionNotifyEvent(pos(180, 180), 1);
    shape.onButtonReleaseEvent(pos(180, 180), 1);
    EXPECT_TRUE(shape.getShape().empty());
    EXPECT_TRUE(page->getSelectedLayer()->getElementsView().size() == 0);
    EXPECT_FALSE(control->getUndoRedoHandler()->canUndo());
}

TEST_F(DrawingInteractionTest, completedShapeIsOneUndoStep) {
    RectangleHandler shape(control.get(), page);
    shape.onButtonPressEvent(pos(100, 100), 1);
    shape.onMotionNotifyEvent(pos(140, 120), 1);
    shape.onButtonReleaseEvent(pos(150, 130), 1);
    ASSERT_EQ(page->getSelectedLayer()->getElementsView().size(), 1);
    control->getUndoRedoHandler()->undo();
    EXPECT_TRUE(page->getSelectedLayer()->getElementsView().size() == 0);
    EXPECT_FALSE(control->getUndoRedoHandler()->canUndo());
}

TEST_F(DrawingInteractionTest, shiftClickConnectsAtTheSelectedWidthAndSupportsUndo) {
    StraightLineConstraintState state;
    {
        StrokeHandler first(control.get(), page, state);
        first.onButtonPressEvent(pos(100, 100), 1);
        first.onButtonReleaseEvent(pos(100, 100), 1);
    }
    {
        StrokeHandler line(control.get(), page, state);
        line.onButtonPressEvent(pos(150, 130, GDK_SHIFT_MASK), 1);
        line.onButtonReleaseEvent(pos(150, 130, GDK_SHIFT_MASK), 1);
    }
    ASSERT_EQ(page->getSelectedLayer()->getElementsView().size(), 2);
    const auto* stroke = dynamic_cast<const Stroke*>(page->getSelectedLayer()->getElementsView().back());
    ASSERT_NE(stroke, nullptr);
    ASSERT_EQ(stroke->getPointCount(), 2);
    EXPECT_DOUBLE_EQ(stroke->getPoint(0).x, 100);
    EXPECT_DOUBLE_EQ(stroke->getPoint(1).x, 150);
    EXPECT_DOUBLE_EQ(stroke->getPoint(1).y, 130);
    EXPECT_FALSE(stroke->hasPressure());
    control->getUndoRedoHandler()->undo();
    EXPECT_EQ(page->getSelectedLayer()->getElementsView().size(), 1);
}

TEST_F(DrawingInteractionTest, heldSpaceAndWidthShortcutsPreserveTheTool) {
    auto* view = window->getXournal();
    const auto before = control->getToolHandler()->getToolType();
    const auto size = control->getToolHandler()->getSize();
    ASSERT_TRUE(view->onKeyPressEvent(key(GDK_KEY_space)));
    EXPECT_TRUE(view->isSpacePanHeld());
    EXPECT_EQ(control->getToolHandler()->getToolType(), before);
    ASSERT_TRUE(view->onKeyReleaseEvent(key(GDK_KEY_space)));
    EXPECT_FALSE(view->isSpacePanHeld());
    view->onKeyPressEvent(key(GDK_KEY_bracketright));
    EXPECT_EQ(control->getToolHandler()->getSize(), std::min(int(size) + 1, int(TOOL_SIZE_VERY_THICK)));
    view->releaseSpacePan();
}

TEST_F(DrawingInteractionTest, pdfColorControlsDoNotChangeTheSelectedTool) {
    control->selectTool(TOOL_SMART_SELECT);
    const auto penColor = control->getToolHandler()->getTool(TOOL_PEN).getColor();
    const GdkRGBA blue{0.1, 0.3, 0.9, 1};
    auto* swatch = window->get("pdfTbHighlightColor");
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(swatch), &blue);
    g_signal_emit_by_name(swatch, "color-set");
    EXPECT_EQ(control->getToolHandler()->getToolType(), TOOL_SMART_SELECT);
    EXPECT_EQ(control->getToolHandler()->getTool(TOOL_HIGHLIGHTER).getColor().blue, 230);
    EXPECT_EQ(control->getToolHandler()->getTool(TOOL_PEN).getColor(), penColor);
}

TEST_F(DrawingInteractionTest, existingHighlightCanBeRecoloredWithoutReselectingPdfText) {
    control->selectTool(TOOL_SMART_SELECT);
    auto highlight = std::make_unique<Stroke>();
    highlight->setToolType(StrokeTool::HIGHLIGHTER);
    highlight->setColor(Colors::yellow);
    highlight->setFill(80);
    highlight->setWidth(12);
    highlight->addPoint(Point(80, 80));
    highlight->addPoint(Point(180, 80));
    const auto* raw = highlight.get();
    control->getDocument()->lock();
    page->getSelectedLayer()->addElement(std::move(highlight));
    control->getDocument()->unlock();
    page->fireElementChanged(raw);
    auto* view = window->getXournal();
    auto selected = SelectionFactory::createFromElementOnActiveLayer(control.get(), page, view->getViewFor(0), raw);
    view->setSelection(selected.release());
    window->getPdfToolbox()->showForObjectSelection(100, 100);
    EXPECT_FALSE(window->getPdfToolbox()->isHidden());
    auto* swatch = window->get("pdfTbHighlightColor");
    const GdkRGBA green{0, 1, 0, 1};
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(swatch), &green);
    g_signal_emit_by_name(swatch, "color-set");
    EXPECT_EQ(raw->getColor(), Color(0, 255, 0));
    EXPECT_EQ(raw->getFill(), 80);
    EXPECT_EQ(control->getToolHandler()->getToolType(), TOOL_SMART_SELECT);
    control->clearSelection();
    control->getUndoRedoHandler()->undo();
    EXPECT_EQ(raw->getColor(), Colors::yellow);
}

TEST_F(DrawingInteractionTest, temporaryPanDoesNotCreateInkThroughTheInputPipeline) {
    window->show(nullptr);
    while (g_main_context_iteration(nullptr, false)) {}
    auto* view = window->getXournal();
    InjectedMouse mouse(GTK_XOURNAL(view->getWidget())->input);
    auto* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(view->getWidget())));
    InputEvent event;
    event.device = pointer;
    event.deviceClass = INPUT_DEVICE_MOUSE;
    event.deviceId = DeviceId(pointer);
    event.button = 1;
    event.timestamp = 1000;
    event.absolute = {200, 200};
    event.relative = {200, 200};
    event.type = BUTTON_PRESS_EVENT;
    view->onKeyPressEvent(key(GDK_KEY_space));
    mouse.press(event);
    event.type = MOTION_EVENT;
    event.absolute.y -= 40;
    event.relative.y -= 40;
    event.timestamp += 20;
    mouse.move(event);
    // Real input returns to GTK's main loop before the input handler is destroyed.
    while (g_main_context_iteration(nullptr, false)) {}
    view->onKeyReleaseEvent(key(GDK_KEY_space));
    event.type = BUTTON_RELEASE_EVENT;
    mouse.release(event);
    EXPECT_TRUE(page->getSelectedLayer()->getElementsView().size() == 0);
    EXPECT_EQ(control->getToolHandler()->getToolType(), TOOL_PEN);
    EXPECT_FALSE(view->isSpacePanHeld());
}

TEST_F(DrawingInteractionTest, shiftClickInkTakesPrecedenceOverOptionalTapSelection) {
    auto* view = window->getXournal();
    InjectedMouse mouse(GTK_XOURNAL(view->getWidget())->input);
    auto* pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(view->getWidget())));
    const auto pagePosition = view->getViewFor(0)->getPixelPosition();
    InputEvent event;
    event.device = pointer;
    event.deviceClass = INPUT_DEVICE_MOUSE;
    event.deviceId = DeviceId(pointer);
    event.button = 1;
    event.timestamp = 1000;
    event.absolute = {100, 100};
    event.relative = {double(pagePosition.x + 100), double(pagePosition.y + 100)};
    event.type = BUTTON_PRESS_EVENT;
    mouse.press(event);
    event.type = BUTTON_RELEASE_EVENT;
    event.timestamp += 20;
    mouse.release(event);
    ASSERT_EQ(page->getSelectedLayer()->getElementsView().size(), 1);
    control->getSettings()->setStrokeFilterEnabled(true);
    event.state = GDK_SHIFT_MASK;
    event.absolute = {150, 130};
    event.relative = {double(pagePosition.x + 150), double(pagePosition.y + 130)};
    event.timestamp += 1000;
    event.type = BUTTON_PRESS_EVENT;
    mouse.press(event);
    event.type = BUTTON_RELEASE_EVENT;
    event.timestamp += 20;
    mouse.release(event);
    EXPECT_EQ(page->getSelectedLayer()->getElementsView().size(), 2);
}

TEST_F(DrawingInteractionTest, pdfScrollZoomAndInkStress) {
    gchar* filename = nullptr;
    const int fd = g_file_open_tmp("xopp-interaction-XXXXXX.pdf", &filename, nullptr);
    ASSERT_GE(fd, 0);
    g_close(fd, nullptr);
    auto* surface = cairo_pdf_surface_create(filename, 595, 842);
    auto* cr = cairo_create(surface);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    for (int p = 0; p < 30; ++p) {
        for (int line = 0; line < 60; ++line) {
            cairo_move_to(cr, 40, 40 + 12 * line);
            cairo_show_text(cr, "Dense lecture PDF: selection, annotation, scrolling and rendering.");
        }
        cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    bool loaded = false;
    control->openFileWithoutSavingTheCurrentDocument(fs::path(filename), false, 0, [&](bool ok) { loaded = ok; });
    ASSERT_TRUE(loaded);
    auto* view = window->getXournal();
    std::vector<double> pageReadyMs;
    for (size_t p: {0U, 15U, 29U, 5U, 20U, 0U}) {
        const auto start = g_get_monotonic_time();
        view->scrollTo(p);
        const auto deadline = start + 5 * G_TIME_SPAN_SECOND;
        auto* target = view->getViewFor(p);
        while (!target->hasBuffer() && g_get_monotonic_time() < deadline) {
            while (g_main_context_iteration(nullptr, false)) {}
            g_usleep(1000);
        }
        ASSERT_TRUE(target->hasBuffer());
        pageReadyMs.push_back(double(g_get_monotonic_time() - start) / 1000);
    }
    auto* zoom = control->getZoomControl();
    control->getScheduler()->beginZoomGesture();
    zoom->startZoomSequence({200, 200});
    for (int i = 1; i <= 10; ++i) {
        zoom->zoomSequenceChange(1.0 + 0.05 * i, true, {2, 1});
    }
    zoom->endZoomSequence();
    control->getScheduler()->endZoomGesture();
    control->selectTool(TOOL_PEN);
    control->getToolHandler()->setDrawingType(DRAWING_TYPE_DEFAULT);
    auto* pageView = view->getViewFor(view->getCurrentPage());
    const double currentZoom = view->getZoom();
    auto press = pos(100 * currentZoom, 100 * currentZoom);
    pageView->onButtonPressEvent(press);
    double maximumDispatchUs = 0;
    for (int i = 1; i <= 200; ++i) {
        auto sample = pos((100 + i) * currentZoom, (100 + 5 * std::sin(i * 0.1)) * currentZoom);
        const auto start = g_get_monotonic_time();
        pageView->onMotionNotifyEvent(sample);
        maximumDispatchUs = std::max(maximumDispatchUs, double(g_get_monotonic_time() - start));
        if (i % 20 == 0) {
            while (g_main_context_iteration(nullptr, false)) {}
        }
    }
    pageView->onButtonReleaseEvent(pos(300 * currentZoom, 100 * currentZoom));
    EXPECT_EQ(pageView->getPage()->getSelectedLayer()->getElementsView().size(), 1);
    std::sort(pageReadyMs.begin(), pageReadyMs.end());
    std::cout << "XOPP_INTERACTION_RESULT {\"pages\":30,\"page_ready_median_ms\":" << pageReadyMs[3]
              << ",\"page_ready_max_ms\":" << pageReadyMs.back() << ",\"pen_dispatch_max_us\":" << maximumDispatchUs
              << "}\n";
    g_unlink(filename);
    g_free(filename);
}

TEST_F(DrawingInteractionTest, pdfTextCanBeHighlightedInAChosenColorWithoutLeavingSmartSelect) {
    bool loaded = false;
    control->openFileWithoutSavingTheCurrentDocument(
            fs::path(GET_TESTFILE(u8"packaged_xopp/pdfBackground/old.xopp.bg.pdf")), false, 0,
            [&](bool ok) { loaded = ok; });
    ASSERT_TRUE(loaded);
    control->selectTool(TOOL_SMART_SELECT);
    const auto penColor = control->getToolHandler()->getTool(TOOL_PEN).getColor();
    auto* toolbox = window->getPdfToolbox();
    toolbox->newSelection(55, 60);
    auto* selected = toolbox->getSelection();
    selected->setToolType(TOOL_SELECT_PDF_TEXT_LINEAR);
    selected->currentPos(250, 80, XojPdfPageSelectionStyle::Linear);
    ASSERT_TRUE(selected->finalizeSelectionAndRepaint(XojPdfPageSelectionStyle::Linear));
    toolbox->show(200, 200);
    auto* swatch = window->get("pdfTbHighlightColor");
    const GdkRGBA pink{1, 0, 1, 1};
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(swatch), &pink);
    g_signal_emit_by_name(swatch, "color-set");
    EXPECT_FALSE(toolbox->hasSelection());
    const auto elements = control->getCurrentPage()->getSelectedLayer()->getElementsView();
    ASSERT_GT(elements.size(), 0);
    EXPECT_EQ(elements.front()->getColor(), Color(255, 0, 255));
    EXPECT_EQ(control->getToolHandler()->getToolType(), TOOL_SMART_SELECT);
    EXPECT_EQ(control->getToolHandler()->getTool(TOOL_PEN).getColor(), penColor);
    control->getUndoRedoHandler()->undo();
    EXPECT_EQ(control->getCurrentPage()->getSelectedLayer()->getElementsView().size(), 0);
}

}  // namespace
