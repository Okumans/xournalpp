#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace prototype::behavior {

// Baseline: Settings::loadDefault, ToolHandler::initTools, Control's percent conversion, and StrokeView.
enum class ToolSize : size_t {
    VeryFine = 0,
    Fine = 1,
    Medium = 2,
    Thick = 3,
    VeryThick = 4,
};

inline constexpr double minimumPressure = 0.05;
inline constexpr double pressureMultiplier = 1.0;
inline constexpr double zoomStep = 0.10;
inline constexpr double scrollZoomStep = 0.02;
// Preserve the existing mouse-wheel movement while smooth surface deltas stay 1:1.
inline constexpr double discreteScrollStepPixels = 35.0;
inline constexpr double snapGridSize = 14.17;
inline constexpr double snapGridTolerance = 0.50;
inline constexpr double highlightOpacity = 0.47;
inline constexpr unsigned int laserPointerFadeOutTimeMs = 500;
inline constexpr unsigned int laserPointerFadeStepMs = 50;
inline constexpr unsigned int laserPointerFadeAlphaStep = 25;
inline constexpr double motionThreshold = 0.3;              // InputHandler::PIXEL_MOTION_THRESHOLD
inline constexpr double eraserPaddingCapRound = 0.4;        // EraseHandler::PADDING_COEFFICIENT_CAP[ROUND]
inline constexpr double defaultPageGap = 15.0;              // Layout.cpp: XOURNAL_PADDING_BETWEEN = 15
inline constexpr unsigned int autosaveTimeoutSeconds = 180;  // Settings::loadDefault: 3 minutes

inline constexpr std::array<double, 5> penWidths = {0.42, 0.85, 1.41, 2.26, 5.67};
inline constexpr std::array<double, 5> highlighterWidths = {1.0, 2.83, 8.50, 19.84, 30.0};
inline constexpr std::array<double, 5> eraserWidths = {1.0, 2.83, 8.50, 12.0, 18.0};
inline constexpr std::array<double, 5> laserPenWidths = {0.7, 1.41, 2.4, 4.0, 7.0};
inline constexpr std::array<double, 5> laserHighlighterWidths = {1.0, 2.83, 8.50, 19.84, 30.0};

inline constexpr std::array<double, 3> defaultPenColor = {0x33 / 255.0, 0x33 / 255.0, 0xcc / 255.0};  // #3333cc
inline constexpr std::array<double, 3> defaultHighlighterColor = {1.0, 1.0, 0.0};                      // #ffff00
inline constexpr std::array<double, 3> whiteoutColor = {1.0, 1.0, 1.0};                                // #ffffff
inline constexpr std::array<double, 3> laserPointerColor = {1.0, 0.0, 0.0};                            // #ff0000

inline std::optional<ToolSize> matchToolSize(const std::array<double, 5>& widths, double width) {
    for (size_t i = 0; i < widths.size(); ++i) {
        if (std::abs(widths[i] - width) < 0.005) {
            return static_cast<ToolSize>(i);
        }
    }
    return std::nullopt;
}

inline double steppedZoom(double zoom, bool zoomIn, bool scroll, double step = zoomStep,
                          double scrollStep = scrollZoomStep) {
    const auto factor = 1 + (scroll ? scrollStep : step);
    return zoomIn ? zoom * factor : zoom / factor;
}

inline double scrollDeltaToPixels(double delta, bool discrete) {
    return discrete ? delta * discreteScrollStepPixels : delta;
}

}  // namespace prototype::behavior
