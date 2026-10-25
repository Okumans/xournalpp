#pragma once

#include <charconv>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace prototype::bridge {

inline constexpr std::size_t maxPdfPageLabels = 10000;
inline constexpr std::size_t maxPdfPageLabelBytes = 1024 * 1024;
inline constexpr std::size_t maxPdfPageLabelLength = 65536;

inline std::size_t parsePdfPageLabelInteger(std::string_view value, const char* error) {
    std::size_t parsed = 0;
    const auto [end, result] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error(error);
    }
    return parsed;
}

inline std::vector<std::string> readPdfPageLabels(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > maxPdfPageLabelBytes) {
        throw std::runtime_error("PDF page labels are missing or too large");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open PDF page labels");
    }
    std::string header;
    if (!std::getline(stream, header) || header.rfind("XOURNALPP_GTK4_PDF_LABELS_V1 ", 0) != 0) {
        throw std::runtime_error("Invalid PDF page-label header");
    }
    const auto count = parsePdfPageLabelInteger(std::string_view(header).substr(29), "Invalid PDF page-label count");
    if (count > maxPdfPageLabels) {
        throw std::runtime_error("PDF page-label count exceeds the supported limit");
    }

    std::vector<std::string> labels;
    labels.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::string lengthLine;
        if (!std::getline(stream, lengthLine)) {
            throw std::runtime_error("Truncated PDF page-label length");
        }
        const auto length = parsePdfPageLabelInteger(lengthLine, "Invalid PDF page-label length");
        if (length > maxPdfPageLabelLength) {
            throw std::runtime_error("PDF page label exceeds the supported limit");
        }

        std::string label(length, '\0');
        if (length && !stream.read(label.data(), static_cast<std::streamsize>(length))) {
            throw std::runtime_error("Truncated PDF page label");
        }
        char newline = 0;
        if (!stream.get(newline) || newline != '\n') {
            throw std::runtime_error("Invalid PDF page-label terminator");
        }
        labels.push_back(std::move(label));
    }
    if (stream.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Unexpected trailing PDF page-label data");
    }
    return labels;
}

inline void writePdfPageLabels(const std::filesystem::path& path, const std::vector<std::string>& labels) {
    if (labels.size() > maxPdfPageLabels) {
        throw std::runtime_error("PDF page-label count exceeds the supported limit");
    }
    std::size_t totalBytes =
            std::string_view("XOURNALPP_GTK4_PDF_LABELS_V1 ").size() + 1 + std::to_string(labels.size()).size();
    for (const auto& label: labels) {
        if (label.size() > maxPdfPageLabelLength) {
            throw std::runtime_error("PDF page label exceeds the supported limit");
        }
        totalBytes += std::to_string(label.size()).size() + 1 + label.size() + 1;
        if (totalBytes > maxPdfPageLabelBytes) {
            throw std::runtime_error("PDF page labels exceed the supported size limit");
        }
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Cannot create PDF page labels");
    }
    stream << "XOURNALPP_GTK4_PDF_LABELS_V1 " << labels.size() << '\n';
    for (const auto& label: labels) {
        stream << label.size() << '\n';
        stream.write(label.data(), static_cast<std::streamsize>(label.size()));
        stream.put('\n');
    }
    if (!stream) {
        throw std::runtime_error("Cannot write PDF page labels within the supported size limit");
    }
}

}  // namespace prototype::bridge
