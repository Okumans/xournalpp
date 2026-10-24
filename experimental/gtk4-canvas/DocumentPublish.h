#pragma once

#include <exception>
#include <functional>

#include "DocumentVersion.h"

namespace prototype::bridge {

// Owns only its newly created directory. Keep it if rollback cannot restore an
// attachment, so error reporting can point to recoverable original files.
class SaveStaging {
public:
    explicit SaveStaging(const fs::path& parent) {
        std::string pattern = (parent / ".xournalpp-gtk4-save-XXXXXX").string();
        if (!g_mkdtemp(pattern.data())) {
            throw std::runtime_error("Cannot create private save staging directory");
        }
        path = pattern;
    }
    ~SaveStaging() {
        if (!retain) {
            std::error_code error;
            fs::remove_all(path, error);
        }
    }
    SaveStaging(const SaveStaging&) = delete;
    SaveStaging& operator=(const SaveStaging&) = delete;

    fs::path path;
    bool retain = false;
};

inline void requireVersion(const fs::path& target, const std::string& expected) {
    if (documentVersion(target).token != expected) {
        throw std::runtime_error("The document or its attachments changed outside this window. Reopen it or use Save As.");
    }
}

// The optional callback exercises pre-commit failures in the filesystem tests.
// Production callers leave it empty. The existing document is replaced last.
inline void publishDocument(SaveStaging& staging, const fs::path& target, const std::string& expected,
                            const std::function<void()>& beforeCommit = {}) {
    const auto version = documentVersion(target);
    if (version.token != expected) {
        throw std::runtime_error("The document or its attachments changed outside this window. Reopen it or use Save As.");
    }
    const auto staged = staging.path / target.filename();
    if (!fs::is_regular_file(staged)) {
        throw std::runtime_error("Staged document is missing");
    }
    const bool replacing = version.exists;
    const auto originalDigest = version.primaryDigest;
    const auto backup = fs::path(target.string() + "~");
    if (replacing && existsWithoutFollowing(backup)) {
        throw std::runtime_error("A backup already exists: " + backup.string() + ". Reopen it or use Save As.");
    }
    struct Asset {
        fs::path staged, target, original;
        bool existed = false;
        bool published = false;
        std::string publishedDigest;
    };
    std::vector<Asset> assets;
    const auto rollback = staging.path / "rollback";
    for (const auto& entry: fs::directory_iterator(staging.path)) {
        if (entry.path() != staged) {
            const auto destination = target.parent_path() / entry.path().filename();
            const bool existed = existsWithoutFollowing(destination);
            if (existed) {
                if (!replacing) {
                    throw std::runtime_error("An attached background already exists: " + destination.string());
                }
                fileDigest(destination);  // Reject symlinks and nonregular destinations before changing anything.
            }
            assets.push_back({entry.path(), destination, rollback / entry.path().filename(), existed, false,
                              fileDigest(entry.path())});
        }
    }
    requireVersion(target, expected);
    if (replacing) {
        fs::permissions(staged, fs::status(target).permissions());
        fs::copy_file(target, backup);
        if (fileDigest(backup) != originalDigest) {
            throw std::runtime_error("The document changed while creating its backup. Use Save As.");
        }
    }
    bool committed = false;
    try {
        fs::create_directory(rollback);
        for (auto& asset: assets) {
            if (asset.existed) {
                fs::copy_file(asset.target, asset.original);
                fs::permissions(asset.staged, fs::status(asset.target).permissions());
            }
        }
        requireVersion(target, expected);
        for (auto& asset: assets) {
            if (asset.existed) {
                fs::rename(asset.staged, asset.target);
            } else {
                fs::create_hard_link(asset.staged, asset.target);
            }
            asset.published = true;
        }
        if (beforeCommit) {
            beforeCommit();
        }
        for (const auto& asset: assets) {
            if (fileDigest(asset.target) != asset.publishedDigest) {
                throw std::runtime_error("An attachment changed while saving. Its new content was not overwritten.");
            }
        }
        if (replacing) {
            if (fileDigest(target) != originalDigest) {
                throw std::runtime_error("The document changed while saving. Its new content was not overwritten.");
            }
            fs::rename(staged, target);
        } else {
            fs::create_hard_link(staged, target);
        }
        committed = true;
    } catch (...) {
        const auto error = std::current_exception();
        bool restored = true;
        for (auto asset = assets.rbegin(); asset != assets.rend(); ++asset) {
            if (!asset->published) {
                continue;
            }
            try {
                if (fileDigest(asset->target) != asset->publishedDigest) {
                    throw std::runtime_error("An attachment changed during rollback");
                }
                if (asset->existed) {
                    fs::rename(asset->original, asset->target);
                } else {
                    fs::remove(asset->target);
                }
            } catch (...) {
                restored = false;
            }
        }
        if (!restored) {
            staging.retain = true;
            const auto detail = replacing ? ". The document backup is " + backup.string() : std::string{};
            throw std::runtime_error("Save failed; recovery files are retained in " + staging.path.string() + detail);
        }
        // Leave a recoverable backup after a failed replacement, like SaveJob.
        std::rethrow_exception(error);
    }
    if (committed && replacing) {
        // A cleanup failure must not report a committed save as unsaved.
        std::error_code error;
        fs::remove(backup, error);
    }
}

}  // namespace prototype::bridge
