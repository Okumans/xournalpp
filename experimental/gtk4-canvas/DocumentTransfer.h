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

#include "CanvasLineStyle.h"
#include "CanvasLayers.h"

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
    bool whiteout = false;
    size_t layerOrder = 0;
    size_t layerId = 1;
    size_t originLayer = newLayerOrigin;
    int fill = -1;
    int capStyle = 0;
    LineDashPattern lineDashes;

    StrokePacket() = default;
    StrokePacket(int page, bool highlighter, bool pressureSensitive, double width, std::array<double, 3> color,
                 std::vector<Sample> points, bool whiteout = false):
            page(page), highlighter(highlighter), pressureSensitive(pressureSensitive), width(width), color(color),
            points(std::move(points)), whiteout(whiteout) {}
};

inline constexpr size_t maxTransferPoints = 1000000;
inline constexpr size_t maxTransferDashValues = 65536;

inline void validate(const StrokePacket& stroke) {
    if (stroke.page < 0 || stroke.layerId == 0 || (stroke.highlighter && stroke.whiteout) ||
        !std::isfinite(stroke.width) || stroke.width <= 0 || stroke.points.empty() || stroke.fill < -1 ||
        stroke.fill > 255 || stroke.capStyle < 0 || stroke.capStyle > 2 ||
        (stroke.fill >= 0 && stroke.whiteout) || !validLineDashPattern(stroke.lineDashes)) {
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
    if (!(stream >> magic >> count) ||
        (magic != "XOURNALPP_GTK4_INK_V1" && magic != "XOURNALPP_GTK4_INK_V2" &&
         magic != "XOURNALPP_GTK4_INK_V3" && magic != "XOURNALPP_GTK4_INK_V4" &&
         magic != "XOURNALPP_GTK4_INK_V5") ||
        count > maxTransferPoints) {
        throw std::runtime_error("Invalid ink-transfer header");
    }
    const bool hasLayerOrder = magic == "XOURNALPP_GTK4_INK_V2" || magic == "XOURNALPP_GTK4_INK_V3" ||
                               magic == "XOURNALPP_GTK4_INK_V4" || magic == "XOURNALPP_GTK4_INK_V5";
    const bool hasLayerIdentity = magic == "XOURNALPP_GTK4_INK_V3" || magic == "XOURNALPP_GTK4_INK_V4" ||
                                  magic == "XOURNALPP_GTK4_INK_V5";
    const bool hasStrokeAppearance = magic == "XOURNALPP_GTK4_INK_V4" || magic == "XOURNALPP_GTK4_INK_V5";
    const bool hasLineStyle = magic == "XOURNALPP_GTK4_INK_V5";
    std::vector<StrokePacket> strokes;
    size_t points = 0;
    size_t dashValues = 0;
    for (size_t i = 0; i < count; ++i) {
        StrokePacket stroke;
        int tool, pressureSensitive;
        size_t length;
        if (hasLayerIdentity) {
            if (!(stream >> stroke.page >> stroke.layerId >> stroke.originLayer >> stroke.layerOrder >> tool >>
                  pressureSensitive >> stroke.width)) {
                throw std::runtime_error("Invalid ink-transfer stroke header");
            }
            if (hasStrokeAppearance && !(stream >> stroke.fill >> stroke.capStyle)) {
                throw std::runtime_error("Invalid ink-transfer stroke header");
            }
            if (hasLineStyle) {
                size_t dashCount = 0;
                if (!(stream >> dashCount) || dashCount > maxLineDashValues - dashValues) {
                    throw std::runtime_error("Invalid ink-transfer line style");
                }
                dashValues += dashCount;
                stroke.lineDashes.reserve(dashCount);
                for (size_t dash = 0; dash < dashCount; ++dash) {
                    double value = 0;
                    if (!(stream >> value)) {
                        throw std::runtime_error("Incomplete ink-transfer line style");
                    }
                    stroke.lineDashes.push_back(value);
                }
            }
            if (!(stream >> stroke.color[0] >> stroke.color[1] >> stroke.color[2] >> length)) {
                throw std::runtime_error("Invalid ink-transfer stroke header");
            }
        }
        if (!hasLayerIdentity && hasLayerOrder && !(stream >> stroke.page >> stroke.layerOrder >> tool >> pressureSensitive >> stroke.width >>
                               stroke.color[0] >> stroke.color[1] >> stroke.color[2] >> length)) {
            throw std::runtime_error("Invalid ink-transfer stroke header");
        }
        if (!hasLayerOrder) {
            stroke.layerOrder = i;
        }
        if (!hasLayerIdentity) {
            stroke.layerId = legacyTopLayer;
            stroke.originLayer = newLayerOrigin;
        }
        if ((!hasLayerOrder && !(stream >> stroke.page >> tool >> pressureSensitive >> stroke.width >> stroke.color[0] >>
                                 stroke.color[1] >> stroke.color[2] >> length)) ||
            tool < 0 || tool > 2 || pressureSensitive < 0 || pressureSensitive > 1 ||
            length > maxTransferPoints - points) {
            throw std::runtime_error("Invalid ink-transfer stroke header");
        }
        stroke.highlighter = tool == 1;
        stroke.whiteout = tool == 2;
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
    size_t dashValues = 0;
    for (const auto& stroke: strokes) {
        validate(stroke);
        if (stroke.points.size() > maxTransferPoints - points) {
            throw std::runtime_error("Ink transfer exceeds sample limit");
        }
        if (stroke.lineDashes.size() > maxTransferDashValues - dashValues) {
            throw std::runtime_error("Ink transfer exceeds line-style limit");
        }
        points += stroke.points.size();
        dashValues += stroke.lineDashes.size();
    }
    std::ofstream stream(path);
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "XOURNALPP_GTK4_INK_V5 " << strokes.size() << '\n';
    for (const auto& stroke: strokes) {
        const int tool = stroke.whiteout ? 2 : (stroke.highlighter ? 1 : 0);
        stream << stroke.page << ' ' << stroke.layerId << ' ' << stroke.originLayer << ' ' << stroke.layerOrder << ' '
               << tool << ' ' << stroke.pressureSensitive << ' ' << stroke.width << ' ' << stroke.fill << ' '
               << stroke.capStyle << ' ' << stroke.lineDashes.size();
        for (double dash: stroke.lineDashes) {
            stream << ' ' << dash;
        }
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
