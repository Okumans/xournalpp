#include "AudioTarget.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(double first, double second) { return std::abs(first - second) < 1e-8; }

}  // namespace

int main() {
    try {
        prototype::AudioTarget stroke;
        stroke.page = 2;
        stroke.originLayer = 1;
        stroke.layerOrder = 5;
        stroke.timestampMs = 9123;
        stroke.shape = prototype::AudioTargetShape::Stroke;
        stroke.filename = "attachments/line\nwith-break.ogg";
        stroke.playbackFilename = "/tmp/session/audio.ogg";
        stroke.boundsX = 10;
        stroke.boundsY = 18;
        stroke.boundsWidth = 100;
        stroke.boundsHeight = 4;
        stroke.strokeWidth = 4;
        stroke.capStyle = 0;
        stroke.points = {{10, 20, -1}, {110, 20, -1}};
        require(near(prototype::audioTargetDistance(stroke, 60, 23), 1),
                "Stroke hit distance does not match the native centerline geometry");
        require(prototype::audioTargetIntersectsArea(stroke, 108, 18, 4, 4),
                "Stroke bounding-box precheck rejected an intersecting action area");
        require(!prototype::audioTargetIntersectsArea(stroke, 200, 200, 2, 2),
                "Stroke bounding-box precheck accepted a distant action area");

        prototype::AudioTarget rectangle;
        rectangle.page = 0;
        rectangle.shape = prototype::AudioTargetShape::Rectangle;
        rectangle.filename = "external.ogg";
        rectangle.playbackFilename = "/tmp/external.ogg";
        rectangle.boundsX = 180;
        rectangle.boundsY = 100;
        rectangle.boundsWidth = 20;
        rectangle.boundsHeight = 50;
        rectangle.xx = 0;
        rectangle.yx = 1;
        rectangle.xy = -1;
        rectangle.yy = 0;
        rectangle.x = 200;
        rectangle.y = 100;
        rectangle.naturalWidth = 50;
        rectangle.naturalHeight = 20;
        require(near(prototype::audioTargetDistance(rectangle, 190, 125), 0),
                "Rotated rectangular hit distance did not retain the transformed bounds");
        require(near(prototype::audioTargetDistance(rectangle, 175, 125), 5),
                "Rotated rectangular hit distance is incorrect outside the object");

        const auto path = std::filesystem::temp_directory_path() / "xournalpp-audio-target-test.dat";
        prototype::writeAudioTargets(path, {stroke, rectangle});
        const auto loaded = prototype::readAudioTargets(path);
        std::filesystem::remove(path);
        require(loaded.size() == 2 && loaded[0].filename == stroke.filename &&
                        loaded[0].playbackFilename == stroke.playbackFilename &&
                        loaded[0].timestampMs == stroke.timestampMs && loaded[0].points == stroke.points,
                "Audio-target transfer did not round-trip stroke metadata and filenames");
        require(loaded[1].shape == prototype::AudioTargetShape::Rectangle &&
                        near(loaded[1].xx, rectangle.xx) && near(loaded[1].naturalHeight, rectangle.naturalHeight),
                "Audio-target transfer did not round-trip transformed rectangular metadata");

        const auto invalidPath = std::filesystem::temp_directory_path() / "xournalpp-audio-target-invalid.dat";
        {
            std::ofstream invalid(invalidPath, std::ios::binary | std::ios::trunc);
            invalid << "XOURNALPP_GTK4_AUDIO_TARGETS_V1\n1\nnot a valid record\n";
        }
        bool rejected = false;
        try {
            prototype::readAudioTargets(invalidPath);
        } catch (const std::exception&) {
            rejected = true;
        }
        std::filesystem::remove(invalidPath);
        require(rejected, "Malformed audio-target transfer was not rejected");
        std::cout << "PASS: audio target geometry and bounded transfer" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
