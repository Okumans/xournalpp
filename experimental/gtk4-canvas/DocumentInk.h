#pragma once

#include "CanvasState.h"
#include "DocumentTransfer.h"

namespace prototype::bridge {

inline std::vector<StrokePacket> transferInk(const std::vector<Ink>& ink) {
    std::vector<StrokePacket> packets;
    packets.reserve(ink.size());
    for (const auto& stroke: ink) {
        const bool whiteout = stroke.brush.tool == Tool::Eraser;
        StrokePacket packet{stroke.page,
                            stroke.brush.tool == Tool::Highlighter,
                            stroke.pressureSensitive && !whiteout,
                            stroke.brush.width,
                            whiteout ? behavior::whiteoutColor : stroke.brush.color,
                            {},
                            whiteout};
        packet.points.reserve(stroke.points.size());
        for (const auto& point: stroke.points) {
            packet.points.push_back({point.x, point.y, point.pressure});
        }
        packets.push_back(std::move(packet));
    }
    return packets;
}

}  // namespace prototype::bridge
