#include "TouchpadZoomGuard.h"

namespace {
constexpr gint64 SCROLL_COOLDOWN = 200 * G_TIME_SPAN_MILLISECOND;
}

void TouchpadZoomGuard::begin(GtkScrolledWindow* window, GdkDevice* device) {
    this->pinchDevice = xoj::util::GObjectSPtr<GdkDevice>(device, xoj::util::ref);
    this->active = true;
    this->blockUntil = 0;

    // GTK 3 has no public stop-deceleration API. Switching kinetic scrolling off
    // cancels its running animation. Force that transition even if touchscreen
    // kinetic scrolling was disabled: touchpad momentum can still be running.
    const bool kinetic = gtk_scrolled_window_get_kinetic_scrolling(window);
    if (!kinetic) {
        gtk_scrolled_window_set_kinetic_scrolling(window, true);
    }
    gtk_scrolled_window_set_kinetic_scrolling(window, false);
    if (kinetic) {
        gtk_scrolled_window_set_kinetic_scrolling(window, true);
    }
}

void TouchpadZoomGuard::end() {
    if (this->active) {
        this->active = false;
        this->blockUntil = g_get_monotonic_time() + SCROLL_COOLDOWN;
    }
}

void TouchpadZoomGuard::reset() {
    this->active = false;
    this->blockUntil = 0;
    this->pinchDevice = nullptr;
}

bool TouchpadZoomGuard::blocksScroll(const GdkEventScroll* event) const {
    if (!this->active && g_get_monotonic_time() >= this->blockUntil) {
        return false;
    }

    // Discrete wheel events must keep working, including on backends that use
    // the same logical pointer for a mouse and a touchpad.
    if (event->direction != GDK_SCROLL_SMOOTH) {
        return false;
    }

    auto* source = gdk_event_get_source_device(reinterpret_cast<const GdkEvent*>(event));
    // Some backends expose a pinch through a logical pointer rather than a
    // device classified as a touchpad. Match that source as well.
    return source && (gdk_device_get_source(source) == GDK_SOURCE_TOUCHPAD || source == this->pinchDevice.get());
}
