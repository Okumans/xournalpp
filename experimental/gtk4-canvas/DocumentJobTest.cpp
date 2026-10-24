#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "DocumentJob.h"

int main() {
    auto* loop = g_main_loop_new(nullptr, false);
    const auto mainThread = std::this_thread::get_id();
    std::atomic<bool> mainLoopRan = false;
    bool finished = false;
    bool passed = true;
    g_idle_add(+[](gpointer data) -> gboolean {
        *static_cast<std::atomic<bool>*>(data) = true;
        return G_SOURCE_REMOVE;
    }, &mainLoopRan);
    const auto timeout = g_timeout_add_seconds(5, +[](gpointer data) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(data));
        return G_SOURCE_REMOVE;
    }, loop);
    prototype::bridge::documentJob([&] {
        if (std::this_thread::get_id() == mainThread) {
            throw std::runtime_error("Document work ran on the UI thread");
        }
        // Require the caller's main loop to stay responsive while work is active.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!mainLoopRan && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!mainLoopRan) {
            throw std::runtime_error("Document work blocked the main loop");
        }
    }, [&](const std::string& error) {
        passed = error.empty() && std::this_thread::get_id() == mainThread;
        prototype::bridge::documentJob([] { throw std::runtime_error("expected failure"); },
                                      [&](const std::string& error) {
            passed = passed && error == "expected failure" && std::this_thread::get_id() == mainThread;
            finished = true;
            g_main_loop_quit(loop);
        });
    });
    g_main_loop_run(loop);
    if (finished) {
        g_source_remove(timeout);
    }
    g_main_loop_unref(loop);
    if (!finished || !passed) {
        std::cerr << "Document job thread, main-context completion or error propagation failed\n";
        return 1;
    }
    std::cout << "PASS: background work, responsive main loop, main-context completion and error propagation\n";
}
