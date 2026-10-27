#include "CanvasState.h"

namespace prototype {

bool CanvasState::beginVerticalSpace(double x, double y, bool moveAbove) {
    const auto hit = hitPage(x, y);
    if (!hit || current || eraseGesture || selectionGesture || verticalSpace || pinching) {
        return false;
    }

    selectActivePage(hit->page);
    VerticalSpacePreview preview;
    preview.page = hit->page;
    preview.layerId = activeLayerId(hit->page);
    preview.startY = preview.endY = snapVertical(hit->page, hit->local.y);
    preview.moveAbove = moveAbove;
    collectVerticalSpaceObjects(preview);
    verticalSpace = std::move(preview);
    return true;
}

void CanvasState::moveVerticalSpace(double x, double y, bool moveAbove) {
    if (!verticalSpace || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    setVerticalSpaceDirection(moveAbove);
    const auto local = pageLocalPoint(verticalSpace->page, x, y);
    verticalSpace->endY = snapVertical(verticalSpace->page, local.y);
}

void CanvasState::setVerticalSpaceDirection(bool moveAbove) {
    if (verticalSpace && verticalSpace->moveAbove != moveAbove) {
        verticalSpace->moveAbove = moveAbove;
        collectVerticalSpaceObjects(*verticalSpace);
    }
}

bool CanvasState::finishVerticalSpace() {
    if (!verticalSpace) {
        return false;
    }
    auto preview = std::move(*verticalSpace);
    verticalSpace.reset();
    const double deltaY = preview.endY - preview.startY;
    if (deltaY == 0 || (preview.inkIndices.empty() && preview.textIndices.empty() && preview.imageIndices.empty())) {
        return false;
    }

    Edit edit;
    edit.selectionBefore = edit.selectionAfter = selected;
    edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
    edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
    for (const auto index: preview.inkIndices) {
        if (index >= ink.size()) {
            return false;
        }
        auto moved = ink[index];
        for (auto& point: moved.points) {
            point.y += deltaY;
            if (!std::isfinite(point.y)) {
                return false;
            }
        }
        edit.changes.push_back({index, {ink[index]}, index, {std::move(moved)}});
    }
    for (const auto index: preview.textIndices) {
        if (index >= texts.size()) {
            return false;
        }
        auto moved = texts[index];
        moved.y += deltaY;
        if (!validTextPage(moved)) {
            return false;
        }
        edit.textChanges.push_back({index, {texts[index]}, index, {std::move(moved)}});
    }
    for (const auto index: preview.imageIndices) {
        if (index >= images.size()) {
            return false;
        }
        auto moved = images[index];
        moved.transform.y += deltaY;
        if (!validCanvasImage(moved)) {
            return false;
        }
        edit.imageChanges.push_back({index, {images[index]}, index, {std::move(moved)}});
    }
    apply(edit, true);
    record(std::move(edit));
    return true;
}

void CanvasState::cancelVerticalSpace() { verticalSpace.reset(); }

void CanvasState::cancelSelection() {
    if (selectionGesture) {
        selected = std::move(selectionGesture->previousSelection);
        selectedTexts = std::move(selectionGesture->previousTextSelection);
        selectedImages = std::move(selectionGesture->previousImageSelection);
        selectionFrame = std::move(selectionGesture->previousSelectionFrame);
        selectionGesture.reset();
    }
}

void CanvasState::clearSelection() {
    cancelSelection();
    cancelVerticalSpace();
    selected.clear();
    selectedTexts.clear();
    selectedImages.clear();
    selectionFrame.reset();
}

void CanvasState::extendLassoAtPageEdges(std::vector<Point>& boundary, const PageSize& size) {
    if (boundary.size() <= 2 || size.width <= 0 || size.height <= 0)
        return;
    const auto onEdge = [&](const Point& p) {
        return p.x <= 1 || p.x >= size.width - 1 || p.y <= 1 || p.y >= size.height - 1;
    };
    if (std::none_of(boundary.begin(), boundary.end(), onEdge))
        return;
    const auto project = [&](const Point& p) {
        return Point{p.x <= 1              ? std::numeric_limits<double>::lowest() :
                     p.x >= size.width - 1 ? std::numeric_limits<double>::max() :
                                             p.x,
                     p.y <= 1               ? std::numeric_limits<double>::lowest() :
                     p.y >= size.height - 1 ? std::numeric_limits<double>::max() :
                                              p.y};
    };
    std::vector<Point> extended;
    extended.reserve(boundary.size() * 2);
    for (size_t i = 0; i < boundary.size(); ++i) {
        const auto& current = boundary[i];
        if (!onEdge(current)) {
            extended.push_back(current);
            continue;
        }
        const bool previousOnEdge = onEdge(boundary[(i + boundary.size() - 1) % boundary.size()]);
        const bool nextOnEdge = onEdge(boundary[(i + 1) % boundary.size()]);
        if (!previousOnEdge)
            extended.push_back(current);
        if (!previousOnEdge || nextOnEdge)
            extended.push_back(project(current));
        if (previousOnEdge && !nextOnEdge)
            extended.push_back(current);
    }
    boundary = std::move(extended);
}

bool CanvasState::beginSelection(double x, double y, bool forceRectangle) {
    auto hit = hitPage(x, y);
    if (!hit && !forceRectangle) {
        hit = hitSelectionHandle(x, y);
    }
    if (!hit || current || eraseGesture || selectionGesture || pinching) {
        return false;
    }
    if (selectionFrame && !selectionFrameMatchesCurrentSelection()) {
        selectionFrame.reset();
    }
    SelectionGesture gesture;
    gesture.page = hit->page;
    gesture.targetPage = hit->page;
    gesture.start = gesture.latest = hit->local;
    gesture.previousSelection = selected;
    gesture.previousTextSelection = selectedTexts;
    gesture.previousImageSelection = selectedImages;
    gesture.previousSelectionFrame = selectionFrame;
    gesture.baseRotation = selectionFrame ? selectionFrame->rotation : 0;
    gesture.rotationTarget = gesture.baseRotation;
    gesture.baseFrameTransform = selectionFrame ? selectionFrame->transform : SelectionTransform{};
    gesture.multiLayer = selectionAcrossLayers;
    auto bounds = selectionLocalBounds();
    if (bounds && bounds->page == hit->page && !forceRectangle) {
        const auto handles = selectionHandles();
        for (int n = 0; n < 9; ++n) {
            if (std::hypot(handles[n].x - hit->local.x, handles[n].y - hit->local.y) <= 5 / zoom) {
                gesture.handle = n;
                break;
            }
        }
    }
    const auto inverseFrame = inverseSelectionTransform(gesture.baseFrameTransform);
    const auto framePoint = inverseFrame.apply(hit->local);
    const auto paddingX =
            5 / (zoom * std::max(0.01, std::hypot(gesture.baseFrameTransform.xx, gesture.baseFrameTransform.yx)));
    const auto paddingY =
            5 / (zoom * std::max(0.01, std::hypot(gesture.baseFrameTransform.xy, gesture.baseFrameTransform.yy)));
    gesture.moving =
            !forceRectangle &&
            (gesture.handle >= 0 || (bounds && bounds->page == hit->page && framePoint.x >= bounds->x0 - paddingX &&
                                     framePoint.x <= bounds->x1 + paddingX && framePoint.y >= bounds->y0 - paddingY &&
                                     framePoint.y <= bounds->y1 + paddingY));
    if (brush.tool == Tool::SelectObject) {
        if (gesture.moving) {
            gesture.indices = selected;
            gesture.textIndices = selectedTexts;
            gesture.imageIndices = selectedImages;
            gesture.bounds = gesture.handle >= 0 ? handleBounds(*bounds) : *bounds;
            selectionGesture = std::move(gesture);
        } else {
            selectObjectAt(hit->page, hit->local, forceRectangle);
        }
        selectActivePage(hit->page);
        return true;
    }
    if (!gesture.moving && brush.tool == Tool::SmartSelect && !forceRectangle) {
        const auto pickedObject = pickSmartSelectObject(hit->page, hit->local);
        if (pickedObject) {
            selected.clear();
            selectedTexts.clear();
            selectedImages.clear();
            selectionFrame.reset();
            gesture.baseRotation = 0;
            gesture.rotationTarget = 0;
            gesture.baseFrameTransform = {};
            switch (pickedObject->kind) {
                case SelectableObjectKind::Stroke:
                    selected = {pickedObject->index};
                    break;
                case SelectableObjectKind::Text:
                    selectedTexts = {pickedObject->index};
                    break;
                case SelectableObjectKind::Image:
                    selectedImages = {pickedObject->index};
                    break;
            }
            bounds = selectionLocalBounds();
            gesture.moving = true;
        }
    }
    if (gesture.moving) {
        gesture.indices = selected;
        gesture.textIndices = selectedTexts;
        gesture.imageIndices = selectedImages;
        gesture.bounds = gesture.handle >= 0 ? handleBounds(*bounds, gesture.baseFrameTransform) : *bounds;
        gesture.snapAnchor =
                closestSelectionCorner(transformedCorners(*bounds, gesture.baseFrameTransform), gesture.start);
    } else {
        selected.clear();
        selectedTexts.clear();
        selectedImages.clear();
        selectionFrame.reset();
        gesture.lasso = brush.tool == Tool::SelectLasso && !forceRectangle;
        gesture.boundary.push_back(hit->local);
        gesture.additive = forceRectangle && bounds && bounds->page == hit->page;
    }
    selectActivePage(hit->page);
    selectionGesture = std::move(gesture);
    return true;
}

void CanvasState::moveSelectionPointer(double x, double y, bool altHeld) {
    if (!selectionGesture || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    auto& g = *selectionGesture;
    if (g.moving && g.handle < 0) {
        if (const auto hit = hitPage(x, y))
            g.targetPage = hit->page;
    }
    g.latest = pageLocalPoint(g.targetPage, x, y);
    if (g.moving) {
        const auto previous = g.transform;
        const auto previousFrame = g.frameTransform;
        g.transform = {};
        g.frameTransform = {};
        if (g.handle < 0) {
            const double dx = g.latest.x - g.start.x;
            const double dy = g.latest.y - g.start.y;
            const Point candidate{g.snapAnchor.x + dx, g.snapAnchor.y + dy};
            const Point snapped = snapSelectionMove(g.targetPage, candidate, altHeld);
            g.transform.x0 = snapped.x - g.snapAnchor.x;
            g.transform.y0 = snapped.y - g.snapAnchor.y;
            g.frameTransform = g.transform;
        } else if (g.handle == 8) {
            const auto corners = transformedCorners(g.bounds, g.baseFrameTransform);
            const auto cx = (corners[0].x + corners[2].x) / 2;
            const auto cy = (corners[0].y + corners[2].y) / 2;
            auto angle = std::atan2(g.latest.y - cy, g.latest.x - cx) - std::atan2(g.start.y - cy, g.start.x - cx);
            if (!g.imageIndices.empty() && snapRotationEnabled != altHeld) {
                angle = canvasImageRotationDelta(std::atan2(g.start.y - cy, g.start.x - cx),
                                                 std::atan2(g.latest.y - cy, g.latest.x - cx), true,
                                                 snapRotationTolerance);
            }
            g.rotationTarget = g.baseRotation + angle;
            const auto cosine = std::cos(angle), sine = std::sin(angle);
            g.transform = {cosine, sine, -sine, cosine, cx - cosine * cx + sine * cy, cy - sine * cx - cosine * cy, 1};
        } else {
            constexpr int sx[] = {-1, 0, 1, 1, 1, 0, -1, -1};
            constexpr int sy[] = {-1, -1, -1, 0, 1, 1, 1, 0};
            const auto xs = sx[g.handle], ys = sy[g.handle];
            const auto width = std::max(g.bounds.x1 - g.bounds.x0, 5 / zoom);
            const auto height = std::max(g.bounds.y1 - g.bounds.y0, 5 / zoom);
            const auto ax = xs < 0 ? g.bounds.x1 : g.bounds.x0;
            const auto ay = ys < 0 ? g.bounds.y1 : g.bounds.y0;
            const auto vx = xs * width, vy = ys * height;
            const auto latest = inverseSelectionTransform(g.baseFrameTransform).apply(g.latest);
            const auto projection = (xs ? (latest.x - ax) * vx : 0) + (ys ? (latest.y - ay) * vy : 0);
            auto factor = projection / (vx * vx + vy * vy);
            const auto constrained = xs && ys ? std::min(width, height) : (xs ? width : height);
            factor = std::copysign(std::max(std::abs(factor), (5 / zoom) / constrained), factor);
            const auto rawFactor = factor;
            auto fx = xs ? factor : 1;
            auto fy = ys ? factor : 1;
            g.frameTransform = {fx, 0, 0, fy, ax * (1 - fx), ay * (1 - fy), std::sqrt(std::abs(fx * fy))};

            const auto candidateEdgeX =
                    g.frameTransform.xx * (xs < 0 ? g.bounds.x0 : g.bounds.x1) + g.frameTransform.x0;
            const auto candidateEdgeY =
                    g.frameTransform.yy * (ys < 0 ? g.bounds.y0 : g.bounds.y1) + g.frameTransform.y0;
            const Point candidateHandleLocal{xs ? candidateEdgeX : (g.bounds.x0 + g.bounds.x1) / 2,
                                             ys ? candidateEdgeY : (g.bounds.y0 + g.bounds.y1) / 2};
            const auto candidateHandle = g.baseFrameTransform.apply(candidateHandleLocal);
            const Point snappedHandle{snapSelectionResizeAxis(g.targetPage, candidateHandle.x, true, altHeld),
                                      snapSelectionResizeAxis(g.targetPage, candidateHandle.y, false, altHeld)};
            const auto snappedLocal = inverseSelectionTransform(g.baseFrameTransform).apply(snappedHandle);
            const auto snappedX = xs ? snappedLocal.x : candidateEdgeX;
            const auto snappedY = ys ? snappedLocal.y : candidateEdgeY;
            const auto dx = snappedX - candidateEdgeX;
            const auto dy = snappedY - candidateEdgeY;
            const auto currentWidth = (g.bounds.x1 - g.bounds.x0) * g.frameTransform.xx;
            const auto currentHeight = (g.bounds.y1 - g.bounds.y0) * g.frameTransform.yy;
            const auto snapFactorX =
                    xs && std::abs(currentWidth) > 5 / zoom ? (currentWidth + dx * xs) / currentWidth : 1.0;
            const auto snapFactorY =
                    ys && std::abs(currentHeight) > 5 / zoom ? (currentHeight + dy * ys) / currentHeight : 1.0;
            auto correction = (((std::abs(dx) < std::abs(dy)) && snapFactorX != 1) || snapFactorY == 1) ? snapFactorX :
                                                                                                          snapFactorY;
            if (std::abs(currentWidth * correction) < 5 / zoom || std::abs(currentHeight * correction) < 5 / zoom) {
                correction = 1;
            }
            factor = rawFactor * correction;
            fx = xs ? factor : 1;
            fy = ys ? factor : 1;
            g.frameTransform = {fx, 0, 0, fy, ax * (1 - fx), ay * (1 - fy), std::sqrt(std::abs(fx * fy))};

            g.transform = composeSelectionTransforms(
                    g.baseFrameTransform,
                    composeSelectionTransforms(g.frameTransform, inverseSelectionTransform(g.baseFrameTransform)));
            g.transform.widthScale = g.frameTransform.widthScale;
        }
        const auto& m = g.transform;
        if (!std::isfinite(m.xx) || !std::isfinite(m.xy) || !std::isfinite(m.yx) || !std::isfinite(m.yy) ||
            !std::isfinite(m.x0) || !std::isfinite(m.y0) || !std::isfinite(m.widthScale) || m.widthScale <= 0)
            g.transform = previous;
        const auto& frame = g.frameTransform;
        if (!std::isfinite(frame.xx) || !std::isfinite(frame.xy) || !std::isfinite(frame.yx) ||
            !std::isfinite(frame.yy) || !std::isfinite(frame.x0) || !std::isfinite(frame.y0) ||
            !std::isfinite(frame.widthScale) || frame.widthScale <= 0) {
            g.frameTransform = previousFrame;
        }
    } else if (g.lasso && g.boundary.size() < 1000000 &&
               std::hypot(g.latest.x - g.boundary.back().x, g.latest.y - g.boundary.back().y) >= .3) {
        g.boundary.push_back(g.latest);
    }
}

bool CanvasState::edgePanSelectionAtPointer(double x, double y, double viewportWidth, double viewportHeight,
                                            bool altHeld) {
    if (!selectionCanEdgePan() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(viewportWidth) ||
        !std::isfinite(viewportHeight) || viewportWidth <= 0 || viewportHeight <= 0 || !std::isfinite(zoom) ||
        zoom <= 0 || !std::isfinite(edgePanSpeed) || !std::isfinite(edgePanMaxMultiplier) || edgePanSpeed <= 0 ||
        edgePanMaxMultiplier <= 0) {
        return false;
    }
    const auto bounds = selectionBox();
    if (!bounds) {
        return false;
    }

    const auto pageIndex = bounds->page;
    const auto screenX = offsetX + (pageLeft(pageIndex) + bounds->x0) * zoom;
    const auto screenY = offsetY + (pageTop(pageIndex) + bounds->y0) * zoom;
    const auto screenWidth = (bounds->x1 - bounds->x0) * zoom;
    const auto screenHeight = (bounds->y1 - bounds->y0) * zoom;

    const auto scrollAmount = [&](double viewportLength, double bboxMin, double bboxLength, double pointer,
                                  double layoutLength, double& offset) {
        if (!std::isfinite(viewportLength) || !std::isfinite(bboxMin) || !std::isfinite(bboxLength) ||
            !std::isfinite(pointer) || !std::isfinite(layoutLength) || viewportLength <= 0 || bboxLength <= 0) {
            return;
        }
        const auto bboxMax = bboxMin + bboxLength;
        const auto isLargeSelection = bboxLength > viewportLength;
        double multiplier = 0;
        int direction = 0;
        if (isLargeSelection) {
            const auto pointerDiff = pointer - viewportLength / 2.0;
            multiplier = edgePanMaxMultiplier * std::abs(pointerDiff) / viewportLength;
            if (pointerDiff > 0.05 * viewportLength) {
                direction = 1;
            } else if (pointerDiff < -0.05 * viewportLength) {
                direction = -1;
            }
        } else if (bboxMax > viewportLength) {
            direction = 1;
            multiplier = edgePanMaxMultiplier * std::min(bboxLength, bboxMax - viewportLength) / bboxLength;
        } else if (bboxMin < 0) {
            direction = -1;
            multiplier = edgePanMaxMultiplier * std::min(bboxLength, -bboxMin) / bboxLength;
        }
        if (!direction || layoutLength <= viewportLength) {
            return;
        }

        constexpr double timerRate = 30.0;
        const auto requestedScroll =
                zoom * direction * (viewportLength * edgePanSpeed / (100.0 * timerRate) * multiplier);
        const auto nextOffset = std::clamp(offset - requestedScroll, viewportLength - layoutLength, 0.0);
        offset = nextOffset;
    };

    const auto oldOffsetX = offsetX;
    const auto oldOffsetY = offsetY;
    scrollAmount(viewportWidth, screenX, screenWidth, x, layoutWidth() * zoom, offsetX);
    scrollAmount(viewportHeight, screenY, screenHeight, y, layoutHeight() * zoom, offsetY);
    if (offsetX == oldOffsetX && offsetY == oldOffsetY) {
        return false;
    }

    // Re-evaluate at the stationary pointer so the preview follows the content while the viewport scrolls.
    moveSelectionPointer(x, y, altHeld);
    return true;
}

void CanvasState::finishSelection() {
    if (!selectionGesture) {
        return;
    }
    auto g = std::move(*selectionGesture);
    selectionGesture.reset();
    if (g.moving) {
        Edit edit;
        if (!g.transform.identity() || g.targetPage != g.page) {
            std::vector<Ink> transferred;
            for (auto index: g.indices) {
                auto moved = ink.at(index);
                for (auto& point: moved.points) {
                    point = g.transform.apply(point);
                }
                moved.brush.width *= g.transform.widthScale;
                if (!std::isfinite(moved.brush.width) || moved.brush.width <= 0 ||
                    !std::all_of(moved.points.begin(), moved.points.end(),
                                 [](const Point& p) { return std::isfinite(p.x) && std::isfinite(p.y); }))
                    return;
                moved.page = g.targetPage;
                if (g.targetPage == g.page) {
                    edit.changes.push_back({index, {ink[index]}, index, {std::move(moved)}});
                } else {
                    moved.layerId = activeLayerId(g.targetPage);
                    moved.originLayer = activeLayerOrigin(g.targetPage);
                    moved.layerOrder = nextOriginOrder(g.targetPage, moved.originLayer) + transferred.size();
                    const auto removed = transferred.size();
                    edit.changes.push_back({index, {ink[index]}, index - removed, {}});
                    transferred.push_back(std::move(moved));
                }
            }
            if (!transferred.empty())
                edit.changes.push_back({ink.size(), {}, ink.size() - transferred.size(), std::move(transferred)});

            std::vector<TextItem> transferredTexts;
            for (auto index: g.textIndices) {
                auto moved = transformText(texts.at(index), g.transform);
                if (!validTextPage(moved)) {
                    return;
                }
                moved.page = g.targetPage;
                if (g.targetPage == g.page) {
                    edit.textChanges.push_back({index, {texts[index]}, index, {std::move(moved)}});
                } else {
                    moved.layerId = activeLayerId(g.targetPage);
                    moved.originLayer = activeLayerOrigin(g.targetPage);
                    moved.layerOrder = nextOriginOrder(g.targetPage, moved.originLayer) + transferredTexts.size();
                    const auto removed = transferredTexts.size();
                    edit.textChanges.push_back({index, {texts[index]}, index - removed, {}});
                    transferredTexts.push_back(std::move(moved));
                }
            }
            if (!transferredTexts.empty()) {
                edit.textChanges.push_back(
                        {texts.size(), {}, texts.size() - transferredTexts.size(), std::move(transferredTexts)});
            }

            std::vector<CanvasImage> transferredImages;
            for (auto index: g.imageIndices) {
                auto moved = transformImage(images.at(index), g.transform);
                moved.page = g.targetPage;
                if (!validCanvasImage(moved) || moved.page < 0 || moved.page >= pageCount()) {
                    return;
                }
                if (g.targetPage == g.page) {
                    edit.imageChanges.push_back({index, {images[index]}, index, {std::move(moved)}});
                } else {
                    moved.layerId = activeLayerId(g.targetPage);
                    moved.originLayer = activeLayerOrigin(g.targetPage);
                    moved.layerOrder = nextOriginOrder(g.targetPage, moved.originLayer) + transferredImages.size();
                    const auto removed = transferredImages.size();
                    edit.imageChanges.push_back({index, {images[index]}, index - removed, {}});
                    transferredImages.push_back(std::move(moved));
                }
            }
            if (!transferredImages.empty()) {
                edit.imageChanges.push_back(
                        {images.size(), {}, images.size() - transferredImages.size(), std::move(transferredImages)});
            }
        }
        if (!edit.changes.empty() || !edit.textChanges.empty() || !edit.imageChanges.empty()) {
            edit.selectionBefore = edit.selectionAfter = selected;
            edit.textSelectionBefore = edit.textSelectionAfter = selectedTexts;
            edit.imageSelectionBefore = edit.imageSelectionAfter = selectedImages;
            if (g.targetPage != g.page) {
                edit.selectionAfter = std::vector<size_t>{};
                for (size_t n = ink.size() - selected.size(); n < ink.size(); ++n) edit.selectionAfter->push_back(n);
                edit.textSelectionAfter = std::vector<size_t>{};
                for (size_t n = texts.size() - selectedTexts.size(); n < texts.size(); ++n) {
                    edit.textSelectionAfter->push_back(n);
                }
                edit.imageSelectionAfter = std::vector<size_t>{};
                for (size_t n = images.size() - selectedImages.size(); n < images.size(); ++n) {
                    edit.imageSelectionAfter->push_back(n);
                }
            }
            if (selectionFrame || g.handle == 8 || std::abs(g.baseRotation) > 1e-12) {
                SelectionFrameState nextFrame;
                nextFrame.bounds = g.bounds;
                nextFrame.bounds.page = g.targetPage;
                nextFrame.transform = composeSelectionTransforms(g.transform, g.baseFrameTransform);
                nextFrame.rotation = g.handle == 8 ? g.rotationTarget : g.baseRotation;
                nextFrame.inkIndices = edit.selectionAfter.value_or(selected);
                nextFrame.textIndices = edit.textSelectionAfter.value_or(selectedTexts);
                nextFrame.imageIndices = edit.imageSelectionAfter.value_or(selectedImages);
                edit.hasSelectionFrameState = true;
                edit.selectionFrameBefore = selectionFrame;
                edit.selectionFrameAfter = std::move(nextFrame);
            }
            apply(edit, true);
            record(std::move(edit));
            selectActivePage(g.targetPage);
        }
    } else {
        selectionFrame.reset();
        const auto size = pageSize(g.page);
        const auto x0 = std::min(g.start.x, g.latest.x), x1 = std::max(g.start.x, g.latest.x);
        const auto y0 = std::min(g.start.y, g.latest.y), y1 = std::max(g.start.y, g.latest.y);
        if (g.additive)
            selected = g.previousSelection;
        if (g.additive)
            selectedTexts = g.previousTextSelection;
        if (g.lasso)
            extendLassoAtPageEdges(g.boundary, size);
        // RectangularSelector extends at page edges (1 pt). Stroke selection
        // tests every point, rather than accepting a crossing segment.
        const auto collectLayer = [&](size_t layerId) {
            std::vector<size_t> layerSelection;
            for (size_t i = 0; i < ink.size(); ++i) {
                if (ink[i].page == g.page && ink[i].layerId == layerId && layerVisible(g.page, ink[i].layerId) &&
                    !ink[i].points.empty() &&
                    std::all_of(ink[i].points.begin(), ink[i].points.end(), [&](const Point& p) {
                        if (g.lasso)
                            return polygonContains(g.boundary, p);
                        return (x0 <= 1 || p.x >= x0) && (x1 >= size.width - 1 || p.x <= x1) &&
                               (y0 <= 1 || p.y >= y0) && (y1 >= size.height - 1 || p.y <= y1);
                    })) {
                    layerSelection.push_back(i);
                }
            }
            selected.insert(selected.end(), layerSelection.begin(), layerSelection.end());

            std::vector<size_t> textLayerSelection;
            for (size_t i = 0; i < texts.size(); ++i) {
                const auto& text = texts[i];
                if (text.page != g.page || text.layerId != layerId || text.text.empty() ||
                    !layerVisible(g.page, text.layerId)) {
                    continue;
                }
                const auto corners = textCorners(text);
                const bool contained = std::all_of(corners.begin(), corners.end(), [&](const Point& point) {
                    if (g.lasso) {
                        return polygonContains(g.boundary, point);
                    }
                    return (x0 <= 1 || point.x >= x0) && (x1 >= size.width - 1 || point.x <= x1) &&
                           (y0 <= 1 || point.y >= y0) && (y1 >= size.height - 1 || point.y <= y1);
                });
                if (contained) {
                    textLayerSelection.push_back(i);
                }
            }
            selectedTexts.insert(selectedTexts.end(), textLayerSelection.begin(), textLayerSelection.end());

            std::vector<size_t> imageLayerSelection;
            for (size_t i = 0; i < images.size(); ++i) {
                const auto& image = images[i];
                if (image.page != g.page || image.layerId != layerId || !layerVisible(g.page, image.layerId) ||
                    !validCanvasImage(image)) {
                    continue;
                }
                const auto corners = canvasImageCorners(image);
                const bool contained = std::all_of(corners.begin(), corners.end(), [&](const Point& point) {
                    if (g.lasso) {
                        return polygonContains(g.boundary, point);
                    }
                    return (x0 <= 1 || point.x >= x0) && (x1 >= size.width - 1 || point.x <= x1) &&
                           (y0 <= 1 || point.y >= y0) && (y1 >= size.height - 1 || point.y <= y1);
                });
                if (contained) {
                    imageLayerSelection.push_back(i);
                }
            }
            selectedImages.insert(selectedImages.end(), imageLayerSelection.begin(), imageLayerSelection.end());
            return !layerSelection.empty() || !textLayerSelection.empty() || !imageLayerSelection.empty();
        };
        if (g.multiLayer) {
            const auto& layers = layersForPage(g.page).layers;
            for (size_t layer = layers.size(); layer > 0; --layer) {
                if (layers[layer - 1].visible && collectLayer(layer)) {
                    break;
                }
            }
        } else {
            collectLayer(activeLayerId(g.page));
        }
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        std::sort(selectedTexts.begin(), selectedTexts.end());
        selectedTexts.erase(std::unique(selectedTexts.begin(), selectedTexts.end()), selectedTexts.end());
        std::sort(selectedImages.begin(), selectedImages.end());
        selectedImages.erase(std::unique(selectedImages.begin(), selectedImages.end()), selectedImages.end());
    }
}

void CanvasState::selectAllOnPage() {
    clearSelection();
    for (size_t i = 0; i < ink.size(); ++i) {
        if (ink[i].page == page && ink[i].layerId == activeLayerId(page) && layerVisible(page, ink[i].layerId) &&
            !ink[i].points.empty()) {
            selected.push_back(i);
        }
    }
    for (size_t i = 0; i < texts.size(); ++i) {
        if (texts[i].page == page && texts[i].layerId == activeLayerId(page) && layerVisible(page, texts[i].layerId) &&
            !texts[i].text.empty()) {
            selectedTexts.push_back(i);
        }
    }
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].page == page && images[i].layerId == activeLayerId(page) &&
            layerVisible(page, images[i].layerId) && validCanvasImage(images[i])) {
            selectedImages.push_back(i);
        }
    }
}

