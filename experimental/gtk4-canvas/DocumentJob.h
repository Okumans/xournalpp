#pragma once

#include <exception>
#include <functional>
#include <string>
#include <utility>

#include <gio/gio.h>

namespace prototype::bridge {

// Finish on the caller's main context. Work functions must not access widgets.
inline void documentJob(std::function<void()> work, std::function<void(const std::string&)> complete) {
    struct Job {
        std::function<void()> work;
        std::function<void(const std::string&)> complete;
        std::string error;
    };
    auto* task = g_task_new(nullptr, nullptr, +[](GObject*, GAsyncResult* result, gpointer) {
        auto* job = static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
        job->complete(job->error);
    }, nullptr);
    g_task_set_task_data(task, new Job{std::move(work), std::move(complete), {}},
                         +[](gpointer data) { delete static_cast<Job*>(data); });
    g_task_run_in_thread(task, +[](GTask* task, gpointer, gpointer data, GCancellable*) {
        auto* job = static_cast<Job*>(data);
        try {
            job->work();
        } catch (const std::exception& error) {
            job->error = error.what();
        } catch (...) {
            job->error = "Document operation failed";
        }
        g_task_return_boolean(task, job->error.empty());
    });
    g_object_unref(task);
}

}  // namespace prototype::bridge
