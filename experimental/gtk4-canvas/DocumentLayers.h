#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>

#include "CanvasLayers.h"

namespace prototype::bridge {

inline constexpr size_t maxLayerPages = 10000;
inline constexpr size_t maxLayersPerPage = 1000;
inline constexpr size_t maxLayerOrigins = 10000;
inline constexpr size_t maxLayerNameBytes = 4096;
inline constexpr size_t maxLayerTransferBytes = 64 * 1024 * 1024;

using LayerInfo = prototype::CanvasLayer;
using PageLayerInfo = prototype::PageLayers;
using LayerDocument = std::vector<PageLayerInfo>;

namespace layer_transfer_detail {

inline std::string readLine(std::istream& stream) {
    std::string line;
    char c = 0;
    while (stream.get(c)) {
        if (c == '\n') {
            return line;
        }
        if (line.size() >= 65536) {
            throw std::runtime_error("Layer-transfer record is too long");
        }
        line.push_back(c);
    }
    throw std::runtime_error("Incomplete layer-transfer data");
}

template <typename... Values>
inline void parseLine(const std::string& line, Values&... values) {
    std::istringstream record(line);
    record.imbue(std::locale::classic());
    if (!(record >> ... >> values)) {
        throw std::runtime_error("Invalid layer-transfer metadata");
    }
    record >> std::ws;
    if (!record.eof()) {
        throw std::runtime_error("Unexpected layer-transfer metadata");
    }
}

inline std::string readBytes(std::istream& stream, size_t length) {
    std::string bytes(length, '\0');
    if (length && !stream.read(bytes.data(), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Incomplete layer name");
    }
    if (stream.get() != '\n') {
        throw std::runtime_error("Invalid layer-name delimiter");
    }
    return bytes;
}

inline void validate(const LayerDocument& pages) {
    if (pages.size() > maxLayerPages) {
        throw std::runtime_error("Layer transfer contains too many pages");
    }
    size_t totalOrigins = 0;
    for (const auto& page: pages) {
        if (page.initialLayerCount == 0 || page.initialLayerCount > maxLayersPerPage || page.layers.empty() ||
            page.layers.size() > maxLayersPerPage || page.selectedLayer == 0 ||
            page.selectedLayer > page.layers.size() || !std::isfinite(page.gridYOffset)) {
            throw std::runtime_error("Invalid page layer metadata");
        }
        if (page.backgroundName &&
            (page.backgroundName->size() > maxLayerNameBytes ||
             !g_utf8_validate(page.backgroundName->data(), page.backgroundName->size(), nullptr))) {
            throw std::runtime_error("Invalid background layer name");
        }
        for (const auto& layer: page.layers) {
            if (layer.origins.empty() || layer.name.size() > maxLayerNameBytes ||
                !g_utf8_validate(layer.name.data(), layer.name.size(), nullptr) ||
                layer.origins.size() > maxLayerOrigins - totalOrigins) {
                throw std::runtime_error("Invalid layer name or origin count");
            }
            totalOrigins += layer.origins.size();
            std::vector<size_t> layerOrigins;
            for (size_t origin: layer.origins) {
                if (origin == newLayerOrigin || origin > page.initialLayerCount + maxLayerOrigins) {
                    throw std::runtime_error("Invalid layer origin");
                }
                layerOrigins.push_back(origin);
            }
            std::sort(layerOrigins.begin(), layerOrigins.end());
            if (std::adjacent_find(layerOrigins.begin(), layerOrigins.end()) != layerOrigins.end()) {
                throw std::runtime_error("A layer contains the same source origin more than once");
            }
        }
    }
}

}  // namespace layer_transfer_detail

inline LayerDocument readLayerTransfer(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > maxLayerTransferBytes) {
        throw std::runtime_error("Layer transfer exceeds its size limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open layer transfer");
    }
    stream.imbue(std::locale::classic());
    std::string magic;
    size_t count = 0;
    layer_transfer_detail::parseLine(layer_transfer_detail::readLine(stream), magic, count);
    const bool version1 = magic == "XOURNALPP_GTK4_LAYERS_V1";
    const bool version2 = magic == "XOURNALPP_GTK4_LAYERS_V2";
    const bool version3 = magic == "XOURNALPP_GTK4_LAYERS_V3";
    if ((!version1 && !version2 && !version3) || count > maxLayerPages) {
        throw std::runtime_error("Invalid layer-transfer header");
    }

