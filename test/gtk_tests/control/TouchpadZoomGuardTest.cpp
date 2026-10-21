#include <memory>

#include <gtest/gtest.h>
#include <gtk/gtk.h>

#include "control/zoom/TouchpadZoomGuard.h"
#include "control/zoom/ZoomControl.h"
#include "util/raii/GObjectSPtr.h"

bool onTouchpadPinchEvent(GtkWidget* widget, GdkEventTouchpadPinch* event, ZoomControl* zoom);

namespace {

void processEventsFor(gint64 milliseconds) {
    const auto deadline = g_get_monotonic_time() + milliseconds * G_TIME_SPAN_MILLISECOND;
    do {
        while (g_main_context_iteration(nullptr, false)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}

class TouchpadZoomGuardTest: public ::testing::Test {
protected:
    void SetUp() override {
        if (!gtk_init_check(nullptr, nullptr)) {
            GTEST_SKIP() << "Requires a GTK display";
        }

        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_default_size(GTK_WINDOW(window), 400, 400);
        scrolled = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(nullptr, nullptr));
        auto* content = gtk_drawing_area_new();
        gtk_widget_set_size_request(content, 2000, 20000);
        gtk_container_add(GTK_CONTAINER(scrolled), content);
        gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(scrolled));
        gtk_widget_show_all(window);
        processEventsFor(30);

        auto* display = gtk_widget_get_display(window);
        pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
        // Use the backend's concrete device type, but set only the public source
        // metadata. Event delivery still uses the real seat pointer.
        touchpad = makeSource(GDK_SOURCE_TOUCHPAD);
        mouse = makeSource(GDK_SOURCE_MOUSE);
        adjustment = gtk_scrolled_window_get_vadjustment(scrolled);
        gtk_adjustment_set_value(adjustment, 5000);

        g_signal_connect(scrolled, "scroll-event",
                         G_CALLBACK(+[](GtkWidget*, GdkEventScroll* event, gpointer data) -> gboolean {
                             return static_cast<TouchpadZoomGuard*>(data)->blocksScroll(event);
                         }),
                         &guard);
    }

    void TearDown() override {
        if (window) {
            gtk_widget_destroy(window);
        }
    }

    xoj::util::GObjectSPtr<GdkDevice> makeSource(GdkInputSource source) {
        auto* display = gtk_widget_get_display(window);
        return xoj::util::GObjectSPtr<GdkDevice>(
                GDK_DEVICE(g_object_new(G_OBJECT_TYPE(pointer), "name", "Pinch regression test device", "input-source",
                                        source, "type", GDK_DEVICE_TYPE_SLAVE, "display", display, "device-manager",
                                        gdk_display_get_device_manager(display), nullptr)),
                xoj::util::adopt);
    }

    using Event = std::unique_ptr<GdkEvent, decltype(&gdk_event_free)>;

    Event scroll(GdkDevice* source, double dy, bool stop = false, GdkScrollDirection direction = GDK_SCROLL_SMOOTH) {
        Event event(gdk_event_new(GDK_SCROLL), gdk_event_free);
        event->scroll.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(GTK_WIDGET(scrolled))));
        event->scroll.time = timestamp += 20;
        event->scroll.direction = direction;
        event->scroll.delta_y = dy;
        event->scroll.is_stop = stop;
        gdk_event_set_device(event.get(), pointer);
        gdk_event_set_source_device(event.get(), source);
        return event;
    }

    void sendScroll(GdkDevice* source, double dy, bool stop = false, GdkScrollDirection direction = GDK_SCROLL_SMOOTH) {
        auto event = scroll(source, dy, stop, direction);
        gtk_widget_event(GTK_WIDGET(scrolled), event.get());
    }

    TouchpadZoomGuard guard;
    GtkWidget* window = nullptr;
    GtkScrolledWindow* scrolled = nullptr;
    GtkAdjustment* adjustment = nullptr;
    GdkDevice* pointer = nullptr;
    xoj::util::GObjectSPtr<GdkDevice> touchpad;
    xoj::util::GObjectSPtr<GdkDevice> mouse;
    guint32 timestamp = 1000;
};

TEST_F(TouchpadZoomGuardTest, blocksLargeScrollDeltasAndScrollStopDuringPinch) {
    // Wayland reports pinch and finger scrolling through different source devices.
    guard.begin(scrolled, pointer);
    auto event = scroll(touchpad.get(), 1000);
    event->scroll.delta_x = 1000;
    EXPECT_TRUE(guard.blocksScroll(&event->scroll));
    gtk_widget_event(GTK_WIDGET(scrolled), event.get());
    sendScroll(touchpad.get(), 0, true);
    processEventsFor(100);
    EXPECT_DOUBLE_EQ(gtk_adjustment_get_value(adjustment), 5000);
}

