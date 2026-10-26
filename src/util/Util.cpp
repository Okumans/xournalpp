#include "util/Util.h"

#include <array>    // for array
#include <atomic>   // for atomic
#include <cstdlib>  // for system
#include <memory>   // for unique_ptr
#include <string>   // for allocator, string
#include <utility>  // for move
#include <vector>   // for vector

#include <gdk/gdk.h>  // for gdk_cairo_set_source_rgba, gdk_t...

#include "util/Color.h"              // for argb_to_GdkRGBA, rgb_to_GdkRGBA
#include "util/OutputStream.h"       // for OutputStream
#include "util/PlaceholderString.h"  // for PlaceholderString
#include "util/i18n.h"               // for FS, _F

#if defined(_MSC_VER)
#include <windows.h>
#else
#include <unistd.h>  // for getpid, pid_t
#endif

namespace {
std::atomic<Util::ErrorReporter> errorReporter{nullptr};

struct PendingError {
    Util::ErrorReporter reporter;
    std::string message;
};

gboolean dispatchError(gpointer data) {
    std::unique_ptr<PendingError> error(static_cast<PendingError*>(data));
    error->reporter(error->message);
    return G_SOURCE_REMOVE;
}
}  // namespace


void Util::cairo_set_source_rgbi(cairo_t* cr, Color color, double alpha) {
    auto rgba = argb_to_GdkRGBA(color, alpha);
    gdk_cairo_set_source_rgba(cr, &rgba);
}

void Util::cairo_set_source_argb(cairo_t* cr, Color color) {
    auto rgba = argb_to_GdkRGBA(color);
    gdk_cairo_set_source_rgba(cr, &rgba);
}

auto Util::getPid() -> PID {
#if defined(_MSC_VER)
    return GetCurrentProcessId();
#else
    return ::getpid();
#endif
}

auto Util::paintBackgroundWhite(GtkWidget* widget, cairo_t* cr, void*) -> gboolean {
#if GTK_CHECK_VERSION(4, 0, 0)
    const int width = gtk_widget_get_width(widget);
    const int height = gtk_widget_get_height(widget);
#else
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
#endif
    cairo_set_source_rgb(cr, 1, 1, 1);
#if GTK_CHECK_VERSION(4, 0, 0)
    cairo_rectangle(cr, 0, 0, width, height);
#else
    cairo_rectangle(cr, 0, 0, alloc.width, alloc.height);
#endif
    cairo_fill(cr);
    return false;
}

void Util::cairo_set_dash_from_vector(cairo_t* cr, const std::vector<double>& dashes, double offset) {
    cairo_set_dash(cr, dashes.data(), static_cast<int>(dashes.size()), offset);
}

void Util::writeCoordinateString(OutputStream* out, double xVal, double yVal) {
    std::array<char, G_ASCII_DTOSTR_BUF_SIZE> coordString{};
    g_ascii_formatd(coordString.data(), G_ASCII_DTOSTR_BUF_SIZE, Util::PRECISION_FORMAT_STRING, xVal);
    out->write(coordString.data());
    out->write(" ");
    g_ascii_formatd(coordString.data(), G_ASCII_DTOSTR_BUF_SIZE, Util::PRECISION_FORMAT_STRING, yVal);
    out->write(coordString.data());
}

void Util::systemWithMessage(const char* command) {
    if (auto errc = std::system(command); errc != 0) {
        std::string msg = FS(_F("Error {1} executing system command: {2}") % errc % command);
        reportErrorToUser(msg);
    }
}

void Util::setErrorReporter(ErrorReporter reporter) { errorReporter.store(reporter); }

void Util::reportErrorToUser(const std::string& message) {
    const auto reporter = errorReporter.load();
    if (!reporter) {
        g_warning("%s", message.c_str());
        return;
    }

    auto* pending = new PendingError{reporter, message};
    g_idle_add_full(G_PRIORITY_DEFAULT, dispatchError, pending, nullptr);
}

bool Util::isFlatpakInstallation() { return fs::exists("/.flatpak-info"); }
