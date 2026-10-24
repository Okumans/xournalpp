#pragma once

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

#include "CanvasImage.h"

namespace prototype::bridge {

inline constexpr size_t maxEditableImages = 10000;
inline constexpr size_t maxImageTransferFileBytes = 128 * 1024 * 1024;

struct ImagePacket {
    int page = 0;
    size_t layerId = 1;
    size_t originLayer = newLayerOrigin;
    size_t layerOrder = 0;
    int pixelWidth = 0;
    int pixelHeight = 0;
    CanvasImageTransform transform;
    std::string encodedData;
};

inline void validate(const ImagePacket& image) {
    const auto& m = image.transform;
    const auto determinant = m.xx * m.yy - m.xy * m.yx;
    if (image.page < 0 || image.layerId == 0 || image.pixelWidth <= 0 || image.pixelHeight <= 0 ||
        static_cast<size_t>(image.pixelWidth) > maxCanvasImagePixels / static_cast<size_t>(image.pixelHeight) ||
        image.encodedData.empty() || image.encodedData.size() > maxCanvasImageBytes ||
        !std::isfinite(determinant) || std::abs(determinant) <= 1e-10 || !std::isfinite(m.xx) ||
        !std::isfinite(m.yx) || !std::isfinite(m.xy) || !std::isfinite(m.yy) || !std::isfinite(m.x) ||
        !std::isfinite(m.y)) {
        throw std::runtime_error("Invalid transferred image");
    }
}

inline ImagePacket imagePacket(const CanvasImage& image) {
    ImagePacket packet;
    packet.page = image.page;
    packet.layerId = image.layerId;
    packet.originLayer = image.originLayer;
    packet.layerOrder = image.layerOrder;
    packet.pixelWidth = image.pixelWidth;
    packet.pixelHeight = image.pixelHeight;
    packet.transform = image.transform;
    packet.encodedData = image.encodedData;
    validate(packet);
    return packet;
}

inline CanvasImage canvasImage(const ImagePacket& packet) {
    validate(packet);
    CanvasImage image;
    image.page = packet.page;
    image.layerId = packet.layerId;
    image.originLayer = packet.originLayer;
    image.layerOrder = packet.layerOrder;
    image.pixelWidth = packet.pixelWidth;
    image.pixelHeight = packet.pixelHeight;
    image.transform = packet.transform;
    image.encodedData = packet.encodedData;
    return image;
}

namespace image_transfer_detail {

inline std::string readLine(std::istream& stream) {
    std::string line;
    char c = 0;
    while (stream.get(c)) {
        if (c == '\n') {
            return line;
        }
        if (line.size() >= 2048) {
            throw std::runtime_error("Image-transfer metadata line is too long");
        }
        line.push_back(c);
    }
    throw std::runtime_error("Incomplete image-transfer metadata");
}

template <typename... Values>
inline void parseLine(const std::string& line, Values&... values) {
    std::istringstream record(line);
    record.imbue(std::locale::classic());
    if (!(record >> ... >> values)) {
        throw std::runtime_error("Invalid image-transfer metadata");
    }
    record >> std::ws;
    if (!record.eof()) {
        throw std::runtime_error("Unexpected image-transfer metadata");
    }
}

inline std::string readBytes(std::istream& stream, size_t length) {
    std::string bytes(length, '\0');
    if (length && !stream.read(bytes.data(), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Incomplete image-transfer data");
    }
    if (stream.get() != '\n') {
        throw std::runtime_error("Invalid image-transfer delimiter");
    }
    return bytes;
}

}  // namespace image_transfer_detail

inline std::vector<ImagePacket> readImageTransfer(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > maxImageTransferFileBytes) {
        throw std::runtime_error("Image transfer exceeds its size limit");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open image transfer");
    }
    stream.imbue(std::locale::classic());
    std::string magic;
    size_t count = 0;
    image_transfer_detail::parseLine(image_transfer_detail::readLine(stream), magic, count);
    if (magic != "XOURNALPP_GTK4_IMAGES_V1" || count > maxEditableImages) {
        throw std::runtime_error("Invalid image-transfer header");
    }
    std::vector<ImagePacket> images;
    images.reserve(count);
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
        ImagePacket image;
        size_t dataLength = 0;
        image_transfer_detail::parseLine(image_transfer_detail::readLine(stream), image.page, image.layerId,
                                        image.originLayer, image.layerOrder, image.pixelWidth, image.pixelHeight,
                                        image.transform.xx, image.transform.yx, image.transform.xy,
                                        image.transform.yy, image.transform.x, image.transform.y, dataLength);
        if (dataLength == 0 || dataLength > maxCanvasImageBytes || dataLength > maxImageTransferFileBytes - bytes) {
            throw std::runtime_error("Image transfer exceeds its data limit");
        }
        image.encodedData = image_transfer_detail::readBytes(stream, dataLength);
        validate(image);
        bytes += dataLength;
        images.push_back(std::move(image));
    }
    stream >> std::ws;
    if (!stream.eof()) {
        throw std::runtime_error("Unexpected trailing image-transfer data");
    }
    return images;
}

inline void writeImageTransfer(const std::filesystem::path& path, const std::vector<ImagePacket>& images) {
    if (images.size() > maxEditableImages) {
        throw std::runtime_error("Image transfer contains too many images");
    }
    size_t bytes = 0;
    for (const auto& image: images) {
        validate(image);
        if (image.encodedData.size() > maxImageTransferFileBytes - bytes) {
            throw std::runtime_error("Image transfer exceeds its data limit");
        }
        bytes += image.encodedData.size();
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Cannot create image transfer");
    }
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "XOURNALPP_GTK4_IMAGES_V1 " << images.size() << '\n';
    for (const auto& image: images) {
        const auto& m = image.transform;
        stream << image.page << ' ' << image.layerId << ' ' << image.originLayer << ' ' << image.layerOrder << ' '
               << image.pixelWidth << ' ' << image.pixelHeight << ' ' << m.xx << ' ' << m.yx << ' ' << m.xy << ' '
               << m.yy << ' ' << m.x << ' ' << m.y << ' ' << image.encodedData.size() << '\n';
        stream.write(image.encodedData.data(), static_cast<std::streamsize>(image.encodedData.size()));
        stream.put('\n');
    }
    stream.close();
    if (!stream) {
        throw std::runtime_error("Cannot write image transfer");
    }
}

inline std::vector<ImagePacket> transferImages(const std::vector<CanvasImage>& images) {
    std::vector<ImagePacket> packets;
    packets.reserve(images.size());
    for (const auto& image: images) {
        packets.push_back(imagePacket(image));
    }
    return packets;
}

}  // namespace prototype::bridge
