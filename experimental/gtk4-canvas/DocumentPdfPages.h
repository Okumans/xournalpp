#pragma once

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <locale>
#include <vector>

namespace prototype::bridge {

inline constexpr size_t maxPdfPageMapPages = 10000;
inline constexpr size_t maxPdfPageMapBytes = 256 * 1024;

inline std::vector<int> readPdfPageMap(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > maxPdfPageMapBytes) {
        throw std::runtime_error("PDF page map is missing or too large");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open PDF page map");
    }
    std::string magic;
    size_t count = 0;
    stream.imbue(std::locale::classic());
    if (!(stream >> magic >> count) || magic != "XOURNALPP_GTK4_PDF_PAGES_V1" || count > maxPdfPageMapPages) {
        throw std::runtime_error("Invalid PDF page-map header");
    }

    std::vector<int> pages;
    pages.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        long long page = 0;
        if (!(stream >> page) || page < -1 || page > std::numeric_limits<int>::max()) {
            throw std::runtime_error("Invalid PDF page-map entry");
        }
        pages.push_back(static_cast<int>(page));
    }
    stream >> std::ws;
    if (!stream.eof()) {
        throw std::runtime_error("Unexpected trailing PDF page-map data");
    }
    return pages;
}

inline void writePdfPageMap(const std::filesystem::path& path, const std::vector<int>& pages) {
    if (pages.size() > maxPdfPageMapPages) {
        throw std::runtime_error("PDF page map contains too many pages");
    }
    for (int page: pages) {
        if (page < -1) {
            throw std::runtime_error("Invalid PDF page-map entry");
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Cannot create PDF page map");
    }
    stream << "XOURNALPP_GTK4_PDF_PAGES_V1 " << pages.size() << '\n';
    for (int page: pages) {
        stream << page << '\n';
    }
    if (!stream) {
        throw std::runtime_error("Cannot write PDF page map");
    }
}

}  // namespace prototype::bridge
