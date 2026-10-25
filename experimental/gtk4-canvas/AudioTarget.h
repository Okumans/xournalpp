#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace prototype {

enum class AudioTargetShape : int { Stroke = 0, Rectangle = 1 };

struct AudioTargetPoint {
    double x = 0;
    double y = 0;
    double width = -1;

    bool operator==(const AudioTargetPoint&) const = default;
};

struct AudioTarget {
    int page = 0;
    size_t originLayer = 0;
    size_t layerOrder = 0;
    uint64_t timestampMs = 0;
    AudioTargetShape shape = AudioTargetShape::Stroke;
    std::string filename;
    std::string playbackFilename;
    double boundsX = 0;
    double boundsY = 0;
    double boundsWidth = 0;
    double boundsHeight = 0;
    double strokeWidth = 1;
    bool pressureSensitive = false;
    int capStyle = 0;
    std::vector<AudioTargetPoint> points;
    double xx = 1;
    double yx = 0;
    double xy = 0;
    double yy = 1;
    double x = 0;
    double y = 0;
    double naturalWidth = 0;
    double naturalHeight = 0;
};

inline constexpr size_t maxAudioTargets = 65536;
inline constexpr size_t maxAudioTargetPoints = 2'000'000;
inline constexpr size_t maxAudioTargetTransferBytes = 128 * 1024 * 1024;

