#pragma once

#include <optional>
#include <string>

namespace prototype {

struct DocumentExportOptions {
    std::string outputFile;
    std::optional<std::string> range;
    std::optional<std::string> layerRange;
    std::string background = "all";
    bool progressiveMode = false;
    std::string backend = "default";
    int pngDpi = -1;
    int pngWidth = -1;
    int pngHeight = -1;
};

}  // namespace prototype
