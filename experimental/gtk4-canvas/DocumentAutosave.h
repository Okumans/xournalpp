#pragma once

#include <cerrno>
#include <charconv>
#include <csignal>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "DocumentPublish.h"

namespace prototype::bridge {

inline fs::path clearXournalExtensions(fs::path path) {
    auto strip = [&path](const char* extension) {
        if (g_ascii_strcasecmp(path.extension().string().c_str(), extension) == 0) {
            path.replace_extension();
        }
    };
    strip(".xoj");
    strip(".xopp");
    return path;
}

inline bool isAutosaveFilename(const fs::path& path) {
    const auto name = path.filename().string();
    constexpr std::string_view suffix = ".autosave.xopp";
    return name.size() >= suffix.size() &&
           g_ascii_strcasecmp(name.c_str() + (name.size() - suffix.size()), suffix.data()) == 0;
}

inline bool isRecoveryArtifact(const fs::path& path) {
    return isAutosaveFilename(path) || path.filename() == "emergencysave.xopp";
}

// Matches AutosaveJob::run and Util::getAutosaveFilepath in GTK 3 Xournal++.
inline fs::path autosavePathForTarget(const fs::path& saveTarget, const fs::path& cacheRoot, long long pid) {
    if (saveTarget.empty()) {
        return cacheRoot / "xournalpp" / "autosaves" / (std::to_string(pid) + ".autosave.xopp");
    }
    auto hidden = saveTarget;
    hidden.replace_filename(fs::path(".") += saveTarget.filename());
    hidden = clearXournalExtensions(std::move(hidden));
    hidden += ".autosave.xopp";
    return hidden;
}

// Matches XournalMain.cpp checkForEmergencySave: Util::getConfigFile("emergencysave.xopp").
inline fs::path emergencySavePath(const fs::path& configRoot) {
    return configRoot / "xournalpp" / "emergencysave.xopp";
}

// Matches Control.cpp shouldFileOpen: refuse direct Open inside <cache>/xournalpp.
inline bool isCacheProtectedPath(const fs::path& candidate, const fs::path& cacheRoot) {
    if (candidate.empty() || cacheRoot.empty()) {
        return false;
    }
    auto canonicalOrLexical = [](const fs::path& p) {
        std::error_code error;
        auto resolved = fs::weakly_canonical(p, error);
        return error ? fs::absolute(p).lexically_normal() : resolved;
    };
    const auto base = canonicalOrLexical(cacheRoot / "xournalpp");
    const auto target = canonicalOrLexical(candidate);
    const auto relative = target.lexically_relative(base);
    return !relative.empty() && *relative.begin() != "..";
}

inline bool defaultPidAlive(long long pid) {
    if (pid <= 0) {
        return false;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
}

inline std::optional<long long> autosaveOwnerPid(const fs::path& path) {
    if (!isAutosaveFilename(path)) {
        return std::nullopt;
    }
    const auto name = path.filename().string();
    constexpr std::string_view suffix = ".autosave.xopp";
    const std::string_view prefix(name.data(), name.size() - suffix.size());
    if (prefix.empty()) {
        return std::nullopt;
    }
    long long pid = 0;
    const auto [end, ec] = std::from_chars(prefix.data(), prefix.data() + prefix.size(), pid);
    if (ec != std::errc{} || end != prefix.data() + prefix.size() || pid <= 0) {
        return std::nullopt;
    }
    return pid;
}

// Check emergency save first, then orphaned cache autosaves from terminated processes.
inline std::optional<fs::path> findStartupRecoveryFile(
        const fs::path& configRoot, const fs::path& cacheRoot, long long currentPid,
        const std::function<bool(long long)>& pidAlive = defaultPidAlive) {
    const auto emergency = emergencySavePath(configRoot);
    std::error_code error;
    if (fs::is_regular_file(fs::symlink_status(emergency, error)) && !error) {
        return emergency;
    }
    const auto autosaveDir = cacheRoot / "xournalpp" / "autosaves";
    if (!fs::is_directory(autosaveDir, error) || error) {
        return std::nullopt;
    }
    struct Candidate {
        fs::file_time_type modified;
        fs::path path;
    };
    std::vector<Candidate> candidates;
    for (const auto& entry: fs::directory_iterator(autosaveDir, error)) {
        if (error) {
            break;
        }
        std::error_code statusError;
        if (!fs::is_regular_file(entry.symlink_status(statusError)) || statusError ||
            !isAutosaveFilename(entry.path())) {
            continue;
        }
        if (const auto owner = autosaveOwnerPid(entry.path())) {
            if (*owner == currentPid || pidAlive(*owner)) {
                continue;
            }
        }
        const auto mtime = fs::last_write_time(entry.path(), statusError);
        if (!statusError) {
            candidates.push_back({mtime, entry.path()});
        }
    }
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.modified != b.modified) {
            return a.modified > b.modified;
        }
        return a.path.filename().string() > b.path.filename().string();
    });
    return candidates.front().path;
}