namespace audio_target_detail {

inline std::string readLine(std::istream& stream) {
    std::string line;
    char character = 0;
    while (stream.get(character)) {
        if (character == '\n') {
            return line;
        }
        if (line.size() >= 65536) {
            throw std::runtime_error("Audio-target metadata record is too long");
        }
        line.push_back(character);
    }
    throw std::runtime_error("Incomplete audio-target metadata");
}

template <typename... Values>
inline void parseLine(const std::string& line, Values&... values) {
    std::istringstream record(line);
    record.imbue(std::locale::classic());
    if (!(record >> ... >> values)) {
        throw std::runtime_error("Invalid audio-target metadata");
    }
    record >> std::ws;
    if (!record.eof()) {
        throw std::runtime_error("Unexpected audio-target metadata");
    }
}

inline void writeString(std::ostream& stream, const std::string& value) {
    if (value.size() > 65536) {
        throw std::runtime_error("Audio filename exceeds its transfer limit");
    }
    stream << value.size() << '\n';
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.put('\n');
}

inline std::string readString(std::istream& stream, size_t length) {
    if (length > 65536) {
        throw std::runtime_error("Audio filename exceeds its transfer limit");
    }
    std::string value(length, '\0');
    if (length && !stream.read(value.data(), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Incomplete audio filename in transfer");
    }
    if (stream.get() != '\n') {
        throw std::runtime_error("Invalid audio filename delimiter");
    }
    return value;
}

inline void validate(const AudioTarget& target) {
    const auto finite = [](double value) { return std::isfinite(value); };
    if (target.page < 0 || target.shape < AudioTargetShape::Stroke || target.shape > AudioTargetShape::Rectangle ||
        !finite(target.boundsX) || !finite(target.boundsY) || !finite(target.boundsWidth) ||
        !finite(target.boundsHeight) || target.boundsWidth < 0 || target.boundsHeight < 0 ||
        target.filename.size() > 65536 || target.playbackFilename.size() > 65536) {
        throw std::runtime_error("Invalid audio target");
    }
    if (target.shape == AudioTargetShape::Stroke) {
        if (target.points.size() < 2 || !finite(target.strokeWidth) || target.strokeWidth <= 0 ||
            target.capStyle < 0 || target.capStyle > 2) {
            throw std::runtime_error("Invalid audio-linked stroke geometry");
        }
        for (const auto& point: target.points) {
            if (!finite(point.x) || !finite(point.y) || !finite(point.width) ||
                (point.width != -1 && point.width <= 0)) {
                throw std::runtime_error("Invalid audio-linked stroke point");
            }
        }
    } else {
        const double determinant = target.xx * target.yy - target.xy * target.yx;
        if (!finite(target.xx) || !finite(target.yx) || !finite(target.xy) || !finite(target.yy) ||
            !finite(target.x) || !finite(target.y) || !finite(target.naturalWidth) ||
            !finite(target.naturalHeight) || target.naturalWidth < 0 || target.naturalHeight < 0 ||
            !finite(determinant) || std::abs(determinant) < 1e-12) {
            throw std::runtime_error("Invalid audio-linked rectangular geometry");
        }
    }
}

}  // namespace audio_target_detail

inline void writeAudioTargets(const std::filesystem::path& path, const std::vector<AudioTarget>& targets) {
    if (targets.size() > maxAudioTargets) {
        throw std::runtime_error("Document contains too many audio-linked objects");
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.imbue(std::locale::classic());
    if (!stream) {
        throw std::runtime_error("Cannot create audio-target transfer");
    }
    stream << "XOURNALPP_GTK4_AUDIO_TARGETS_V1 " << targets.size() << '\n';
    size_t totalPoints = 0;
    for (const auto& target: targets) {
        audio_target_detail::validate(target);
        if (target.points.size() > maxAudioTargetPoints - totalPoints) {
            throw std::runtime_error("Audio-target transfer contains too many stroke points");
        }
        totalPoints += target.points.size();
        stream << target.page << ' ' << target.originLayer << ' ' << target.layerOrder << ' '
               << target.timestampMs << ' ' << static_cast<int>(target.shape) << ' '
               << target.boundsX << ' ' << target.boundsY << ' ' << target.boundsWidth << ' '
               << target.boundsHeight << ' ' << target.strokeWidth << ' '
               << static_cast<int>(target.pressureSensitive) << ' ' << target.capStyle << ' '
               << target.xx << ' ' << target.yx << ' ' << target.xy << ' ' << target.yy << ' '
               << target.x << ' ' << target.y << ' ' << target.naturalWidth << ' '
               << target.naturalHeight << ' ' << target.points.size() << '\n';
        audio_target_detail::writeString(stream, target.filename);
        audio_target_detail::writeString(stream, target.playbackFilename);
        for (const auto& point: target.points) {
            stream << point.x << ' ' << point.y << ' ' << point.width << '\n';
        }
    }
    stream.flush();
    if (!stream) {
        throw std::runtime_error("Cannot write audio-target transfer");
    }
}

inline std::vector<AudioTarget> readAudioTargets(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > maxAudioTargetTransferBytes) {
        throw std::runtime_error("Audio-target transfer exceeds its size limit");
    }
    std::ifstream stream(path, std::ios::binary);
    stream.imbue(std::locale::classic());
    if (!stream) {
        throw std::runtime_error("Cannot open audio-target transfer");
    }
    std::string magic;
    size_t count = 0;
    audio_target_detail::parseLine(audio_target_detail::readLine(stream), magic, count);
    if (magic != "XOURNALPP_GTK4_AUDIO_TARGETS_V1" || count > maxAudioTargets) {
        throw std::runtime_error("Invalid audio-target transfer header");
    }

    std::vector<AudioTarget> targets;
    targets.reserve(count);
    size_t totalPoints = 0;
    size_t totalStrings = 0;
    for (size_t index = 0; index < count; ++index) {
        AudioTarget target;
        int shape = 0;
        int pressureSensitive = 0;
        size_t pointCount = 0;
        audio_target_detail::parseLine(audio_target_detail::readLine(stream), target.page, target.originLayer,
                                      target.layerOrder, target.timestampMs, shape, target.boundsX,
                                      target.boundsY, target.boundsWidth, target.boundsHeight,
                                      target.strokeWidth, pressureSensitive, target.capStyle,
                                      target.xx, target.yx, target.xy, target.yy, target.x, target.y,
                                      target.naturalWidth, target.naturalHeight, pointCount);
        if ((pressureSensitive != 0 && pressureSensitive != 1) || pointCount > maxAudioTargetPoints - totalPoints) {
            throw std::runtime_error("Invalid audio-target transfer record");
        }
        target.shape = static_cast<AudioTargetShape>(shape);
        target.pressureSensitive = pressureSensitive != 0;
        target.filename = audio_target_detail::readString(stream, [&] {
            size_t length = 0;
            audio_target_detail::parseLine(audio_target_detail::readLine(stream), length);
            return length;
        }());
        target.playbackFilename = audio_target_detail::readString(stream, [&] {
            size_t length = 0;
            audio_target_detail::parseLine(audio_target_detail::readLine(stream), length);
            return length;
        }());
        totalStrings += target.filename.size() + target.playbackFilename.size();
        if (totalStrings > maxAudioTargetTransferBytes) {
            throw std::runtime_error("Audio-target transfer exceeds its size limit");
        }
        totalPoints += pointCount;
        target.points.reserve(pointCount);
        for (size_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
            AudioTargetPoint point;
            audio_target_detail::parseLine(audio_target_detail::readLine(stream), point.x, point.y, point.width);
            target.points.push_back(point);
        }
        audio_target_detail::validate(target);
        targets.push_back(std::move(target));
    }
    stream >> std::ws;
    if (!stream.eof()) {
        throw std::runtime_error("Unexpected data in audio-target transfer");
    }
    return targets;
}

inline bool audioTargetIntersectsArea(const AudioTarget& target, double x, double y, double width, double height) {
    return target.boundsX <= x + width && x <= target.boundsX + target.boundsWidth &&
           target.boundsY <= y + height && y <= target.boundsY + target.boundsHeight;
}

inline double audioTargetDistance(const AudioTarget& target, double x, double y) {
    if (target.shape == AudioTargetShape::Rectangle) {
        const double determinant = target.xx * target.yy - target.xy * target.yx;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
            return std::numeric_limits<double>::max();
        }
        const double dx = x - target.x;
        const double dy = y - target.y;
        const double localX = (target.yy * dx - target.xy * dy) / determinant;
        const double localY = (-target.yx * dx + target.xx * dy) / determinant;
        const double projectedX = std::clamp(localX, 0.0, target.naturalWidth);
        const double projectedY = std::clamp(localY, 0.0, target.naturalHeight);
        const double pageX = target.xx * projectedX + target.xy * projectedY + target.x;
        const double pageY = target.yx * projectedX + target.yy * projectedY + target.y;
        return std::hypot(pageX - x, pageY - y);
    }

