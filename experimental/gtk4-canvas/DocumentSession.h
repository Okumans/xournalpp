#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "DocumentAutosave.h"
#include "DocumentTransfer.h"
#include "DocumentVersion.h"

namespace prototype::bridge {

// The GTK 3 loader/renderer lives in a separate process. Its private snapshot
// retains native objects independently of subsequent edits to the source file.
// External linked assets still refer to their original locations.
class DocumentSession {
public:
    DocumentSession(const std::string& worker, const std::string& input) {
        GError* error = nullptr;
        auto* directory = g_dir_make_tmp("xournalpp-gtk4-document-XXXXXX", &error);
        if (!directory) {
            throwError(error);
        }
        path = directory;
        g_free(directory);
        try {
            const bool nativeFile = g_ascii_strcasecmp(fs::path(input).extension().string().c_str(), ".xopp") == 0 &&
                                    fs::is_regular_file(fs::symlink_status(input));
            const auto before = nativeFile ? documentVersion(input).token : std::string{};
            const auto kind = runWorker(worker, {"open", input, snapshotPath().string(), previewPath().string()});
            if (nativeFile) {
                requireUnchangedInput(input, before);
                if (kind == "NATIVE_SAVE\n") {
                    originalSavePath = input;
                    originalVersion = before;
                }
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~DocumentSession() { cleanup(); }
    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    void saveCopy(const std::string& worker, const std::string& output,
                  const std::vector<StrokePacket>& packets) const {
        const auto transfer = path / "session.ink";
        writeTransfer(transfer, packets);
        runWorker(worker, {"save-copy", snapshotPath().string(), output, transfer.string()});
    }

    std::string save(const std::string& worker, const std::string& output, const std::vector<StrokePacket>& packets,
                     const std::string& expected) const {
        const auto transfer = path / "session.ink";
        writeTransfer(transfer, packets);
        auto version = runWorker(worker, {"save", snapshotPath().string(), output, transfer.string(), expected});
        if (!version.empty() && version.back() == '\n') {
            version.pop_back();
        }
        return version;
    }

    void autosave(const std::string& worker, const std::string& output,
                  const std::vector<StrokePacket>& packets) const {
        const auto transfer = path / "autosave.ink";
        writeTransfer(transfer, packets);
        runWorker(worker, {"autosave", snapshotPath().string(), output, transfer.string()});
    }

    std::string originalSavePath;
    std::string originalVersion;

    std::filesystem::path snapshotPath() const { return path / "snapshot.xopp"; }
    std::filesystem::path previewPath() const { return path / "preview.pdf"; }

private:
    static void requireUnchangedInput(const std::string& input, const std::string& before) {
        if (documentVersion(input).token != before) {
            throw std::runtime_error("The document changed while opening. Open it again to load the current file.");
        }
    }

    static std::string runWorker(const std::string& worker, const std::vector<std::string>& arguments) {
        std::vector<const gchar*> command{worker.c_str()};
        for (const auto& argument: arguments) {
            command.push_back(argument.c_str());
        }
        command.push_back(nullptr);
        GError* error = nullptr;
        auto* process = g_subprocess_newv(command.data(),
                                          GSubprocessFlags(G_SUBPROCESS_FLAGS_STDERR_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE),
                                          &error);
        if (!process) {
            throwError(error);
        }
        gchar* diagnostics = nullptr;
        gchar* output = nullptr;
        const bool communicated = g_subprocess_communicate_utf8(process, nullptr, nullptr, &output,
                                                                &diagnostics, &error);
        const bool succeeded = communicated && g_subprocess_get_successful(process);
        const std::string message = diagnostics ? diagnostics : "";
        const std::string result = output ? output : "";
        g_free(diagnostics);
        g_free(output);
        if (!communicated) {
            g_subprocess_force_exit(process);
            g_subprocess_wait(process, nullptr, nullptr);
        }
        g_object_unref(process);
        if (!communicated) {
            throwError(error);
        }
        if (!succeeded) {
            throw std::runtime_error(message.empty() ? "Document worker failed" : message);
        }
        return result;
    }

    [[noreturn]] static void throwError(GError* error) {
        const std::string message = error ? error->message : "Document worker failed";
        g_clear_error(&error);
        throw std::runtime_error(message);
    }

    void cleanup() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

}  // namespace prototype::bridge
