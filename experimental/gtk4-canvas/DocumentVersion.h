#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>

namespace prototype::bridge {

namespace fs = std::filesystem;

inline bool existsWithoutFollowing(const fs::path& path) { return fs::exists(fs::symlink_status(path)); }

inline std::string fileDigest(const fs::path& path) {
    if (!fs::is_regular_file(fs::symlink_status(path))) {
        throw std::runtime_error("Save targets and attached backgrounds must be regular files");
    }
    std::ifstream file(path, std::ios::binary);
    std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
    std::array<char, 65536> buffer;
    while (file.read(buffer.data(), buffer.size()) || file.gcount()) {
        g_checksum_update(checksum.get(), reinterpret_cast<const guchar*>(buffer.data()), file.gcount());
    }
    if (!file.eof()) {
        throw std::runtime_error("Cannot read file while checking its saved version: " + path.string());
    }
    return g_checksum_get_string(checksum.get());
}

inline fs::path pdfCompanion(fs::path document) {
    while (g_ascii_strcasecmp(document.extension().string().c_str(), ".xopp") == 0 ||
           g_ascii_strcasecmp(document.extension().string().c_str(), ".xoj") == 0) {
        document.replace_extension();
    }
    document += ".xopp.bg.pdf";
    return document;
}

struct DocumentVersion {
    bool exists;
    std::string token;
    std::string primaryDigest;
};

// Include the attached PDF and pixmap sidecars that the real saver can replace.
// No timestamp-only comparisons: equal-size edits and replaced inodes still count.
inline DocumentVersion documentVersion(const fs::path& path) {
    const bool exists = existsWithoutFollowing(path);
    std::vector<fs::path> assets;
    const auto pdf = pdfCompanion(path);
    if (existsWithoutFollowing(pdf)) {
        assets.push_back(pdf);
    }
    const auto prefix = path.filename().string() + ".bg_";
    if (fs::is_directory(path.parent_path())) {
        for (const auto& entry: fs::directory_iterator(path.parent_path())) {
            const auto name = entry.path().filename().string();
            if (name.starts_with(prefix) && name.ends_with(".png")) {
                assets.push_back(entry.path());
            }
        }
    }
    std::sort(assets.begin(), assets.end());
    std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
    auto add = [&](const std::string& value) {
        g_checksum_update(checksum.get(), reinterpret_cast<const guchar*>(value.c_str()), value.size() + 1);
    };
    const auto primary = exists ? fileDigest(path) : std::string("missing");
    add(primary);
    for (const auto& asset: assets) {
        add(asset.filename().string());
        add(fileDigest(asset));
    }
    return {exists, g_checksum_get_string(checksum.get()), primary};
}

}  // namespace prototype::bridge
