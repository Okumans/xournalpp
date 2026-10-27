#include "CanvasState.h"

namespace prototype {

bool CanvasState::beginLaser(Device source, double x, double y, double pressure, const Brush& laserBrush,
                             bool pressureSensitive) {
    if (!isLaserPointerTool(laserBrush.tool) || laserOwner || current || currentLaser || eraseGesture ||
        selectionGesture || pinching) {
        return false;
    }
    const auto hit = hitPage(x, y, pressure);
    if (!hit) {
        return false;
    }
    auto trail = std::find_if(laserTrails.begin(), laserTrails.end(),
                              [&](const LaserTrail& item) { return item.page == hit->page; });
    if (trail == laserTrails.end()) {
        laserTrails.push_back({hit->page, {}, 255, std::nullopt});
        trail = std::prev(laserTrails.end());
    }
    trail->alpha = 255;
    trail->fadeStart.reset();
    selectActivePage(hit->page);
    currentLaser = Ink{hit->page, {hit->local}, laserBrush, false};
    currentLaser->pressureSensitive = laserBrush.tool == Tool::LaserPointerPen && pressureSensitive;
    currentLaser->layerId = activeLayerId(hit->page);
    currentLaser->originLayer = activeLayerOrigin(hit->page);
    laserOwner = source;
    return true;
}

void CanvasState::appendLaser(Device source, double x, double y, double pressure) {
    if (!currentLaser || !laserOwner || source != *laserOwner || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const auto point = pageLocalPoint(currentLaser->page, x, y, pressure);
    if (!containsOnPage(currentLaser->page, point)) {
        return;
    }
    if (std::hypot(point.x - currentLaser->points.back().x, point.y - currentLaser->points.back().y) <
        behavior::motionThreshold) {
        return;
    }
    currentLaser->points.push_back(point);
}

void CanvasState::finishLaser(Device source, std::optional<double> releasePressure,
                              std::chrono::steady_clock::time_point now) {
    if (!currentLaser || !laserOwner || source != *laserOwner) {
        return;
    }
    auto trail = std::find_if(laserTrails.begin(), laserTrails.end(),
                              [&](const LaserTrail& item) { return item.page == currentLaser->page; });
    if (trail == laserTrails.end()) {
        laserTrails.push_back({currentLaser->page, {}, 255, std::nullopt});
        trail = std::prev(laserTrails.end());
    }
    if (currentLaser->points.size() == 1) {
        if (releasePressure && currentLaser->pressureSensitive && std::isfinite(*releasePressure)) {
            currentLaser->points.front().pressure = std::max(currentLaser->points.front().pressure, *releasePressure);
        }
        currentLaser->points.push_back(currentLaser->points.front());
    }
    trail->strokes.push_back(std::move(*currentLaser));
    trail->alpha = 255;
    trail->fadeStart = now + std::chrono::milliseconds(laserFadeOutTimeMs);
    currentLaser.reset();
    laserOwner.reset();
}

void CanvasState::cancelLaser(Device source, std::chrono::steady_clock::time_point now) {
    if (!laserOwner || source != *laserOwner) {
        return;
    }
    const int pageIndex = currentLaser ? currentLaser->page : -1;
    currentLaser.reset();
    laserOwner.reset();
    auto trail = std::find_if(laserTrails.begin(), laserTrails.end(),
                              [&](const LaserTrail& item) { return item.page == pageIndex; });
    if (trail == laserTrails.end()) {
        return;
    }
    if (trail->strokes.empty()) {
        laserTrails.erase(trail);
    } else {
        trail->alpha = 255;
        trail->fadeStart = now + std::chrono::milliseconds(laserFadeOutTimeMs);
    }
}

bool CanvasState::hasLaserFades() const {
    return std::any_of(laserTrails.begin(), laserTrails.end(),
                       [](const LaserTrail& trail) { return trail.fadeStart.has_value(); });
}

bool CanvasState::advanceLaserFades(std::chrono::steady_clock::time_point now) {
    bool changed = false;
    for (auto& trail: laserTrails) {
        if (!trail.fadeStart || now < *trail.fadeStart) {
            continue;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *trail.fadeStart);
        const auto steps = static_cast<unsigned long long>(elapsed.count()) / behavior::laserPointerFadeStepMs;
        const auto faded = std::min<unsigned long long>(255, steps * behavior::laserPointerFadeAlphaStep);
        const auto nextAlpha = static_cast<uint8_t>(255 - faded);
        changed = changed || nextAlpha != trail.alpha;
        trail.alpha = nextAlpha;
    }
    const auto oldSize = laserTrails.size();
    std::erase_if(laserTrails, [](const LaserTrail& trail) { return trail.alpha == 0; });
    return changed || oldSize != laserTrails.size();
}

void CanvasState::clearLaserPointers() {
    currentLaser.reset();
    laserTrails.clear();
    laserOwner.reset();
}

bool CanvasState::begin(Device source, double x, double y, double pressure, bool eraserEnd, bool shift, bool control,
                        bool alt) {
    if (brush.tool == Tool::Text) {
        return false;
    }
    if (isSelectionTool(brush.tool) && !eraserEnd) {
        return false;
    }
    clearSelection();
    const bool useEraser = eraserEnd || brush.tool == Tool::Eraser;
    const auto hit = hitPage(x, y, useEraser ? 1 : pressure);
    if ((useEraser && eraserMode != EraserMode::Whiteout) || pinching || current || eraseGesture || !hit) {
        return false;
    }
    selectActivePage(hit->page);
    owner = source;
    if (useEraser) {
        current =
                Ink{hit->page, {hit->local}, Brush{Tool::Eraser, behavior::whiteoutColor, activeEraserWidth()}, false};
    } else {
        current = Ink{hit->page, {hit->local}, brush, false};
        if (isFillableTool(brush.tool) && brush.fillEnabled) {
            current->fill = std::clamp(brush.fillOpacity, 0, 255);
        }
    }
    current->layerId = activeLayerId(hit->page);
    current->originLayer = activeLayerOrigin(hit->page);
    rawCurrent = {hit->local};
    if (!useEraser && drawingMode == DrawingMode::Spline) {
        splineKnots = {hit->local};
        splineTangents = {{0, 0}};
        splineHover = hit->local;
        current->points = splineBezierPoints(splineKnots, splineTangents);
    }
    currentShift = shift;
    currentControl = control;
    currentAlt = alt;
    return true;
}

void CanvasState::append(Device source, double x, double y, double pressure, bool shift, bool control, bool alt) {
    if (!current || source != owner) {
        return;
    }
    const auto p = pageLocalPoint(current->page, x, y, current->brush.tool == Tool::Eraser ? 1 : pressure);
    if (containsOnPage(current->page, p)) {
        auto& last = current->points.back();
        if (std::hypot(p.x - last.x, p.y - last.y) < behavior::motionThreshold) {
            if (current->points.size() == 1 && current->brush.tool == Tool::Pen) {
                last.pressure = std::max(last.pressure, p.pressure);
            }
            return;
        }
        rawCurrent.push_back(p);
        currentShift = shift;
        currentControl = control;
        currentAlt = alt;
        if (current->brush.tool == Tool::Eraser || drawingMode == DrawingMode::Freehand ||
            drawingMode == DrawingMode::ShapeRecognizer) {
            current->points.push_back(p);
        } else {
            current->points = makeDrawingPath(drawingMode, rawCurrent.front(), p, current->brush.width, shift, control,
                                              alt, rawCurrent);
        }
    }
}

void CanvasState::refreshCurrentShape(bool shift, bool control, bool alt) {
    if (!current || current->brush.tool == Tool::Eraser || rawCurrent.empty())
        return;
    currentShift = shift;
    currentControl = control;
    currentAlt = alt;
    if (drawingMode == DrawingMode::Spline) {
        refreshSplinePreview();
    } else if (drawingMode != DrawingMode::Freehand && drawingMode != DrawingMode::ShapeRecognizer) {
        current->points = makeDrawingPath(drawingMode, rawCurrent.front(), rawCurrent.back(), current->brush.width,
                                          shift, control, alt, rawCurrent);
    }
}

bool CanvasState::addSplineKnot(double x, double y, double pressure) {
    if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty())
        return false;
    const auto hit = hitPage(x, y);
    if (!hit || hit->page != current->page)
        return false;
    auto point = hit->local;
    point.pressure = std::isfinite(pressure) && pressure >= 0 ? pressure : 1;
    splineKnots.push_back(point);
    splineTangents.push_back({0, 0});
    splineHover = point;
    refreshSplinePreview();
    return true;
}

void CanvasState::moveSplineTangent(double x, double y, double pressure) {
    if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty())
        return;
    const auto point = pageLocalPoint(current->page, x, y);
    if (!containsOnPage(current->page, point))
        return;
    auto& knot = splineKnots.back();
    splineTangents.back() = {point.x - knot.x, point.y - knot.y};
    splineHover = Point{point.x, point.y, std::isfinite(pressure) && pressure >= 0 ? pressure : 1};
    refreshSplinePreview();
}

