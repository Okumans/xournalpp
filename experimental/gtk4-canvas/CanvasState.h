#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "BehaviorDefaults.h"

namespace prototype {

struct Point {
    double x;
    double y;
    double pressure = 1;
};

enum class Tool { Pen, Highlighter, Eraser };

struct Brush {
    Tool tool = Tool::Pen;
    std::array<double, 3> color = {0.12, 0.35, 0.78};
    double width = 3;
};

struct Ink {
    int page;
    std::vector<Point> points;
    Brush brush;
};

enum class Device { Mouse, Stylus };

// Deliberately independent of GTK. Coordinates are PDF points, not device pixels.
class CanvasState {
public:
    double zoom = 1;
    double offsetX = 0;
    double offsetY = 0;
    double pageWidth = 595;
    double pageHeight = 842;
    int page = 0;
    Brush brush;
    std::vector<Ink> ink;
    std::optional<Ink> current;
    bool pinching = false;
    long long scrollBlockedUntil = 0;

    Point documentPoint(double x, double y, double pressure = 1) const {
        return {(x - offsetX) / zoom, (y - offsetY) / zoom,
                std::isfinite(pressure) ? std::max(behavior::minimumPressure, pressure) : 1.0};
    }

    bool contains(const Point& p) const {
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= 0 && p.y >= 0 && p.x <= pageWidth &&
               p.y <= pageHeight;
    }

    void fit(double width, double height) {
        cancel();
        zoom = std::clamp(std::min(std::max(1.0, width - 64) / pageWidth, std::max(1.0, height - 48) / pageHeight), 0.1,
                          8.0);
        offsetX = (width - pageWidth * zoom) / 2;
        offsetY = (height - pageHeight * zoom) / 2;
    }

    void zoomAt(double value, double x, double y) {
        if (!std::isfinite(value) || current) {
            return;
        }
        const auto anchor = documentPoint(x, y);
        zoom = std::clamp(value, 0.1, 8.0);
        offsetX = x - anchor.x * zoom;
        offsetY = y - anchor.y * zoom;
    }

    bool begin(Device source, double x, double y, double pressure = 1) {
        if (brush.tool == Tool::Eraser || pinching || current || eraseEdit ||
            !contains(documentPoint(x, y, pressure))) {
            return false;
        }
        owner = source;
        current = Ink{page, {documentPoint(x, y, pressure)}, brush};
        return true;
    }

    void append(Device source, double x, double y, double pressure = 1) {
        if (!current || source != owner) {
            return;
        }
        const auto p = documentPoint(x, y, pressure);
        if (contains(p)) {
            auto& last = current->points.back();
            if (std::hypot(p.x - last.x, p.y - last.y) < behavior::motionThreshold) {
                if (current->points.size() == 1 && current->brush.tool == Tool::Pen) {
                    last.pressure = std::max(last.pressure, p.pressure);
                }
                return;
            }
            current->points.push_back(p);
        }
    }

    void finish(Device source, std::optional<double> releasePressure = std::nullopt) {
        if (current && source == owner) {
            if (current->points.size() == 1) {
                const auto point = current->points.front();
                if (releasePressure && std::isfinite(*releasePressure) && current->brush.tool == Tool::Pen) {
                    current->points.front().pressure = std::max(point.pressure, *releasePressure);
                }
                current->points.push_back(point);
            }
            record(Edit{true, {{ink.size(), *current}}});
            ink.push_back(std::move(*current));
            current.reset();
        }
    }

    void cancel() {
        finishErase();
        current.reset();
    }

    void cancel(Device source) {
        if (current && owner == source) {
            cancel();
        }
    }

    void undo() {
        cancel();
        if (!undoStates.empty()) {
            apply(undoStates.back(), false);
            redoStates.push_back(std::move(undoStates.back()));
            undoStates.pop_back();
        }
    }

    void redo() {
        cancel();
        if (!redoStates.empty()) {
            apply(redoStates.back(), true);
            undoStates.push_back(std::move(redoStates.back()));
            redoStates.pop_back();
        }
    }

    bool canUndo() const { return !undoStates.empty(); }
    bool canRedo() const { return !redoStates.empty(); }

    void beginErase() {
        if (!eraseEdit) {
            eraseEdit = Edit{false, {}};
        }
    }

    void finishErase() {
        if (eraseEdit) {
            if (!eraseEdit->strokes.empty()) {
                record(std::move(*eraseEdit));
            }
            eraseEdit.reset();
        }
    }

    void erase(double x, double y) {
        if (current || pinching) {
            return;
        }
        const auto p = documentPoint(x, y);
        auto hits = [&](const Ink& stroke) {
            if (stroke.page != page) {
                return false;
            }
            for (size_t i = 0; i < stroke.points.size(); ++i) {
                const auto& a = stroke.points[i ? i - 1 : 0];
                const auto& b = stroke.points[i];
                const auto dx = b.x - a.x;
                const auto dy = b.y - a.y;
                const auto length = dx * dx + dy * dy;
                const auto t = length ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.0, 1.0) : 0;
                if (std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy) <= 10 / zoom + stroke.brush.width / 2) {
                    return true;
                }
            }
            return false;
        };
        Edit edit{false, {}};
        for (size_t i = 0; i < ink.size(); ++i) {
            if (hits(ink[i])) {
                edit.strokes.push_back({i, ink[i]});
            }
        }
        if (!edit.strokes.empty()) {
            if (eraseEdit) {
                // Translate current positions back to the pre-gesture order so
                // one undo restores deletions made by many eraser samples.
                for (auto& entry: edit.strokes) {
                    for (const auto& removed: eraseEdit->strokes) {
                        if (removed.index <= entry.index) {
                            ++entry.index;
                        } else {
                            break;
                        }
                    }
                }
                for (auto& entry: edit.strokes) {
                    eraseEdit->strokes.push_back(std::move(entry));
                }
                std::sort(eraseEdit->strokes.begin(), eraseEdit->strokes.end(),
                          [](const auto& a, const auto& b) { return a.index < b.index; });
            } else {
                record(std::move(edit));
            }
            std::erase_if(ink, hits);
        }
    }

    void beginPinch() {
        cancel();
        pinching = true;
    }

    void endPinch(long long now) {
        pinching = false;
        scrollBlockedUntil = now + 200000;
    }

    bool blocksScroll(bool discrete, long long now) const {
        return !discrete && (pinching || now < scrollBlockedUntil);
    }

    void resetInput() {
        cancel();
        pinching = false;
        scrollBlockedUntil = 0;
    }

private:
    struct IndexedInk {
        size_t index;
        Ink ink;
    };

    struct Edit {
        bool insertion;
        std::vector<IndexedInk> strokes;
    };

    void record(Edit edit) {
        redoStates.clear();
        // Keep stroke deltas, rather than a full document copy for each edit.
        undoStates.push_back(std::move(edit));
    }

    void apply(const Edit& edit, bool forward) {
        if (edit.insertion == forward) {
            for (const auto& entry: edit.strokes) {
                ink.insert(ink.begin() + entry.index, entry.ink);
            }
        } else {
            for (auto entry = edit.strokes.rbegin(); entry != edit.strokes.rend(); ++entry) {
                ink.erase(ink.begin() + entry->index);
            }
        }
    }

    Device owner = Device::Mouse;
    std::vector<Edit> undoStates;
    std::vector<Edit> redoStates;
    std::optional<Edit> eraseEdit;
};

}  // namespace prototype