void CanvasState::deleteSelection() {
    cancelSelection();
    if (!hasSelection()) {
        return;
    }
    Edit edit;
    edit.selectionBefore = selected;
    edit.selectionAfter = std::vector<size_t>{};
    edit.textSelectionBefore = selectedTexts;
    edit.textSelectionAfter = std::vector<size_t>{};
    edit.imageSelectionBefore = selectedImages;
    edit.imageSelectionAfter = std::vector<size_t>{};
    edit.hasSelectionFrameState = true;
    edit.selectionFrameBefore = selectionFrame;
    for (size_t n = 0; n < selected.size(); ++n) {
        const auto i = selected[n];
        edit.changes.push_back({i, {ink.at(i)}, i - n, {}});
    }
    for (size_t n = 0; n < selectedTexts.size(); ++n) {
        const auto i = selectedTexts[n];
        edit.textChanges.push_back({i, {texts.at(i)}, i - n, {}});
    }
    for (size_t n = 0; n < selectedImages.size(); ++n) {
        const auto i = selectedImages[n];
        edit.imageChanges.push_back({i, {images.at(i)}, i - n, {}});
    }
    apply(edit, true);
    record(std::move(edit));
}

bool CanvasState::polygonContains(const std::vector<Point>& polygon, Point p) {
    if (polygon.size() < 3)
        return false;
    int winding = 0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        const long double cross = (static_cast<long double>(b.x) - a.x) * (static_cast<long double>(p.y) - a.y) -
                                  (static_cast<long double>(p.x) - a.x) * (static_cast<long double>(b.y) - a.y);
        if (cross == 0 && p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) &&
            p.y <= std::max(a.y, b.y))
            return true;
        if (a.y <= p.y && b.y > p.y && cross > 0)
            ++winding;
        if (a.y > p.y && b.y <= p.y && cross < 0)
            --winding;
    }
    return winding != 0;
}

}  // namespace prototype
