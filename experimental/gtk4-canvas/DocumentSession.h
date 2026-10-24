#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <gio/gio.h>

#include "DocumentTransfer.h"

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
            runWorker(worker, {"open", input, snapshotPath().string(), previewPath().string()});
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

    std::filesystem::path snapshotPath() const { return path / "snapshot.xopp"; }
    std::filesystem::path previewPath() const { return path / "preview.pdf"; }

private:
    static void runWorker(const std::string& worker, const std::vector<std::string>& arguments) {
        std::vector<const gchar*> command{worker.c_str()};
        for (const auto& argument: arguments) {
            command.push_back(argument.c_str());
        }
        command.push_back(nullptr);
        GError* error = nullptr;
        auto* process = g_subprocess_newv(command.data(), G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
        if (!process) {
            throwError(error);
        }
        gchar* diagnostics = nullptr;
        const bool communicated = g_subprocess_communicate_utf8(process, nullptr, nullptr, nullptr,
                                                                &diagnostics, &error);
        const bool succeeded = communicated && g_subprocess_get_successful(process);
        const std::string message = diagnostics ? diagnostics : "";
        g_free(diagnostics);
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
