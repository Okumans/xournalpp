#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include <gio/gio.h>

namespace prototype::bridge {

// The GTK 3 loader/renderer lives in a separate process. Its private snapshot
// gives this session a stable document even when another app edits the source.
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
            const auto snapshot = (path / "snapshot.xopp").string();
            const auto preview = previewPath().string();
            auto* process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_PIPE, &error, worker.c_str(), "open",
                                              input.c_str(), snapshot.c_str(), preview.c_str(), nullptr);
            if (!process) {
                throwError(error);
            }
            gchar* diagnostics = nullptr;
            const bool communicated = g_subprocess_communicate_utf8(process, nullptr, nullptr, nullptr,
                                                                    &diagnostics, &error);
            const bool succeeded = communicated && g_subprocess_get_successful(process);
            const std::string message = diagnostics ? diagnostics : "";
            g_free(diagnostics);
            g_object_unref(process);
            if (!communicated) {
                throwError(error);
            }
            if (!succeeded) {
                throw std::runtime_error("Cannot open document: " + message);
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~DocumentSession() { cleanup(); }
    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    std::filesystem::path previewPath() const { return path / "preview.pdf"; }

private:
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
