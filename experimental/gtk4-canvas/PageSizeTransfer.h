#pragma once

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <vector>

namespace prototype::bridge {

struct PageSizePacket {
    double width = 595;
    double height = 842;
};

inline constexpr std::size_t maxTransferredPages = 100000;

inline void validate(const PageSizePacket& size) {
    if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0 || size.height <= 0) {
        throw std::runtime_error("Invalid transferred page size");
    }
}

inline std::vector<PageSizePacket> readPageSizeTransfer(const std::filesystem::path& path) {
    std::ifstream stream(path);
    stream.imbue(std::locale::classic());
    std::string magic;
    std::size_t count = 0;
    if (!(stream >> magic >> count) || magic != "XOURNALPP_GTK4_PAGE_SIZES_V1" || count == 0 ||
        count > maxTransferredPages) {
        throw std::runtime_error("Invalid page-size transfer header");
    }

    std::vector<PageSizePacket> sizes;
    sizes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        PageSizePacket size;
        if (!(stream >> size.width >> size.height)) {
            throw std::runtime_error("Incomplete page-size transfer");
        }
        validate(size);
        sizes.push_back(size);
    }
    stream >> std::ws;
    if (!stream.eof()) {
        throw std::runtime_error("Unexpected trailing page-size transfer data");
    }
    return sizes;
}

inline void writePageSizeTransfer(const std::filesystem::path& path,
                                  const std::vector<PageSizePacket>& sizes) {
    if (sizes.empty() || sizes.size() > maxTransferredPages) {
        throw std::runtime_error("Invalid page-size transfer count");
    }
    for (const auto& size: sizes) {
        validate(size);
    }

    std::ofstream stream(path, std::ios::trunc);
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "XOURNALPP_GTK4_PAGE_SIZES_V1 " << sizes.size() << '\n';
    for (const auto& size: sizes) {
        stream << size.width << ' ' << size.height << '\n';
    }
    stream.close();
    if (!stream) {
        throw std::runtime_error("Cannot write page-size transfer");
    }
}

}  // namespace prototype::bridge
