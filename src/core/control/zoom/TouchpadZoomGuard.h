#pragma once

#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "util/raii/GObjectSPtr.h"

/** Keep touchpad scrolling and its momentum from moving the document during a pinch. */
class TouchpadZoomGuard {
public:
    void begin(GtkScrolledWindow* window, GdkDevice* device);
    void end();
    void reset();
    bool isActive() const { return active; }
    bool blocksScroll(const GdkEventScroll* event) const;

private:
    bool active = false;
    gint64 blockUntil = 0;
    xoj::util::GObjectSPtr<GdkDevice> pinchDevice;
};
