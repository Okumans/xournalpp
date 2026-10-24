#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

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
                std::isfinite(pressure) ? std::clamp(pressure, 0.05, 1.0) : 1.0};
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
        if (brush.tool == Tool::Eraser || pinching || current || !contains(documentPoint(x, y, pressure))) {
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
            current->points.push_back(p);
        }
    }

    void finish(Device source) {
        if (current && source == owner) {
            record(Edit{true, {{ink.size(), *current}}});
            ink.push_back(std::move(*current));
            current.reset();
        }
    }

    void cancel() { current.reset(); }

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
            record(std::move(edit));
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
        if (undoStates.size() == 128) {
            undoStates.erase(undoStates.begin());
        }
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
};

}  // namespace prototype