    LayerDocument pages;
    pages.reserve(count);
    size_t totalBytes = 64;
    for (size_t i = 0; i < count; ++i) {
        PageLayerInfo page;
        int backgroundVisible = 0;
        size_t layerCount = 0;
        if (version3) {
            layer_transfer_detail::parseLine(layer_transfer_detail::readLine(stream), page.initialLayerCount,
                                            page.selectedLayer, backgroundVisible, page.gridYOffset, layerCount);
        } else {
            layer_transfer_detail::parseLine(layer_transfer_detail::readLine(stream), page.initialLayerCount,
                                            page.selectedLayer, backgroundVisible, layerCount);
        }
        if (backgroundVisible < 0 || backgroundVisible > 1 || layerCount == 0 || layerCount > maxLayersPerPage) {
            throw std::runtime_error("Invalid page layer-transfer header");
        }
        page.backgroundVisible = backgroundVisible != 0;
        if (version2 || version3) {
            int hasBackgroundName = 0;
            size_t nameBytes = 0;
            layer_transfer_detail::parseLine(layer_transfer_detail::readLine(stream), hasBackgroundName, nameBytes);
            if ((hasBackgroundName != 0 && hasBackgroundName != 1) || nameBytes > maxLayerNameBytes ||
                (!hasBackgroundName && nameBytes != 0)) {
                throw std::runtime_error("Invalid background layer name");
            }
            if (hasBackgroundName) {
                page.backgroundName = layer_transfer_detail::readBytes(stream, nameBytes);
            }
            totalBytes += nameBytes;
            if (totalBytes > maxLayerTransferBytes) {
                throw std::runtime_error("Layer transfer exceeds its size limit");
            }
        }
        page.layers.clear();
        page.layers.reserve(layerCount);
        for (size_t n = 0; n < layerCount; ++n) {
            LayerInfo layer;
            int visible = 0;
            size_t nameBytes = 0;
            size_t originCount = 0;
            layer_transfer_detail::parseLine(layer_transfer_detail::readLine(stream), visible, nameBytes, originCount);
            if ((visible != 0 && visible != 1) || nameBytes > maxLayerNameBytes || originCount > maxLayerOrigins) {
                throw std::runtime_error("Invalid layer-transfer entry");
            }
            layer.visible = visible != 0;
            layer.origins.reserve(originCount);
            for (size_t origin = 0; origin < originCount; ++origin) {
                size_t id = 0;
                layer_transfer_detail::parseLine(layer_transfer_detail::readLine(stream), id);
                layer.origins.push_back(id);
            }
            layer.name = layer_transfer_detail::readBytes(stream, nameBytes);
            totalBytes += nameBytes;
            if (totalBytes > maxLayerTransferBytes) {
                throw std::runtime_error("Layer transfer exceeds its size limit");
            }
            page.layers.push_back(std::move(layer));
        }
        pages.push_back(std::move(page));
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Unexpected trailing layer-transfer data");
    }
    layer_transfer_detail::validate(pages);
    return pages;
}

inline void writeLayerTransfer(const std::filesystem::path& path, const LayerDocument& pages) {
    layer_transfer_detail::validate(pages);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Cannot create layer transfer");
    }
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "XOURNALPP_GTK4_LAYERS_V3 " << pages.size() << '\n';
    for (const auto& page: pages) {
        stream << page.initialLayerCount << ' ' << page.selectedLayer << ' ' << page.backgroundVisible << ' '
               << page.gridYOffset << ' ' << page.layers.size() << '\n';
        const size_t backgroundNameBytes = page.backgroundName ? page.backgroundName->size() : 0;
        stream << page.backgroundName.has_value() << ' ' << backgroundNameBytes << '\n';
        if (page.backgroundName) {
            stream.write(page.backgroundName->data(), static_cast<std::streamsize>(backgroundNameBytes));
            stream.put('\n');
        }
        for (const auto& layer: page.layers) {
            stream << layer.visible << ' ' << layer.name.size() << ' ' << layer.origins.size() << '\n';
            for (size_t origin: layer.origins) {
                stream << origin << '\n';
            }
            stream.write(layer.name.data(), static_cast<std::streamsize>(layer.name.size()));
            stream.put('\n');
        }
    }
    stream.close();
    if (!stream) {
        throw std::runtime_error("Cannot write layer transfer");
    }
}

}  // namespace prototype::bridge