void CanvasState::updateSplineHover(double x, double y) {
    if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty())
        return;
    const auto point = pageLocalPoint(current->page, x, y);
    if (containsOnPage(current->page, point)) {
        splineHover = point;
        refreshSplinePreview();
    }
}

bool CanvasState::removeLastSplineKnot() {
    if (!current || drawingMode != DrawingMode::Spline || splineKnots.size() <= 1)
        return false;
    splineKnots.pop_back();
    splineTangents.pop_back();
    splineHover = splineKnots.back();
    refreshSplinePreview();
    return true;
}

bool CanvasState::closeSplinePath() {
    if (!current || drawingMode != DrawingMode::Spline || splineKnots.size() < 2)
        return false;
    const auto& first = splineKnots.front();
    const auto& last = splineKnots.back();
    if (std::hypot(first.x - last.x, first.y - last.y) > 1e-8) {
        splineKnots.push_back(first);
        splineTangents.push_back(splineTangents.front());
    }
    splineHover.reset();
    refreshSplinePreview();
    return true;
}

bool CanvasState::moveLastSplineKnot(double dx, double dy) {
    if (!current || drawingMode != DrawingMode::Spline || splineKnots.empty())
        return false;
    splineKnots.back().x += dx;
    splineKnots.back().y += dy;
    splineHover = splineKnots.back();
    refreshSplinePreview();
    return true;
}

