#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace prototype {

inline constexpr size_t newLayerOrigin = std::numeric_limits<size_t>::max();
inline constexpr size_t legacyTopLayer = std::numeric_limits<size_t>::max() - 1;

struct CanvasLayer {
    std::string name;
    bool visible = true;
    std::vector<size_t> origins;

    bool operator==(const CanvasLayer&) const = default;
};

struct PageLayers {
    size_t initialLayerCount = 1;
    size_t selectedLayer = 1;
    bool backgroundVisible = true;
    std::optional<std::string> backgroundName;
    std::vector<CanvasLayer> layers{{"Layer 1", true, {0}}};

    bool operator==(const PageLayers&) const = default;
};

}  // namespace prototype