    if (target.points.size() < 2) {
        return std::numeric_limits<double>::max();
    }
    double distance = std::numeric_limits<double>::max();
    size_t first = 0;
    size_t last = target.points.size() - 1;
    while (first < last && target.points[first].x == target.points[first + 1].x &&
           target.points[first].y == target.points[first + 1].y) {
        ++first;
    }
    while (last > first && target.points[last].x == target.points[last - 1].x &&
           target.points[last].y == target.points[last - 1].y) {
        --last;
    }

    for (size_t index = 0; index + 1 < target.points.size(); ++index) {
        const auto& p1 = target.points[index];
        const auto& p2 = target.points[index + 1];
        const double vx = p2.x - p1.x;
        const double vy = p2.y - p1.y;
        const double length = std::hypot(vx, vy);
        if (length == 0) {
            if (target.pressureSensitive || target.capStyle == 0) {
                const double width = p1.width < 0 ? target.strokeWidth : p1.width;
                distance = std::min(distance, std::max(std::hypot(x - p1.x, y - p1.y) - width / 2.0, 0.0));
            }
            continue;
        }

        const double along = ((x - p1.x) * vx + (y - p1.y) * vy) / length;
        if (!target.pressureSensitive && target.capStyle != 0) {
            const double halfWidth = target.strokeWidth / 2.0;
            const bool atStart = index == first;
            const bool atEnd = index + 1 == last;
            const bool square = target.capStyle == 2;
            const double start = atStart && square ? -halfWidth : 0.0;
            const double end = length + (atEnd && square ? halfWidth : 0.0);
            const double across = std::abs((x - p1.x) * vy - (y - p1.y) * vx) / length;
            double current = std::hypot(std::max({start - along, along - end, 0.0}),
                                        std::max(across - halfWidth, 0.0));
            if (!atStart) {
                current = std::min(current, std::max(std::hypot(x - p1.x, y - p1.y) - halfWidth, 0.0));
            }
            if (!atEnd) {
                current = std::min(current, std::max(std::hypot(x - p2.x, y - p2.y) - halfWidth, 0.0));
            }
            distance = std::min(distance, current);
            continue;
        }

        const double ratio = std::clamp(along / length, 0.0, 1.0);
        const double projectedX = p1.x + ratio * vx;
        const double projectedY = p1.y + ratio * vy;
        const double width = p1.width < 0 ? target.strokeWidth : p1.width;
        distance = std::clamp(std::hypot(x - projectedX, y - projectedY) - width / 2.0, 0.0, distance);
    }
    return distance;
}

}  // namespace prototype
