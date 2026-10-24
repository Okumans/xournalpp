#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace prototype::bridge {

struct Sample {
    double x;
    double y;
    double pressure;
};

struct StrokePacket {
    int page = 0;
    bool highlighter = false;
    bool pressureSensitive = false;
    double width = 1;
    std::array<double, 3> color{};
    std::vector<Sample> points;
};

inline constexpr size_t maxTransferPoints = 1000000;

inline void validate(const StrokePacket& stroke) {
    if (stroke.page < 0 || !std::isfinite(stroke.width) || stroke.width <= 0 || stroke.points.empty()) {
        throw std::runtime_error("Invalid transferred stroke");
    }
    for (double channel: stroke.color) {
        if (!std::isfinite(channel) || channel < 0 || channel > 1) {
            throw std::runtime_error("Invalid transferred ink color");
        }
    }
    for (const auto& point: stroke.points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.pressure) ||
            point.pressure <= 0) {
            throw std::runtime_error("Invalid transferred stroke sample");
        }
    }
}

inline std::vector<StrokePacket> readTransfer(const std::filesystem::path& path) {
    std::ifstream stream(path);
    stream.imbue(std::locale::classic());
    std::string magic;
    size_t count;
    if (!(stream >> magic >> count) || magic != "XOURNALPP_GTK4_INK_V1" || count > maxTransferPoints) {
        throw std::runtime_error("Invalid ink-transfer header");
    }
    std::vector<StrokePacket> strokes;
    size_t points = 0;
    for (size_t i = 0; i < count; ++i) {
        StrokePacket stroke;
        int highlighter, pressureSensitive;
        size_t length;
        if (!(stream >> stroke.page >> highlighter >> pressureSensitive >> stroke.width >> stroke.color[0] >>
              stroke.color[1] >> stroke.color[2] >> length) ||
            highlighter < 0 || highlighter > 1 || pressureSensitive < 0 || pressureSensitive > 1 ||
            length > maxTransferPoints - points) {
            throw std::runtime_error("Invalid ink-transfer stroke header");
        }
        stroke.highlighter = highlighter;
        stroke.pressureSensitive = pressureSensitive;
        stroke.points.reserve(length);
        for (size_t p = 0; p < length; ++p) {
            Sample point;
            if (!(stream >> point.x >> point.y >> point.pressure)) {
                throw std::runtime_error("Incomplete ink-transfer samples");
            }
            stroke.points.push_back(point);
        }
        validate(stroke);
        points += length;
        strokes.push_back(std::move(stroke));
    }
    stream >> std::ws;
    if (!stream.eof()) {
        throw std::runtime_error("Unexpected trailing ink-transfer data");
    }
    return strokes;
}

inline void writeTransfer(const std::filesystem::path& path, const std::vector<StrokePacket>& strokes) {
    size_t points = 0;
    for (const auto& stroke: strokes) {
        validate(stroke);
        if (stroke.points.size() > maxTransferPoints - points) {
            throw std::runtime_error("Ink transfer exceeds sample limit");
        }
        points += stroke.points.size();
    }
    std::ofstream stream(path);
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "XOURNALPP_GTK4_INK_V1 " << strokes.size() << '\n';
    for (const auto& stroke: strokes) {
        stream << stroke.page << ' ' << stroke.highlighter << ' ' << stroke.pressureSensitive << ' ' << stroke.width;
        for (double channel: stroke.color) {
            stream << ' ' << channel;
        }
        stream << ' ' << stroke.points.size() << '\n';
        for (const auto& point: stroke.points) {
            stream << point.x << ' ' << point.y << ' ' << point.pressure << '\n';
        }
    }
    stream.close();
    if (!stream) {
        throw std::runtime_error("Cannot write ink transfer");
    }
}

}  // namespace prototype::bridge
