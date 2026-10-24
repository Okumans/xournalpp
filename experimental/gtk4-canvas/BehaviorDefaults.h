#pragma once

namespace prototype::behavior {

// Baseline: Settings::loadDefault, Control's percent conversion, and StrokeView.
inline constexpr double minimumPressure = 0.05;
inline constexpr double pressureMultiplier = 1.0;
inline constexpr double zoomStep = 0.10;
inline constexpr double scrollZoomStep = 0.02;
inline constexpr double highlightOpacity = 0.47;
inline constexpr double motionThreshold = 0.3;  // InputHandler::PIXEL_MOTION_THRESHOLD

inline double steppedZoom(double zoom, bool zoomIn, bool scroll) {
    const auto factor = 1 + (scroll ? scrollZoomStep : zoomStep);
    return zoomIn ? zoom * factor : zoom / factor;
}

}  // namespace prototype::behavior
