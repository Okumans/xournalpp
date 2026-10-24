#include <chrono>
#include <iostream>
#include <iterator>
#include <thread>

#include "DocumentAutosave.h"

using namespace prototype::bridge;

static void write(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << content;
    file.close();
    if (!file) {
        throw std::runtime_error("Cannot prepare autosave filesystem test");
    }
}

static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    try {
        SaveStaging root(fs::temp_directory_path());
        const auto cache = root.path / "cache";
        const auto config = root.path / "config";
        const auto docs = root.path / "docs";
        fs::create_directories(docs);

        // Source-grounded path derivation.
        check(autosavePathForTarget(docs / "notes.xopp", cache, 4242) == docs / ".notes.autosave.xopp",
              "Associated .xopp autosave path mismatch");
        check(autosavePathForTarget(docs / "Lecture.1.XOPP", cache, 4242) == docs / ".Lecture.1.autosave.xopp",
              "Case-insensitive .XOPP autosave path mismatch");
        check(autosavePathForTarget(docs / "legacy.xoj", cache, 4242) == docs / ".legacy.autosave.xopp",
              "Legacy .xoj autosave path mismatch");
        check(autosavePathForTarget({}, cache, 4242) == cache / "xournalpp" / "autosaves" / "4242.autosave.xopp",
              "Unsaved cache autosave path mismatch");
        check(emergencySavePath(config) == config / "xournalpp" / "emergencysave.xopp",
              "Emergency save path mismatch");

        // Cache-folder direct-open protection (Control.cpp shouldFileOpen).
        const auto cachedAutosave = autosavePathForTarget({}, cache, 9999);
        write(cachedAutosave, "cached autosave");
        check(isCacheProtectedPath(cachedAutosave, cache), "Cache autosave must be protected from direct open");
        const auto symlinkIntoCache = docs / "sneaky.xopp";
        fs::create_symlink(cachedAutosave, symlinkIntoCache);
        check(isCacheProtectedPath(symlinkIntoCache, cache), "Symlink into cache autosaves must be protected");
        fs::remove(symlinkIntoCache);
        check(!isCacheProtectedPath(docs / "notes.xopp", cache), "Normal document path falsely blocked");
        check(!isCacheProtectedPath(docs / ".notes.autosave.xopp", cache),
              "Sidecar autosave outside cache should use recovery rather than cache block");

        // Primary document and its backup must remain untouched across autosave writes, failures, and cleanup.
        const auto primary = docs / "notes.xopp";
        const auto primaryPdf = pdfCompanion(primary);
        const auto primaryBackup = fs::path(primary.string() + "~");
        write(primary, "saved primary");
        write(primaryPdf, "saved primary PDF");
        write(primaryBackup, "existing primary backup");

        const auto sidecarAutosave = autosavePathForTarget(primary, cache, 4242);
        const auto autosavePdf = pdfCompanion(sidecarAutosave);
        const auto autosavePng = fs::path(sidecarAutosave.string() + ".bg_1.png");
        {
            SaveStaging staging(docs);
            write(staging.path / sidecarAutosave.filename(), "autosave v1");
            write(staging.path / autosavePdf.filename(), "autosave PDF v1");
            write(staging.path / autosavePng.filename(), "autosave PNG v1");
            publishAutosave(staging, sidecarAutosave);
        }
        check(read(sidecarAutosave) == "autosave v1" && read(autosavePdf) == "autosave PDF v1" &&
                      read(autosavePng) == "autosave PNG v1",
              "Initial autosave did not publish document and sidecars");
        check(read(primary) == "saved primary" && read(primaryPdf) == "saved primary PDF" &&
                      read(primaryBackup) == "existing primary backup",
              "Autosave altered the primary document, its PDF, or its ~ backup");

        // Pre-commit failure must roll back sidecars and preserve the previous autosave.
        {
            SaveStaging staging(docs);
            write(staging.path / sidecarAutosave.filename(), "broken autosave");
            write(staging.path / autosavePdf.filename(), "broken PDF");
            bool failed = false;
            try {
                publishAutosave(staging, sidecarAutosave, [] { throw std::runtime_error("injected failure"); });
            } catch (const std::runtime_error&) {
                failed = true;
            }
            check(failed && read(sidecarAutosave) == "autosave v1" && read(autosavePdf) == "autosave PDF v1" &&
                          read(autosavePng) == "autosave PNG v1" &&
                          !existsWithoutFollowing(fs::path(sidecarAutosave.string() + ".swap")),
                  "Failed autosave corrupted prior autosave, lost sidecars, or leaked .swap");
        }

        // Second autosave swaps cleanly and removes obsolete autosave-only pixmap sidecar.
        {
            SaveStaging staging(docs);
            write(staging.path / sidecarAutosave.filename(), "autosave v2");
            write(staging.path / autosavePdf.filename(), "autosave PDF v2");
            publishAutosave(staging, sidecarAutosave);
        }
        check(read(sidecarAutosave) == "autosave v2" && read(autosavePdf) == "autosave PDF v2" &&
                      !existsWithoutFollowing(autosavePng) &&
                      !existsWithoutFollowing(fs::path(sidecarAutosave.string() + ".swap")),
              "Updated autosave did not replace via .swap or clean obsolete autosave sidecars");

        // Symlink targets must be rejected.
        const auto symlinkAutosave = docs / ".linked.autosave.xopp";
        fs::create_symlink(primary, symlinkAutosave);
        {
            SaveStaging staging(docs);
            write(staging.path / symlinkAutosave.filename(), "must not overwrite symlink target");
            bool rejected = false;
            try {
                publishAutosave(staging, symlinkAutosave);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected && read(primary) == "saved primary", "Autosave followed a symlink target");
        }
        fs::remove(symlinkAutosave);

        // removeAutosaveFiles must remove autosave + sidecars while refusing non-recovery paths.
        removeAutosaveFiles(primary);
        check(read(primary) == "saved primary" && read(primaryPdf) == "saved primary PDF" &&
                      read(primaryBackup) == "existing primary backup",
              "removeAutosaveFiles touched a normal document");
        removeAutosaveFiles(sidecarAutosave);
        check(!existsWithoutFollowing(sidecarAutosave) && !existsWithoutFollowing(autosavePdf),
              "removeAutosaveFiles left autosave or sidecar behind");

        // Startup recovery discovery: live PIDs and current PID are ignored; emergency save takes priority.
        const auto liveAutosave = autosavePathForTarget({}, cache, 1001);
        const auto oldDeadAutosave = autosavePathForTarget({}, cache, 2002);
        const auto newDeadAutosave = autosavePathForTarget({}, cache, 3003);
        write(liveAutosave, "live session");
        write(oldDeadAutosave, "older crashed session");
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        write(newDeadAutosave, "newer crashed session");
        auto alive = [](long long pid) { return pid == 1001; };
        const auto picked = findStartupRecoveryFile(config, cache, 9999, alive);
        check(picked && *picked == newDeadAutosave, "Startup recovery did not pick newest dead-PID autosave");
        check(!findStartupRecoveryFile(config, cache, 3003, [](long long) { return true; }).has_value(),
              "Startup recovery stole an active process's autosave");

        const auto emergency = emergencySavePath(config);
        write(emergency, "emergency crash save");
        check(findStartupRecoveryFile(config, cache, 9999, alive) == emergency,
              "Emergency save did not take precedence at startup");
        removeAutosaveFiles(emergency);
        check(!existsWithoutFollowing(emergency), "Emergency save cleanup failed");

        std::cout << "PASS: autosave paths, cache protection, .swap publication, rollback, symlink rejection, "
                     "sidecar cleanup, orphan/emergency discovery\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
