#include <iostream>
#include <iterator>

#include "DocumentPublish.h"

using namespace prototype::bridge;

static void write(const fs::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary);
    file << content;
    file.close();
    if (!file) {
        throw std::runtime_error("Cannot prepare filesystem test");
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
        SaveStaging directory(fs::temp_directory_path());
        const auto target = directory.path / "notes.xopp";
        check(pdfCompanion(directory.path / "notes.XOPP") == directory.path / "notes.xopp.bg.pdf",
              "Case-insensitive document extensions changed the real saver's PDF sidecar name");
        const auto pdf = pdfCompanion(target);
        const auto png = fs::path(target.string() + ".bg_1.png");
        const auto backup = fs::path(target.string() + "~");
        write(target, "original document");
        write(pdf, "original PDF");
        fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write);
        const auto initial = documentVersion(target).token;
        {
            SaveStaging staging(directory.path);
            write(staging.path / target.filename(), "replacement");
            write(staging.path / pdf.filename(), "replacement PDF");
            write(staging.path / png.filename(), "new pixmap");
            bool failed = false;
            try {
                publishDocument(staging, target, initial, [&] {
                    check(read(backup) == "original document", "Backup missing before document commit");
                    throw std::runtime_error("simulated pre-commit I/O failure");
                });
            } catch (const std::runtime_error&) {
                failed = true;
            }
            check(failed && read(target) == "original document" && read(pdf) == "original PDF" && !fs::exists(png),
                  "Failed publication did not roll back existing and newly created assets");
            check(read(backup) == "original document", "Failed save must retain its recoverable backup");
            check(documentVersion(target).token == initial, "Rollback changed the original version");
        }
        fs::remove(backup);
        {
            SaveStaging staging(directory.path);
            write(staging.path / target.filename(), "replacement");
            write(staging.path / pdf.filename(), "replacement PDF");
            publishDocument(staging, target, initial);
            check(read(target) == "replacement" && read(pdf) == "replacement PDF" && !fs::exists(backup),
                  "Successful save did not commit both files and clean its temporary backup");
            check(fs::status(target).permissions() == (fs::perms::owner_read | fs::perms::owner_write),
                  "Save changed existing document permissions");
        }
        {
            SaveStaging staging(directory.path);
            write(staging.path / target.filename(), "must not be written");
            bool rejected = false;
            try {
                publishDocument(staging, target, initial);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected && read(target) == "replacement", "A stale version overwrote newer content");
        }
        const auto current = documentVersion(target).token;
        write(pdf, "external PDF update");
        check(documentVersion(target).token != current, "Version token missed an attachment-only edit");
        const auto external = documentVersion(target).token;
        {
            SaveStaging staging(directory.path);
            write(staging.path / target.filename(), "must not replace concurrent edit");
            write(staging.path / pdf.filename(), "temporary replacement PDF");
            bool rejected = false;
            try {
                publishDocument(staging, target, external, [&] { write(target, "concurrent document edit"); });
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected && read(target) == "concurrent document edit" && read(pdf) == "external PDF update",
                  "A document edit before commit was overwritten or its assets were not restored");
            check(read(backup) == "replacement", "Backup changed with a concurrent in-place write");
        }
        // A previously retained backup is never overwritten by another save attempt.
        {
            SaveStaging staging(directory.path);
            write(staging.path / target.filename(), "must not replace existing backup");
            bool rejected = false;
            try {
                publishDocument(staging, target, documentVersion(target).token);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            check(rejected && read(backup) == "replacement" && read(target) == "concurrent document edit",
                  "A retained backup or document was overwritten");
        }
        fs::remove(backup);
        // If an attachment itself changes during rollback, retain original recovery data.
        fs::path retained;
        {
            SaveStaging staging(directory.path);
            retained = staging.path;
            write(staging.path / target.filename(), "uncommitted document");
            write(staging.path / pdf.filename(), "temporary PDF");
            bool failed = false;
            try {
                publishDocument(staging, target, documentVersion(target).token, [&] {
                    write(pdf, "concurrent attachment edit");
                    throw std::runtime_error("simulated failure");
                });
            } catch (const std::runtime_error&) {
                failed = true;
            }
            check(failed && staging.retain, "Failed rollback discarded original recovery data");
        }
        check(fs::is_directory(retained) && read(retained / "rollback" / pdf.filename()) == "external PDF update" &&
                      read(pdf) == "concurrent attachment edit",
              "Recovery copies were removed or concurrent asset edits were overwritten");
        fs::remove_all(retained);
        std::cout << "PASS: staged save, backup lifetime, rollback/recovery, concurrent edits, sidecar versions, permissions\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