inline std::vector<fs::path> autosaveSidecars(const fs::path& target) {
    std::vector<fs::path> sidecars;
    const auto pdf = pdfCompanion(target);
    if (existsWithoutFollowing(pdf)) {
        sidecars.push_back(pdf);
    }
    const auto prefix = target.filename().string() + ".bg_";
    std::error_code error;
    if (fs::is_directory(target.parent_path(), error) && !error) {
        for (const auto& entry: fs::directory_iterator(target.parent_path(), error)) {
            if (error) {
                break;
            }
            const auto name = entry.path().filename().string();
            if (name.starts_with(prefix) && name.ends_with(".png")) {
                sidecars.push_back(entry.path());
            }
        }
    }
    std::sort(sidecars.begin(), sidecars.end());
    return sidecars;
}

inline void removeAutosaveFiles(const fs::path& autosavePath) {
    if (autosavePath.empty() || !isRecoveryArtifact(autosavePath)) {
        return;
    }
    auto removeRegular = [](const fs::path& file) {
        std::error_code error;
        if (fs::is_regular_file(fs::symlink_status(file, error)) && !error) {
            fs::remove(file, error);
        }
    };
    for (const auto& sidecar: autosaveSidecars(autosavePath)) {
        removeRegular(sidecar);
    }
    removeRegular(fs::path(autosavePath.string() + "~"));
    removeRegular(fs::path(autosavePath.string() + ".swap"));
    removeRegular(autosavePath);
}

// Publishes an autosave or emergency save using AutosaveJob's .swap replacement
// without touching the primary saved document or its ~ backup.
inline void publishAutosave(SaveStaging& staging, const fs::path& target,
                            const std::function<void()>& beforeCommit = {}) {
    if (!isRecoveryArtifact(target)) {
        throw std::runtime_error("Autosave target must be an .autosave.xopp or emergencysave.xopp file");
    }
    const auto staged = staging.path / target.filename();
    if (!fs::is_regular_file(fs::symlink_status(staged))) {
        throw std::runtime_error("Staged autosave is missing");
    }
    const bool replacing = existsWithoutFollowing(target);
    if (replacing) {
        fileDigest(target);  // Reject symlinks and non-regular files.
    }
    const auto swap = fs::path(target.string() + ".swap");
    if (existsWithoutFollowing(swap)) {
        fileDigest(swap);
    }
    struct Asset {
        fs::path staged, target, original;
        bool existed = false;
        bool published = false;
    };
    std::vector<Asset> assets;
    const auto rollback = staging.path / "rollback";
    for (const auto& entry: fs::directory_iterator(staging.path)) {
        if (entry.path() != staged) {
            const auto destination = target.parent_path() / entry.path().filename();
            const bool existed = existsWithoutFollowing(destination);
            if (existed) {
                fileDigest(destination);
            }
            fileDigest(entry.path());
            assets.push_back({entry.path(), destination, rollback / entry.path().filename(), existed, false});
        }
    }
    struct ObsoleteAsset {
        fs::path target, backup;
        bool removed = false;
    };
    std::vector<ObsoleteAsset> obsolete;
    for (const auto& existing: autosaveSidecars(target)) {
        const bool retained = std::any_of(assets.begin(), assets.end(),
                                          [&](const Asset& asset) { return asset.target == existing; });
        if (!retained) {
            fileDigest(existing);
            obsolete.push_back({existing, rollback / existing.filename(), false});
        }
    }
    try {
        fs::create_directory(rollback);
        for (auto& asset: assets) {
            if (asset.existed) {
                fs::copy_file(asset.target, asset.original);
            }
        }
        for (auto& old: obsolete) {
            fs::copy_file(old.target, old.backup);
        }
        for (auto& asset: assets) {
            fs::rename(asset.staged, asset.target);
            asset.published = true;
        }
        for (auto& old: obsolete) {
            fs::remove(old.target);
            old.removed = true;
        }
        if (beforeCommit) {
            beforeCommit();
        }
        if (existsWithoutFollowing(target)) {
            if (existsWithoutFollowing(swap)) {
                fs::remove(swap);
            }
            fs::rename(target, swap);
            try {
                fs::rename(staged, target);
            } catch (...) {
                std::error_code restoreError;
                fs::rename(swap, target, restoreError);
                throw;
            }
            std::error_code cleanupError;
            fs::remove(swap, cleanupError);
        } else {
            fs::rename(staged, target);
        }
    } catch (...) {
        const auto error = std::current_exception();
        std::error_code rollbackError;
        for (auto asset = assets.rbegin(); asset != assets.rend(); ++asset) {
            if (!asset->published) {
                continue;
            }
            if (asset->existed) {
                fs::rename(asset->original, asset->target, rollbackError);
            } else {
                fs::remove(asset->target, rollbackError);
            }
        }
        for (auto old = obsolete.rbegin(); old != obsolete.rend(); ++old) {
            if (old->removed) {
                fs::rename(old->backup, old->target, rollbackError);
            }
        }
        std::rethrow_exception(error);
    }
}

}  // namespace prototype::bridge
