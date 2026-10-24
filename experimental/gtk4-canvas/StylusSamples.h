#pragma once

#include <gdk/gdk.h>

#include "CanvasInput.h"

namespace prototype {

inline bool isEraserEnd(GdkDeviceToolType tool) { return tool == GDK_DEVICE_TOOL_TYPE_ERASER; }

inline std::optional<InputSample> historySample(const GdkTimeCoord& sample) {
    constexpr auto positionAxes = GDK_AXIS_FLAG_X | GDK_AXIS_FLAG_Y;
    if ((sample.flags & positionAxes) != positionAxes || !std::isfinite(sample.axes[GDK_AXIS_X]) ||
        !std::isfinite(sample.axes[GDK_AXIS_Y])) {
        return std::nullopt;
    }
    std::optional<double> pressure;
    if ((sample.flags & GDK_AXIS_FLAG_PRESSURE) && std::isfinite(sample.axes[GDK_AXIS_PRESSURE]) &&
        sample.axes[GDK_AXIS_PRESSURE] >= 0) {
        pressure = sample.axes[GDK_AXIS_PRESSURE];
    }
    // GtkGestureStylus::get_backlog already returns widget-local positions.
    return InputSample{sample.axes[GDK_AXIS_X], sample.axes[GDK_AXIS_Y], pressure};
}

}  // namespace prototype