bool CanvasState::rotateLastSplineTangent(double degrees) {
    if (!current || drawingMode != DrawingMode::Spline || splineTangents.empty())
        return false;
    const double angle = degrees * M_PI / 180.0;
    const auto old = splineTangents.back();
    splineTangents.back() = {std::cos(angle) * old.x + std::sin(angle) * old.y,
                             -std::sin(angle) * old.x + std::cos(angle) * old.y};
    refreshSplinePreview();
    return true;
}

bool CanvasState::scaleLastSplineTangent(double factor) {
    if (!current || drawingMode != DrawingMode::Spline || splineTangents.empty())
        return false;
    splineTangents.back().x *= factor;
    splineTangents.back().y *= factor;
    refreshSplinePreview();
    return true;
}

void CanvasState::finish(Device source, std::optional<double> releasePressure) {
    if (current && source == owner) {
        if (current->brush.tool != Tool::Eraser && drawingMode == DrawingMode::Spline) {
            if (splineKnots.size() < 2) {
                current.reset();
                rawCurrent.clear();
                splineKnots.clear();
                splineTangents.clear();
                splineHover.reset();
                return;
            }
            current->points = splineBezierPoints(splineKnots, splineTangents);
        }
        if (current->brush.tool != Tool::Eraser && drawingMode == DrawingMode::ShapeRecognizer) {
            if (auto recognized = recognizeShape(rawCurrent, shapeRecognizerMinSize)) {
                current->points = std::move(*recognized);
            }
        }
        if (current->brush.tool != Tool::Eraser && drawingMode != DrawingMode::Freehand && current->points.size() < 2) {
            current.reset();
            rawCurrent.clear();
            return;
        }
        if (current->points.size() == 1) {
            const auto point = current->points.front();
            if (releasePressure && std::isfinite(*releasePressure) && current->brush.tool == Tool::Pen) {
                current->points.front().pressure = std::max(point.pressure, *releasePressure);
            }
            current->points.push_back(point);
        }
        if ((current->brush.tool == Tool::Pen || current->brush.tool == Tool::Highlighter ||
             current->brush.tool == Tool::Eraser) &&
            current->pluginReference == 0) {
            current->pluginReference = allocatePluginReference();
        }
        current->layerOrder = nextOriginOrder(current->page, current->originLayer);
        Edit edit;
        edit.changes.push_back({ink.size(), {}, ink.size(), {*current}});
        record(std::move(edit));
        ink.push_back(std::move(*current));
        current.reset();
        rawCurrent.clear();
        splineKnots.clear();
        splineTangents.clear();
        splineHover.reset();
    }
}

void CanvasState::refreshSplinePreview() {
    if (current && drawingMode == DrawingMode::Spline) {
        current->points = splineBezierPoints(splineKnots, splineTangents, splineHover);
    }
}

}  // namespace prototype