TEST_F(TouchpadZoomGuardTest, suppressesTailThenResumesNormalTouchpadScrolling) {
    guard.begin(scrolled, pointer);
    guard.end();
    sendScroll(touchpad.get(), 1000);
    sendScroll(touchpad.get(), 0, true);
    EXPECT_DOUBLE_EQ(gtk_adjustment_get_value(adjustment), 5000);
    processEventsFor(250);
    sendScroll(touchpad.get(), 1);
    EXPECT_GT(gtk_adjustment_get_value(adjustment), 5000);
}

TEST_F(TouchpadZoomGuardTest, preservesDiscreteWheelsAndSeparateSmoothMouse) {
    guard.begin(scrolled, pointer);
    sendScroll(mouse.get(), 0, false, GDK_SCROLL_DOWN);
    EXPECT_GT(gtk_adjustment_get_value(adjustment), 5000);
    const auto beforeSmooth = gtk_adjustment_get_value(adjustment);
    sendScroll(mouse.get(), 1);
    EXPECT_GT(gtk_adjustment_get_value(adjustment), beforeSmooth);
}

TEST_F(TouchpadZoomGuardTest, handlesPinchReportedThroughLogicalPointerAndResets) {
    guard.begin(scrolled, pointer);
    auto event = scroll(pointer, 1000);
    EXPECT_TRUE(guard.blocksScroll(&event->scroll));
    event->scroll.direction = GDK_SCROLL_DOWN;
    EXPECT_FALSE(guard.blocksScroll(&event->scroll));
    event->scroll.direction = GDK_SCROLL_SMOOTH;
    guard.reset();
    EXPECT_FALSE(guard.blocksScroll(&event->scroll));
    // A late end event after losing focus must not start another cooldown.
    guard.end();
    EXPECT_FALSE(guard.blocksScroll(&event->scroll));
}

TEST_F(TouchpadZoomGuardTest, newPinchDuringCooldownKeepsBlockingUntilItEnds) {
    guard.begin(scrolled, pointer);
    guard.end();
    guard.begin(scrolled, pointer);
    processEventsFor(250);
    auto event = scroll(touchpad.get(), 1000);
    EXPECT_TRUE(guard.blocksScroll(&event->scroll));
    guard.end();
    EXPECT_TRUE(guard.blocksScroll(&event->scroll));
    processEventsFor(250);
    EXPECT_FALSE(guard.blocksScroll(&event->scroll));
}

TEST_F(TouchpadZoomGuardTest, ignoresOrphanedUpdatesAndCancellationAfterFocusLoss) {
    ZoomControl zoom;
    GdkEventTouchpadPinch event{};
    event.type = GDK_TOUCHPAD_PINCH;
    event.n_fingers = 2;
    event.scale = 1.5;
    for (auto phase:
         {GDK_TOUCHPAD_GESTURE_PHASE_UPDATE, GDK_TOUCHPAD_GESTURE_PHASE_END, GDK_TOUCHPAD_GESTURE_PHASE_CANCEL}) {
        event.phase = phase;
        EXPECT_TRUE(onTouchpadPinchEvent(GTK_WIDGET(scrolled), &event, &zoom));
        EXPECT_FALSE(zoom.isZoomSequenceActive());
        EXPECT_DOUBLE_EQ(zoom.getZoom(), 1.0);
    }
}

TEST_F(TouchpadZoomGuardTest, cancelsExistingMomentumWithEitherKineticSetting) {
    for (bool kinetic: {true, false}) {
        guard.reset();
        gtk_scrolled_window_set_kinetic_scrolling(scrolled, kinetic);
        gtk_adjustment_set_value(adjustment, 5000);
        for (int i = 0; i < 5; i++) {
            sendScroll(touchpad.get(), 2);
        }
        sendScroll(touchpad.get(), 0, true);
        const auto beforeMomentum = gtk_adjustment_get_value(adjustment);
        processEventsFor(60);
        EXPECT_GT(gtk_adjustment_get_value(adjustment), beforeMomentum) << "Test must start real GTK momentum";

        guard.begin(scrolled, pointer);
        EXPECT_EQ(static_cast<bool>(gtk_scrolled_window_get_kinetic_scrolling(scrolled)), kinetic);
        const auto stopped = gtk_adjustment_get_value(adjustment);
        processEventsFor(120);
        EXPECT_DOUBLE_EQ(gtk_adjustment_get_value(adjustment), stopped);
    }
}

}  // namespace
