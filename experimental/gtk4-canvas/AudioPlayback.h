#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include <gst/gst.h>

namespace prototype {

class AudioPlayback {
public:
    using ErrorHandler = std::function<void(const std::string&)>;
    using ChangeHandler = std::function<void()>;

    AudioPlayback(bool disabled, ErrorHandler onError, ChangeHandler onChanged):
            disabled(disabled), onError(std::move(onError)), onChanged(std::move(onChanged)) {
        if (disabled) {
            return;
        }
        gst_init(nullptr, nullptr);
        pipeline = gst_element_factory_make("playbin", "xournalpp-audio-playback");
        if (!pipeline) {
            initializationError = "GStreamer could not create the audio playback pipeline";
            return;
        }

        if (const char* sinkDescription = g_getenv("XOURNALPP_AUDIO_SINK"); sinkDescription && *sinkDescription) {
            GError* error = nullptr;
            auto* sink = gst_parse_bin_from_description(sinkDescription, true, &error);
            if (!sink) {
                initializationError = error ? error->message : "Cannot create the configured audio sink";
                g_clear_error(&error);
                gst_object_unref(pipeline);
                pipeline = nullptr;
                return;
            }
            gst_object_ref_sink(sink);
            g_object_set(pipeline, "audio-sink", sink, nullptr);
            gst_object_unref(sink);
        }

        auto* bus = gst_element_get_bus(pipeline);
        busWatch = gst_bus_add_watch(bus, onBusMessage, this);
        gst_object_unref(bus);
        if (!busWatch) {
            initializationError = "Cannot monitor audio playback state";
            gst_object_unref(pipeline);
            pipeline = nullptr;
        }
    }

    ~AudioPlayback() {
        onError = {};
        onChanged = {};
        stop();
        if (busWatch) {
            g_source_remove(busWatch);
            busWatch = 0;
        }
        if (pipeline) {
            gst_object_unref(pipeline);
        }
    }

    AudioPlayback(const AudioPlayback&) = delete;
    AudioPlayback& operator=(const AudioPlayback&) = delete;

    bool start(const std::filesystem::path& filename, uint64_t timestampMs, std::string& error) {
        if (disabled) {
            error = "Audio is disabled for this session";
            return false;
        }
        if (!pipeline) {
            error = initializationError.empty() ? "Audio playback is unavailable" : initializationError;
            return false;
        }
        stop();
        std::error_code statusError;
        if (!std::filesystem::is_regular_file(std::filesystem::status(filename, statusError)) || statusError) {
            error = "Audio recording is missing: " + filename.string();
            return false;
        }

        GError* uriError = nullptr;
        gchar* uri = g_filename_to_uri(filename.c_str(), nullptr, &uriError);
        if (!uri) {
            error = uriError ? uriError->message : "Cannot access audio recording: " + filename.string();
            g_clear_error(&uriError);
            return false;
        }
        currentFilename = filename.string();
        startTimestampNs = static_cast<GstClockTime>(timestampMs) * GST_MSECOND;
        pendingStart = true;
        active = true;
        paused = false;
        g_object_set(pipeline, "uri", uri, nullptr);
        g_free(uri);
        const auto result = gst_element_set_state(pipeline, GST_STATE_PAUSED);
        if (result == GST_STATE_CHANGE_FAILURE) {
            pendingStart = false;
            active = false;
            error = "Unable to decode audio recording " + currentFilename;
            currentFilename.clear();
            gst_element_set_state(pipeline, GST_STATE_NULL);
            changed();
            return false;
        }
        changed();
        return true;
    }

    bool isActive() const { return active; }
    bool isPaused() const { return active && paused; }

    void pause() {
        if (!pipeline || !active || paused) {
            return;
        }
        if (gst_element_set_state(pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE) {
            paused = true;
            changed();
        }
    }

    void resume() {
        if (!pipeline || !active || !paused) {
            return;
        }
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
            paused = false;
            changed();
        }
    }

    void stop() {
        pendingStart = false;
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
        const bool hadActivePlayback = active;
        active = false;
        paused = false;
        currentFilename.clear();
        if (hadActivePlayback) {
            changed();
        }
    }

    void seekBySeconds(int seconds) {
        if (!pipeline || !active) {
            return;
        }
        gint64 position = 0;
        if (!gst_element_query_position(pipeline, GST_FORMAT_TIME, &position)) {
            position = 0;
        }
        gint64 duration = GST_CLOCK_TIME_NONE;
        gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration);
        const auto delta = static_cast<gint64>(seconds) * GST_SECOND;
        gint64 target = std::max<gint64>(0, position + delta);
        if (GST_CLOCK_TIME_IS_VALID(duration)) {
            target = std::min(target, duration);
        }
        gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                                GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), target);
    }

    const std::string& filename() const { return currentFilename; }

private:
    static gboolean onBusMessage(GstBus*, GstMessage* message, gpointer data) {
        auto* self = static_cast<AudioPlayback*>(data);
        switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ASYNC_DONE:
                if (self->pendingStart) {
                    self->pendingStart = false;
                    if (self->startTimestampNs > 0) {
                        gst_element_seek_simple(self->pipeline, GST_FORMAT_TIME,
                                                GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                                                static_cast<gint64>(self->startTimestampNs));
                    }
                    if (self->paused) {
                        self->changed();
                    } else if (gst_element_set_state(self->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
                        self->finishWithError("Unable to start audio recording " + self->currentFilename);
                    } else {
                        self->paused = false;
                        self->changed();
                    }
                }
                break;
            case GST_MESSAGE_ERROR: {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                const std::string detail = error ? error->message : "Unknown audio playback error";
                g_clear_error(&error);
                g_free(debug);
                self->finishWithError("Unable to play audio recording " + self->currentFilename + ": " + detail);
                break;
            }
            case GST_MESSAGE_EOS:
                self->finish();
                break;
            default:
                break;
        }
        return G_SOURCE_CONTINUE;
    }

    void finishWithError(const std::string& message) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        finish();
        if (onError) {
            onError(message);
        }
    }

    void finish() {
        pendingStart = false;
        active = false;
        paused = false;
        currentFilename.clear();
        changed();
    }

    void changed() {
        if (onChanged) {
            onChanged();
        }
    }

    bool disabled = false;
    GstElement* pipeline = nullptr;
    guint busWatch = 0;
    bool active = false;
    bool paused = false;
    bool pendingStart = false;
    GstClockTime startTimestampNs = 0;
    std::string currentFilename;
    std::string initializationError;
    ErrorHandler onError;
    ChangeHandler onChanged;
};

}  // namespace prototype
