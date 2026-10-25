#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace prototype::bridge {

inline constexpr std::size_t maxDocumentPathBytes = 1024 * 1024;

inline void writeDocumentPathTransfer(const std::filesystem::path& path, const std::string& value) {
    if (value.size() > maxDocumentPathBytes) {
        throw std::runtime_error("Document path exceeds its size limit");
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Cannot create document-path transfer");
    }
    output << "XOURNALPP_GTK4_DOCUMENT_PATH_V1 " << value.size() << '\n';
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("Cannot write document-path transfer");
    }
}

inline std::string readDocumentPathTransfer(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string magic;
    std::size_t length = 0;
    if (!input || !(input >> magic >> length) || magic != "XOURNALPP_GTK4_DOCUMENT_PATH_V1" ||
        length > maxDocumentPathBytes || input.get() != '\n') {
        throw std::runtime_error("Invalid document-path transfer header");
    }

    std::string value(length, '\0');
    if (length && !input.read(value.data(), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Incomplete document-path transfer");
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Unexpected trailing document-path transfer data");
    }
    return value;
}

}  // namespace prototype::bridge
