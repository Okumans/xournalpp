#pragma once

#include <string>
#include <vector>

#include "CanvasImage.h"

namespace prototype {

enum class CanvasImageAction : int {
    Invert = 1,
    Downscale,
    FlipHorizontal,
    FlipVertical,
    RotateClockwise,
    RotateCounterclockwise,
    MakeWhiteTransparent,
    Trim,
    SplitVertical,
    SplitHorizontal,
};

bool canvasImageActionsAvailable();

bool transformCanvasImage(const CanvasImage& image, CanvasImageAction action, double pageWidth, double pageHeight,
                          std::vector<CanvasImage>& results, std::string& error);

}  // namespace prototype
