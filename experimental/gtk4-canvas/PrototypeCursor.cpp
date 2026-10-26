#include <array>
#include <cmath>

#include "Prototype.h"

namespace prototype::gtk4 {

namespace {

const char* resizeCursor(double angle) {
    angle = std::fmod(angle, 180.0);
    if (angle < 0) {
        angle += 180.0;
    }
    if (angle < 22.5 || angle >= 157.5) {
        return "ew-resize";
    }
    if (angle < 67.5) {
        return "nwse-resize";
    }
    if (angle < 112.5) {
        return "ns-resize";
    }
    return "nesw-resize";
}

const char* selectionResizeCursor(size_t handle, const std::array<prototype::Point, 9>& handles) {
    const double topAngle = std::atan2(handles[2].y - handles[0].y, handles[2].x - handles[0].x) * 180.0 / G_PI;
    switch (handle) {
        case 0:
        case 4:
            return resizeCursor(topAngle + 45.0);
        case 1:
        case 5:
            return resizeCursor(topAngle + 90.0);
        case 2:
        case 6:
            return resizeCursor(topAngle + 135.0);
        default:
            return resizeCursor(topAngle);
    }
}

const char* imageResizeCursor(size_t handle, const std::array<prototype::Point, 4>& corners) {
    const double topAngle = std::atan2(corners[1].y - corners[0].y, corners[1].x - corners[0].x) * 180.0 / G_PI;
    return resizeCursor(topAngle + (handle == 0 || handle == 2 ? 45.0 : 135.0));
}

bool containsPoint(const std::array<prototype::Point, 4>& quad, prototype::Point point) {
    bool positive = false;
    bool negative = false;
    for (size_t i = 0; i < quad.size(); ++i) {
        const auto& a = quad[i];
        const auto& b = quad[(i + 1) % quad.size()];
        const double cross = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
        positive = positive || cross > 1e-8;
        negative = negative || cross < -1e-8;
        if (positive && negative) {
            return false;
        }
    }
    return true;
}

}  // namespace

void Prototype::setCanvasCursor(const char* name) {
    if (!canvas) {
        return;
    }
    const char* cursorName = name && *name ? name : "default";
    if (canvasCursorName == cursorName) {
        return;
    }
    canvasCursorName = cursorName;
    gtk_widget_set_cursor_from_name(canvas, cursorName);
}

void Prototype::refreshCanvasCursor() {
    if (pointerPosition) {
        updateCanvasCursor(pointerPosition->x, pointerPosition->y);
    } else {
        updateCanvasCursor(-1, -1);
    }
}

void Prototype::updateCanvasCursor(double x, double y) {
    if (!canvas) {
        return;
    }

    const auto tool = state.brush.tool;
    bool panActive = input.panning();
    for (auto* gesture: panGestures) {
        const auto* pan = static_cast<const PanState*>(g_object_get_data(G_OBJECT(gesture), "pan-state"));
        panActive = panActive || (pan && pan->enabled);
    }
    if (tool == Tool::Hand || input.spaceHeld || panActive) {
        setCanvasCursor(panActive ? "grabbing" : "grab");
        return;
    }
    if (state.movingSelection()) {
        const auto handle = state.movingSelectionHandle();
        if (!handle || *handle < 0) {
            setCanvasCursor("grabbing");
        } else if (*handle == 8) {
            setCanvasCursor("crosshair");
        } else {
            setCanvasCursor(selectionResizeCursor(static_cast<size_t>(*handle), state.selectionHandles()));
        }
        return;
    }
    if (state.verticalSpacePreview()) {
        setCanvasCursor("ns-resize");
        return;
    }
    if (imageGesture) {
        if (imageGesture->mode == ImageGesture::Mode::Move) {
            setCanvasCursor("grabbing");
        } else if (imageGesture->mode == ImageGesture::Mode::Rotate) {
            setCanvasCursor("crosshair");
        } else {
            const auto corners = prototype::canvasImageCorners(imageGesture->original);
            setCanvasCursor(imageResizeCursor(imageGesture->corner, corners));
        }
        return;
    }

    if (state.hasSelection()) {
        if (const auto bounds = state.selectionBox()) {
            const auto handles = state.selectionHandles();
            for (size_t i = 0; i < handles.size(); ++i) {
                const auto screen = state.pageScreenPoint(bounds->page, handles[i].x, handles[i].y);
                if (std::hypot(screen.x - x, screen.y - y) > 7.0) {
                    continue;
                }
                setCanvasCursor(i == 8 ? "crosshair" : selectionResizeCursor(i, handles));
                return;
            }
            const std::array<prototype::Point, 4> quad{handles[0], handles[2], handles[4], handles[6]};
            if (containsPoint(quad, state.pageLocalPoint(bounds->page, x, y))) {
                setCanvasCursor("all-scroll");
                return;
            }
        }
    }

    const auto hit = state.hitPage(x, y);
    if (!hit) {
        setCanvasCursor("default");
        return;
    }

    if (tool == Tool::SmartSelect) {
        if (selectedImageIndex && *selectedImageIndex < state.images.size()) {
            const auto& image = state.images[*selectedImageIndex];
            if (image.page == hit->page && image.layerId == state.activeLayerId(hit->page) &&
                state.layerVisible(hit->page, image.layerId)) {
                if (const auto handle = imageHandleAt(image, hit->local)) {
                    if (*handle == 4) {
                        setCanvasCursor("crosshair");
                    } else {
                        const auto corners = prototype::canvasImageCorners(image);
                        setCanvasCursor(imageResizeCursor(*handle, corners));
                    }
                    return;
                }
            }
        }
        setCanvasCursor(state.pickText(hit->page, hit->local) ? "text" : "default");
        return;
    }

    switch (tool) {
        case Tool::Text:
        case Tool::Latex:
        case Tool::SelectPdfTextLinear:
            setCanvasCursor("text");
            break;
        case Tool::Link:
        case Tool::PlayObject:
            setCanvasCursor("pointer");
            break;
        case Tool::VerticalSpace:
            setCanvasCursor("ns-resize");
            break;
        case Tool::SelectRectangle:
        case Tool::SelectLasso:
        case Tool::SelectPdfTextRect:
        case Tool::Eraser:
        case Tool::Highlighter:
        case Tool::LaserPointerPen:
        case Tool::LaserPointerHighlighter:
            setCanvasCursor("crosshair");
            break;
        case Tool::Pen:
            setCanvasCursor(input.active() ? "crosshair" : "default");
            break;
        case Tool::SelectObject:
        case Tool::FloatingToolbox:
        case Tool::Hand:
        case Tool::SmartSelect:
            setCanvasCursor("default");
            break;
    }
}

}  // namespace prototype::gtk4
