#pragma once

#include <string>
#include <vector>

#include "CanvasState.h"

namespace prototype::clipboard {

struct RenderedSelection {
    std::string png;
    std::string svg;
    std::string plainText;
};

std::string renderImagePng(const CanvasImage& image);
RenderedSelection renderSelection(const std::vector<EditableObject>& objects, const SelectionBox& selectionBounds);

}  // namespace prototype::clipboard
