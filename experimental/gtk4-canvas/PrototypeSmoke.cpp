#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::capture(const std::string& name, GtkWidget* target, bool captureTargetDirectly) {
    waitForNextWindowFrame();
    graphene_rect_t targetBounds;
    if (target && (captureTargetDirectly || !gtk_widget_compute_bounds(target, window, &targetBounds))) {
        const int targetWidth = gtk_widget_get_width(target);
        const int targetHeight = gtk_widget_get_height(target);
        if (targetWidth <= 0 || targetHeight <= 0) {
            throw SnapshotPending{};
        }
        auto* targetPaintable = gtk_widget_paintable_new(target);
        auto* targetSnapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(GDK_PAINTABLE(targetPaintable), GDK_SNAPSHOT(targetSnapshot), targetWidth, targetHeight);
        auto* targetNode = gtk_snapshot_free_to_node(targetSnapshot);
        g_object_unref(targetPaintable);
        if (!targetNode) {
            throw SnapshotPending{};
        }
        const graphene_rect_t targetRenderBounds{{0, 0},
                                                 {static_cast<float>(targetWidth), static_cast<float>(targetHeight)}};
        auto* targetTexture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), targetNode,
                                                          &targetRenderBounds);
        const auto targetPath = options.smokeDirectory + "/" + name + ".png";
        const bool targetSaved = gdk_texture_save_to_png(targetTexture, targetPath.c_str());
        g_object_unref(targetTexture);
        gsk_render_node_unref(targetNode);
        if (!targetSaved) {
            throw std::runtime_error("Cannot write screenshot");
        }
        return;
    }
    // Snapshot the window so a crop retains ancestor backgrounds and styling.
    auto* paintable = GDK_PAINTABLE(g_object_ref(windowPaintable));
    auto* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(window),
                           gtk_widget_get_height(window));
    // A window paintable may lag a just-queued drawing-area frame. Overlay a
    // fresh canvas snapshot so smoke captures include the state being tested.
    const bool overlayDialogVisible =
            messageDialog || (preferencesUi.dialog && gtk_widget_get_visible(GTK_WIDGET(preferencesUi.dialog)));
    if (!target && canvasOverlay && !overlayDialogVisible) {
        graphene_rect_t canvasBounds;
        if (gtk_widget_compute_bounds(canvasOverlay, window, &canvasBounds)) {
            // Refresh the complete canvas stack. Snapshotting only the drawing
            // area here paints over GtkOverlay children such as the inline
            // text editor, hiding the very UI the smoke is meant to capture.
            auto* canvasOverlayPaintable = gtk_widget_paintable_new(canvasOverlay);
            const graphene_point_t offset{canvasBounds.origin.x, canvasBounds.origin.y};
            gtk_snapshot_save(snapshot);
            gtk_snapshot_translate(snapshot, &offset);
            gdk_paintable_snapshot(GDK_PAINTABLE(canvasOverlayPaintable), GDK_SNAPSHOT(snapshot),
                                   gtk_widget_get_width(canvasOverlay), gtk_widget_get_height(canvasOverlay));
            gtk_snapshot_restore(snapshot);
            g_object_unref(canvasOverlayPaintable);
        }
    }
    // Wayland GtkPopover renders into a child popup surface; composite it when open.
    if (!target && palettePopover && gtk_widget_get_visible(palettePopover) && paletteButton) {
        const int popW = gtk_widget_get_width(palettePopover);
        const int popH = gtk_widget_get_height(palettePopover);
        graphene_rect_t anchorBounds;
        if (popW > 0 && popH > 0 && gtk_widget_compute_bounds(paletteButton, window, &anchorBounds)) {
            auto* popPaintable = gtk_widget_paintable_new(palettePopover);
            const float px = std::clamp(anchorBounds.origin.x + 0.5f * (anchorBounds.size.width - popW), 4.0f,
                                        std::max(4.0f, static_cast<float>(gtk_widget_get_width(window) - popW - 4)));
            const float py = anchorBounds.origin.y + anchorBounds.size.height + 2.0f;
            const graphene_point_t offset{px, py};
            gtk_snapshot_save(snapshot);
            gtk_snapshot_translate(snapshot, &offset);
            gdk_paintable_snapshot(popPaintable, GDK_SNAPSHOT(snapshot), popW, popH);
            gtk_snapshot_restore(snapshot);
            g_object_unref(popPaintable);
        }
    }
    if (!target && pluginsMenuButton) {
        auto* popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(pluginsMenuButton));
        const int popW = popover ? gtk_widget_get_width(GTK_WIDGET(popover)) : 0;
        const int popH = popover ? gtk_widget_get_height(GTK_WIDGET(popover)) : 0;
        graphene_rect_t anchorBounds;
        if (popover && gtk_widget_get_visible(GTK_WIDGET(popover)) && popW > 0 && popH > 0 &&
            gtk_widget_compute_bounds(pluginsMenuButton, window, &anchorBounds)) {
            auto* popPaintable = gtk_widget_paintable_new(GTK_WIDGET(popover));
            const float px = std::clamp(anchorBounds.origin.x + 0.5f * (anchorBounds.size.width - popW), 4.0f,
                                        std::max(4.0f, static_cast<float>(gtk_widget_get_width(window) - popW - 4)));
            const float py = anchorBounds.origin.y + anchorBounds.size.height + 2.0f;
            const graphene_point_t offset{px, py};
            gtk_snapshot_save(snapshot);
            gtk_snapshot_translate(snapshot, &offset);
            gdk_paintable_snapshot(GDK_PAINTABLE(popPaintable), GDK_SNAPSHOT(snapshot), popW, popH);
            gtk_snapshot_restore(snapshot);
            g_object_unref(popPaintable);
        }
    }
    if (!target && pdfActionsPopover && gtk_widget_get_visible(GTK_WIDGET(pdfActionsPopover))) {
        const int popW = gtk_widget_get_width(GTK_WIDGET(pdfActionsPopover));
        const int popH = gtk_widget_get_height(GTK_WIDGET(pdfActionsPopover));
        GdkRectangle pointing{};
        graphene_rect_t overlayBounds;
        if (popW <= 0 || popH <= 0 || !gtk_popover_get_pointing_to(pdfActionsPopover, &pointing) ||
            !gtk_widget_compute_bounds(canvasOverlay, window, &overlayBounds)) {
            throw SnapshotPending{};
        }
        auto* popPaintable = gtk_widget_paintable_new(GTK_WIDGET(pdfActionsPopover));
        const float px = overlayBounds.origin.x + pointing.x - 0.5f * popW;
        const float py = overlayBounds.origin.y + pointing.y -
                         (gtk_popover_get_position(pdfActionsPopover) == GTK_POS_TOP ? popH : 0);
        const graphene_point_t offset{px, py};
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &offset);
        gdk_paintable_snapshot(GDK_PAINTABLE(popPaintable), GDK_SNAPSHOT(snapshot), popW, popH);
        gtk_snapshot_restore(snapshot);
        g_object_unref(popPaintable);
    }
    auto* node = gtk_snapshot_free_to_node(snapshot);
    if (!node) {
        g_object_unref(paintable);
        throw SnapshotPending{};
    }
    graphene_rect_t bounds;
    const bool crop = target && gtk_widget_compute_bounds(target, window, &bounds);
    auto* texture =
            gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node, crop ? &bounds : nullptr);
    const auto path = options.smokeDirectory + "/" + name + ".png";
    const bool saved = gdk_texture_save_to_png(texture, path.c_str());
    g_object_unref(texture);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    if (!saved) {
        throw std::runtime_error("Cannot write screenshot");
    }
}

void Prototype::waitForNextPaint() {
    const int previousPaints = paints;
    gtk_widget_queue_draw(canvas);
    const gint64 deadline = g_get_monotonic_time() + 500000;
    while (paints == previousPaints && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(paints > previousPaints, "Canvas did not paint the requested demo state");
}

void Prototype::waitForNextWindowFrame() {
    auto* frameClock = gtk_widget_get_frame_clock(window);
    require(frameClock != nullptr, "Window does not have a frame clock");
    const auto previousFrame = gdk_frame_clock_get_frame_counter(frameClock);
    gtk_widget_queue_draw(window);
    const gint64 deadline = g_get_monotonic_time() + 500000;
    while (gdk_frame_clock_get_frame_counter(frameClock) <= previousFrame && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    require(gdk_frame_clock_get_frame_counter(frameClock) > previousFrame,
            "Window did not render the requested screenshot frame");
}

void Prototype::require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void Prototype::checkInputParity(double x, double y) {
    const auto count = state.ink.size();
    begin(Device::Stylus, x, y + 100, 0.2);
    std::array<GdkTimeCoord, 2> history{};
    for (size_t i = 0; i < history.size(); ++i) {
        history[i].flags = GdkAxisFlags(GDK_AXIS_FLAG_X | GDK_AXIS_FLAG_Y);
        history[i].axes[GDK_AXIS_X] = x + 20 + 20 * i;
        history[i].axes[GDK_AXIS_Y] = y + 100;
    }
    history[0].flags = GdkAxisFlags(history[0].flags | GDK_AXIS_FLAG_PRESSURE);
    history[0].axes[GDK_AXIS_PRESSURE] = 0.6;
    consumeBacklog(history);
    g_signal_emit_by_name(stylus, "motion", x + 60, y + 100);
    g_signal_emit_by_name(stylus, "up", x + 1000, y + 1000);
    require(state.ink.size() == count + 1 && state.ink.back().points.size() == 4 &&
                    state.ink.back().points.back().pressure == 0.6,
            "Stylus backlog/release lost samples or added a hover tail");
    state.undo();

    begin(Device::Stylus, x + 50, y + 15, 0.5, true);
    g_signal_emit_by_name(stylus, "up", x + 50, y + 15);
    require(state.ink.size() == count + 1 && state.brush.tool == Tool::Pen,
            "Stylus eraser end failed to split stroke or changed toolbar selection");
    state.undo();

    state.eraserMode = prototype::EraserMode::Whiteout;
    begin(Device::Stylus, x + 20, y + 15, 0.5, true);
    g_signal_emit_by_name(stylus, "motion", x + 60, y + 15);
    g_signal_emit_by_name(stylus, "up", x + 60, y + 15);
    require(state.ink.size() == count + 1 && state.ink.back().brush.tool == Tool::Eraser &&
                    state.brush.tool == Tool::Pen,
            "Stylus eraser end in whiteout mode failed or changed toolbar selection");
    state.undo();

    state.eraserMode = prototype::EraserMode::DeleteStroke;
    begin(Device::Stylus, x, y, 0.5, true);
    g_signal_emit_by_name(stylus, "up", x, y);
    require(state.ink.size() == count - 1 && state.brush.tool == Tool::Pen,
            "Stylus eraser end in delete-stroke mode failed or changed toolbar selection");
    state.undo();
    state.eraserMode = prototype::EraserMode::Standard;

    const auto oldX = state.offsetX;
    const auto oldY = state.offsetY;
    gboolean handled = false;
    g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_space, 0u, GdkModifierType(0), &handled);
    require(handled && input.spaceHeld, "Space key did not enable temporary hand tool");
    g_signal_emit_by_name(mouse, "drag-begin", x, y);
    g_signal_emit_by_name(mouse, "drag-update", 10.0, 5.0);
    g_signal_emit_by_name(keys, "key-released", GDK_KEY_space, 0u, GdkModifierType(0));
    g_signal_emit_by_name(mouse, "drag-update", 20.0, 10.0);
    g_signal_emit_by_name(mouse, "drag-end", 400.0, 300.0);
    require(!input.active() && !input.spaceHeld && state.ink.size() == count &&
                    std::abs(state.offsetX - oldX - 20) < 0.001 && std::abs(state.offsetY - oldY - 10) < 0.001,
            "Space drag drew ink, jumped on release, or left the hand tool stuck");
    state.offsetX = oldX;
    state.offsetY = oldY;
    g_signal_emit_by_name(mouse, "drag-begin", 2.0, 2.0);
    require(input.panning(), "A drag in the page margin must use the hand tool");
    g_signal_emit_by_name(mouse, "drag-update", 8.0, 6.0);
    g_signal_emit_by_name(mouse, "drag-end", 100.0, 100.0);
    require(state.ink.size() == count && std::abs(state.offsetX - oldX - 8) < 0.001,
            "Margin drag unexpectedly drew or jumped on release");
    state.offsetX = oldX;
    state.offsetY = oldY;
    update();
}

gboolean Prototype::smoke() {
    if (busy || deferredAction) {
        require(++documentWaits < 120, "Document operation did not finish");
        return G_SOURCE_CONTINUE;
    }
    documentWaits = 0;
    if (g_get_monotonic_time() - lastDocumentCompletion < 250000) {
        return G_SOURCE_CONTINUE;  // Let disabled-control/dialog transitions settle before snapshots.
    }
    if (paints <= lastSmokePaint) {
        if (++paintWaits < 60) {
            return G_SOURCE_CONTINUE;
        }
        throw std::runtime_error("Window did not finish painting the requested state");
    }
    paintWaits = 0;
    if (preferencesVerifyMode.empty() && imageVerifyMode.empty()) {
        const bool currentRasterReady = canvasRenderer.matches(
                canvasRenderRequest(state.page, canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))));
        if (!frames || !currentRasterReady) {
            require(++smokeWaits < 60, "Canvas has not rendered");
            return G_SOURCE_CONTINUE;
        }
    }
    if (!preferencesVerifyMode.empty()) {
        return smokePreferencesVerify();
    }
    if (options.verifyTextClipboard) {
        verifyTextClipboard();
        g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
        return G_SOURCE_REMOVE;
    }
    if (!settingsVerifyMode.empty()) {
        return smokeSettingsVerify();
    }
    if (!layerVerifyMode.empty()) {
        return smokeLayers();
    }
    if (!selectionVerifyMode.empty()) {
        return smokeSelectionModes();
    }
    if (!pdfTextSelectionVerifyMode.empty()) {
        return smokePdfTextSelection();
    }
    if (!linkVerifyMode.empty()) {
        return smokeLinks();
    }
    if (!laserVerifyMode.empty()) {
        return smokeLaserPointers();
    }
    if (!playObjectVerifyMode.empty()) {
        return smokePlayObject();
    }
    if (!imageVerifyMode.empty()) {
        return smokeImages();
    }
    if (documentSmokeStep >= 0) {
        return smokeDocuments();
    }
    const auto x = state.offsetX + 80 * state.zoom;
    const auto y = state.offsetY + 80 * state.zoom;
    bool finishSmoke = false;
    switch (smokeStep++) {
        case 0: {
            auto* native = gtk_widget_get_native(canvas);
            const graphene_point_t origin{0, 0};
            graphene_point_t canvasOrigin;
            require(gtk_widget_compute_point(canvas, GTK_WIDGET(native), &origin, &canvasOrigin),
                    "Cannot locate canvas in native window");
            double tx, ty;
            gtk_native_get_surface_transform(native, &tx, &ty);
            const auto converted = surfacePoint(canvasOrigin.x + 40 - tx, canvasOrigin.y + 60 - ty);
            require(converted && std::abs(converted->x - 40) < 0.01 && std::abs(converted->y - 60) < 0.01,
                    "Pointer anchor failed to account for sidebar and toolbar offsets");
            auto initiallyVisiblePages =
                    state.visiblePageIndices(gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
            if (initiallyVisiblePages.empty()) {
                initiallyVisiblePages.push_back(state.page);
            }
            for (const int page: initiallyVisiblePages) {
                require(waitForCanvasRaster(page, canvasRenderScale(page, gtk_widget_get_scale_factor(canvas))),
                        "Initial visible PDF raster did not finish before ink smoke checks");
            }
            capture("initial");
            baselineFullRasters = canvasRenderer.completedFullRequests();
            require(std::abs(state.brush.width - 1.41) < 1e-6 &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 2u,
                    "Initial Pen preset did not match Medium 1.41pt");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 4u);
            require(std::abs(state.brush.width - 5.67) < 1e-6, "Very Thick Pen preset did not apply 5.67pt");
            const GdkRGBA red{1, 0, 0, 1};
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &red);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(widthButton), 6);
            require(gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 5u,
                    "Custom width did not switch preset selector to Custom");
            g_signal_emit_by_name(mouse, "drag-begin", x, y);
            g_signal_emit_by_name(mouse, "drag-update", 100.0, 30.0);
            g_signal_emit_by_name(mouse, "drag-end", 160.0, 0.0);
            require(state.ink.size() == 1 && state.ink[0].brush.width == 6 && state.ink[0].brush.color[0] == 1,
                    "Color or width controls did not style the stroke");
            break;
        }
        case 1: {
            require(canvasRenderer.completedFullRequests() == baselineFullRasters,
                    "Ink unnecessarily rerasterized a full-resolution PDF page");
            capture("ink");
            gboolean handled = false;
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_z, 0u, GDK_CONTROL_MASK, &handled);
            require(handled && state.ink.empty() && gtk_widget_get_sensitive(redoButton), "Keyboard undo failed");
            g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_Z, 0u,
                                  GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK), &handled);
            require(handled && state.ink.size() == 1, "Keyboard redo failed");
            g_signal_emit_by_name(keys, "key-released", GDK_KEY_Shift_L, 0u, GdkModifierType(GDK_CONTROL_MASK));
            g_signal_emit_by_name(keys, "key-released", GDK_KEY_Control_L, 0u, GdkModifierType(0));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[1]), true);
            require(state.brush.tool == Tool::Highlighter && std::abs(state.brush.width - 8.50) < 1e-6 &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 2u,
                    "Highlighter did not restore its Medium 8.50pt preset");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
            require(std::abs(state.brush.width - 19.84) < 1e-6, "Highlighter Thick preset did not apply 19.84pt");
            g_signal_emit_by_name(mouse, "drag-begin", x, y + 50);
            g_signal_emit_by_name(mouse, "drag-update", 100.0, 0.0);
            g_signal_emit_by_name(mouse, "drag-end", 160.0, 0.0);
            require(state.ink.size() == 2 && state.ink[1].brush.tool == Tool::Highlighter,
                    "Highlighter controller did not commit ink");
            break;
        }
        case 2:
            require(canvasRenderer.completedFullRequests() == baselineFullRasters,
                    "Brush changes unnecessarily rerasterized a full-resolution PDF page");
            capture("tools");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            require(state.brush.width == 6 && state.brush.color[0] == 1 &&
                            gtk_drop_down_get_selected(GTK_DROP_DOWN(sizeDropdown)) == 5u,
                    "Pen custom settings were lost on tool switch");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[2]), true);
            require(!gtk_widget_get_visible(colorBox) && gtk_widget_get_visible(eraserModeDropdown) &&
                            gtk_widget_get_sensitive(widthButton) && std::abs(state.brush.width - 8.50) < 1e-6 &&
                            state.eraserMode == prototype::EraserMode::Standard,
                    "Eraser did not expose mode and size controls with Medium 8.50pt preset");
            g_signal_emit_by_name(mouse, "drag-begin", x + 50, y + 15);
            g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
            require(state.ink.size() == 3, "Standard eraser control did not split pen ink");
            break;
        case 3:
            capture("eraser-split");
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.ink.size() == 2, "Undo button did not restore split pen ink");
            g_signal_emit_by_name(redoButton, "clicked");
            require(state.ink.size() == 3, "Redo button did not split pen ink again");
            g_signal_emit_by_name(undoButton, "clicked");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 1u);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 3u);
            require(state.eraserMode == prototype::EraserMode::Whiteout && std::abs(state.brush.width - 12.0) < 1e-6,
                    "Eraser mode or Thick preset did not update state");
            g_signal_emit_by_name(mouse, "drag-begin", x + 35, y - 5);
            g_signal_emit_by_name(mouse, "drag-update", 30.0, 90.0);
            g_signal_emit_by_name(mouse, "drag-end", 30.0, 90.0);
            require(state.ink.size() == 3 && state.ink.back().brush.tool == Tool::Eraser,
                    "Whiteout eraser did not commit whiteout ink");
            break;
        case 4:
            capture("eraser-whiteout");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 2u);
            require(state.eraserMode == prototype::EraserMode::DeleteStroke, "Delete Stroke mode was not selected");
            g_signal_emit_by_name(mouse, "drag-begin", x + 65, y + 85);
            g_signal_emit_by_name(mouse, "drag-end", 0.0, 0.0);
            require(state.ink.size() == 2, "Delete Stroke eraser did not remove whiteout stroke");
            g_signal_emit_by_name(undoButton, "clicked");
            require(state.ink.size() == 3, "Undo did not restore deleted whiteout stroke");
            g_signal_emit_by_name(redoButton, "clicked");
            require(state.ink.size() == 2, "Redo did not remove whiteout stroke again");
            gtk_drop_down_set_selected(GTK_DROP_DOWN(eraserModeDropdown), 0u);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(sizeDropdown), 2u);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            g_signal_emit_by_name(mouse, "drag-begin", x, y);
            g_signal_emit_by_name(mouse, "cancel", nullptr);
            require(!state.current, "Gesture cancellation left ink active");
            checkInputParity(x, y);
            g_signal_emit_by_name(pinch, "begin", nullptr);
            g_signal_emit_by_name(pinch, "scale-changed", 1.5);
            g_signal_emit_by_name(pinch, "end", nullptr);
            require(!state.pinching && state.scrollBlockedUntil > g_get_monotonic_time(), "Pinch cleanup failed");
            break;
        case 5: {
            require(waitForCanvasRaster(state.page,
                                        canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))) &&
                            canvasRenderer.completedFullRequests() > baselineFullRasters,
                    "Zoom did not refresh PDF resolution");
            capture("zoom");
            zoomPreview = true;
            require(waitForCanvasRaster(state.page, 1), "The 1x canvas raster did not finish");
            const auto* cached1x = canvasRenderer.find(state.page);
            const auto width1x = cairo_image_surface_get_width(cached1x->background.get());
            require(waitForCanvasRaster(state.page, 2), "The 2x canvas raster did not finish");
            const auto* cached2x = canvasRenderer.find(state.page);
            require(cairo_image_surface_get_width(cached2x->background.get()) >= width1x * 2 - 1,
                    "2x cache must preserve logical page size and increase resolution");
            require(cairo_image_surface_get_width(cached2x->background.get()) <= 4096 &&
                            cairo_image_surface_get_height(cached2x->background.get()) <= 4096,
                    "Cache exceeds dimension limit");
            zoomPreview = false;
            update();
            require(waitForCanvasRaster(state.page, canvasRenderScale(state.page, gtk_widget_get_scale_factor(canvas))),
                    "The settled canvas raster did not finish");
            baselineFullRasters = canvasRenderer.completedFullRequests();
            smokeDrawingTools();
            if (pageCount > 1) {
                const double zoomBeforeNav = state.zoom;
                gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(gtk_list_view_get_model(pageList)), 1);
                require(state.page == 1 && std::abs(state.zoom - zoomBeforeNav) < 1e-9 && state.ink.size() == 2,
                        "Sidebar navigation lost session ink or reset zoom");
            }
            gtk_widget_set_halign(contentView, GTK_ALIGN_CENTER);
            gtk_widget_set_size_request(contentView, 520, -1);
            auto* workspace = gtk_widget_get_parent(GTK_WIDGET(splitView));
            auto* adaptive = workspace ? gtk_widget_get_parent(workspace) : nullptr;
            if (adaptive) {
                gtk_widget_set_halign(adaptive, GTK_ALIGN_CENTER);
                gtk_widget_set_size_request(adaptive, 520, -1);
            }
            fitted = false;
            update();
            break;
        }
        case 6:
            require(adw_overlay_split_view_get_collapsed(splitView), "Narrow sidebar did not collapse");
            require(gtk_widget_get_width(canvas) < 720, "Narrow viewport did not resize");
            capture("narrow", contentView);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebarButton), true);
            require(adw_overlay_split_view_get_show_sidebar(splitView), "Sidebar toggle did not open overlay");
            break;
        case 7:
            break;  // Let the overlay animation finish before capturing it.
        default: {
            smokeCacheBytes = totalCacheBytes();
            capture("sidebar", contentView);
            if (pageCount > 1) {
                gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(gtk_list_view_get_model(pageList)), 0);
                const auto centerPage =
                        state.hitPage(gtk_widget_get_width(canvas) / 2.0, gtk_widget_get_height(canvas) / 2.0);
                require(state.page == 0 && centerPage && centerPage->page == 0 &&
                                !adw_overlay_split_view_get_show_sidebar(splitView),
                        "Selecting a page did not scroll to it or dismiss the overlay");
            }
            require(state.ink.size() == 2, "Session ink did not survive sidebar navigation");
            if (nativeSession) {
                const auto pageSize = state.pageSize(0);
                std::optional<prototype::Point> blank;
                for (double y = pageSize.height - 36; y > pageSize.height * 0.55 && !blank; y -= 24) {
                    for (double x = pageSize.width - 150; x > pageSize.width * 0.45; x -= 32) {
                        if (!textAt(0, {x, y})) {
                            blank = prototype::Point{x, y};
                            break;
                        }
                    }
                }
                require(blank.has_value(), "Text smoke could not find an unused spot on the page");
                state.offsetY =
                        gtk_widget_get_height(canvas) / 2.0 - state.pageTop(0) * state.zoom - blank->y * state.zoom;
                update();
                selectTool(Tool::Text);
                require(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(textButton)) &&
                                std::none_of(toolButtons.begin(), toolButtons.end(),
                                             [](GtkWidget* button) {
                                                 return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
                                             }),
                        "Text tool did not exclusively show its selected state");
                gtk_spin_button_set_value(GTK_SPIN_BUTTON(textFontSize), 24.0);
                auto* textSettings = gtk_widget_get_settings(textEditor->widget());
                g_object_get(textSettings, "gtk-cursor-aspect-ratio", &smokeCursorAspectRatioBeforeTextEditor, nullptr);
                const auto createAt = state.pageScreenPoint(0, blank->x, blank->y);
                g_signal_emit_by_name(textClick, "released", 1, createAt.x, createAt.y);
                require(textEditor->active(), "Text tool click did not open an inline editor");
                int initialEditorHeight = 0;
                gtk_widget_get_size_request(textEditor->widget(), &smokeTextEditorInitialWidth, &initialEditorHeight);
                gdouble activeCursorAspectRatio = 0;
                g_object_get(textSettings, "gtk-cursor-aspect-ratio", &activeCursorAspectRatio, nullptr);
                require(smokeTextEditorInitialWidth >= 40 && smokeTextEditorInitialWidth <= 64 &&
                                initialEditorHeight >= 24 && activeCursorAspectRatio >= 0.5,
                        "A new text editor did not start compact with a visible caret");
                waitForNextWindowFrame();
                const int allocatedEditorWidth = gtk_widget_get_width(textEditor->widget());
                capture("text-editor-initial");
                if (allocatedEditorWidth < 32 || allocatedEditorWidth > 64) {
                    throw std::runtime_error("A new text editor allocated " + std::to_string(allocatedEditorWidth) +
                                             " px instead of its compact initial width");
                }
                auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
                gtk_text_buffer_set_text(buffer, "GTK rich π", -1);
                int liveEditorWidth = 0;
                int liveEditorHeight = 0;
                gtk_widget_get_size_request(textEditor->widget(), &liveEditorWidth, &liveEditorHeight);
                require(liveEditorWidth > smokeTextEditorInitialWidth && liveEditorHeight >= 24,
                        "The inline text box did not grow with its live text layout");
                GtkTextIter selectionStart;
                GtkTextIter selectionEnd;
                gtk_text_buffer_get_bounds(buffer, &selectionStart, &selectionEnd);
                gtk_text_buffer_select_range(buffer, &selectionStart, &selectionEnd);
                gboolean handled = false;
                g_signal_emit_by_name(keys, "key-pressed", GDK_KEY_b, 0u, GDK_CONTROL_MASK, &handled);
                require(handled, "Ctrl+B did not format selected text in the inline editor");
                const GdkRGBA textColor{208.0f / 255, 32.0f / 255, 16.0f / 255, 1};
                gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorButton), &textColor);
                require(std::abs(state.textColor[0] - 208.0 / 255) < 1e-7 &&
                                std::abs(state.textColor[1] - 32.0 / 255) < 1e-7 &&
                                std::abs(state.textColor[2] - 16.0 / 255) < 1e-7,
                        "The Text toolbar color control did not update the active text color");
                updateTextEditorPosition();
                update();
            } else {
                require(!gtk_widget_get_sensitive(textButton),
                        "Text tool should be unavailable without a document worker");
                finishSmoke = true;
            }
            break;
        }
        case 9: {
            require(textEditor->active(), "Inline text editor closed before its render capture");
            int requestedWidth = 0;
            int requestedHeight = 0;
            gtk_widget_get_size_request(textEditor->widget(), &requestedWidth, &requestedHeight);
            const auto layoutItem = textEditor->layoutItem();
            const auto [naturalWidth, naturalHeight] = prototype::textNaturalSize(layoutItem);
            const auto screen = state.pageScreenPoint(layoutItem.page, layoutItem.x, layoutItem.y);
            const double availableWidth = std::max(40.0, gtk_widget_get_width(canvas) - std::max(0.0, screen.x) - 8.0);
            const double contentWidth =
                    layoutItem.wrapWidth >= 0 ? layoutItem.wrapWidth * state.zoom : naturalWidth * state.zoom;
            const int expectedWidth = static_cast<int>(std::ceil(std::clamp(contentWidth + 8.0, 40.0, availableWidth)));
            const int expectedHeight = static_cast<int>(std::ceil(std::max(24.0, naturalHeight * state.zoom + 6.0)));
            gdouble activeCursorAspectRatio = 0;
            g_object_get(gtk_widget_get_settings(textEditor->widget()), "gtk-cursor-aspect-ratio",
                         &activeCursorAspectRatio, nullptr);
            require(requestedWidth == expectedWidth && requestedHeight == expectedHeight &&
                            requestedWidth > smokeTextEditorInitialWidth && activeCursorAspectRatio >= 0.5,
                    "Inline text editor geometry or active caret sizing does not match the live Pango layout");
            capture("text-editor");
            auto* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
            GtkTextIter caret;
            gtk_text_buffer_get_end_iter(buffer, &caret);
            gtk_text_buffer_place_cursor(buffer, &caret);
            gtk_widget_grab_focus(textEditor->widget());
            auto* cursorSettings = gtk_widget_get_settings(textEditor->widget());
            gboolean cursorBlinkBeforeCapture = true;
            g_object_get(cursorSettings, "gtk-cursor-blink", &cursorBlinkBeforeCapture, nullptr);
            g_object_set(cursorSettings, "gtk-cursor-blink", FALSE, nullptr);
            capture("text-editor-caret", textEditor->widget(), true);
            g_object_set(cursorSettings, "gtk-cursor-blink", cursorBlinkBeforeCapture, nullptr);
            const auto newTextIndex = state.texts.size();
            finishTextEditing();
            gdouble restoredCursorAspectRatio = 0;
            g_object_get(gtk_widget_get_settings(textEditor->widget()), "gtk-cursor-aspect-ratio",
                         &restoredCursorAspectRatio, nullptr);
            require(std::abs(restoredCursorAspectRatio - smokeCursorAspectRatioBeforeTextEditor) < 1e-6,
                    "Finishing inline text editing did not restore the system caret setting");
            require(state.texts.size() == newTextIndex + 1 && state.texts.back().text == "GTK rich π" &&
                            state.texts.back().fontSize == 24.0 && !state.texts.back().styleRuns.empty() &&
                            state.texts.back().styleRuns[0].color == 0xffd02010U &&
                            state.selectedTexts == std::vector<size_t>{newTextIndex} && state.canUndo(),
                    "Inline text creation did not preserve its formatting or select the new object for transforms");
            state.undo();
            require(state.texts.size() == newTextIndex, "Undo did not remove a newly created text item");
            state.redo();
            require(state.texts.size() == newTextIndex + 1 && state.selectedTexts == std::vector<size_t>{newTextIndex},
                    "Redo did not restore a newly created text item and its selection");
            const double rotation = 0.35;
            const double scale = 1.25;
            const double cosine = scale * std::cos(rotation);
            const double sine = scale * std::sin(rotation);
            require(state.updateSelectedTexts([=](prototype::TextItem& item) {
                        item.xx = cosine;
                        item.yx = sine;
                        item.xy = -sine;
                        item.yy = cosine;
                    }),
                    "Text smoke could not prepare a transformed text object");
            update();

            activateSelection(Tool::SmartSelect);
            auto created = state.texts.back();
            state.clearSelection();
            fitMode = true;
            const auto zoomBeforeSmartSelect = state.zoom;
            const double wordX = created.layoutWidth * 0.12;
            const double wordY = created.layoutHeight / 2;
            const auto editAt = state.pageScreenPoint(created.page, created.x + created.xx * wordX + created.xy * wordY,
                                                      created.y + created.yx * wordX + created.yy * wordY);
            g_signal_emit_by_name(textClick, "released", 1, editAt.x, editAt.y);
            require(!textEditor->active() && state.selectedTexts == std::vector<size_t>{newTextIndex} &&
                            state.hasSelection() && state.selectionBox().has_value(),
                    "Smart Select single click did not select native text for transforms");
            capture("smart-text-selected");
            g_signal_emit_by_name(textClick, "pressed", 2, editAt.x, editAt.y);
            g_signal_emit_by_name(textClick, "released", 2, editAt.x, editAt.y);
            require(textEditor->active() && std::abs(state.zoom - zoomBeforeSmartSelect) < 1e-9 && !fitMode,
                    "Smart Select double-click did not edit text at the existing canvas zoom");
            waitForNextWindowFrame();
            const graphene_point_t editorOrigin{0, 0};
            graphene_point_t canvasOrigin;
            require(gtk_widget_compute_point(textEditor->widget(), canvas, &editorOrigin, &canvasOrigin),
                    "Cannot map the transformed text editor origin to the canvas");
            const auto textOrigin = state.pageScreenPoint(created.page, created.x, created.y);
            // The editor has a one-pixel CSS border; GTK coordinates start inside that border. The widget is already
            // sized in screen pixels, so its local affine transform does not receive the page zoom a second time.
            const double expectedOriginX = textOrigin.x + created.xx + created.xy;
            const double expectedOriginY = textOrigin.y + created.yx + created.yy;
            const double originDeltaX = canvasOrigin.x - expectedOriginX;
            const double originDeltaY = canvasOrigin.y - expectedOriginY;
            if (std::abs(originDeltaX) >= 0.75 || std::abs(originDeltaY) >= 0.75) {
                throw std::runtime_error("The transformed text editor origin does not match the document text origin: dx=" +
                                         std::to_string(originDeltaX) + ", dy=" + std::to_string(originDeltaY));
            }
            const graphene_point_t editorSample{32, 18};
            graphene_point_t canvasSample;
            require(gtk_widget_compute_point(textEditor->widget(), canvas, &editorSample, &canvasSample),
                    "Cannot map a transformed text editor point to the canvas");
            const double expectedSampleX = expectedOriginX + created.xx * editorSample.x + created.xy * editorSample.y;
            const double expectedSampleY = expectedOriginY + created.yx * editorSample.x + created.yy * editorSample.y;
            const double sampleDeltaX = canvasSample.x - expectedSampleX;
            const double sampleDeltaY = canvasSample.y - expectedSampleY;
            if (std::abs(sampleDeltaX) >= 0.75 || std::abs(sampleDeltaY) >= 0.75) {
                throw std::runtime_error("The transformed text editor does not follow the text object's affine matrix: dx=" +
                                         std::to_string(sampleDeltaX) + ", dy=" + std::to_string(sampleDeltaY));
            }
            buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textEditor->widget()));
            GtkTextIter selectedStart;
            GtkTextIter selectedEnd;
            require(gtk_text_buffer_get_selection_bounds(buffer, &selectedStart, &selectedEnd) &&
                            !gtk_text_iter_equal(&selectedStart, &selectedEnd),
                    "Smart Select double-click did not select the clicked text word");
            gchar* selectedWord = gtk_text_buffer_get_text(buffer, &selectedStart, &selectedEnd, true);
            require(selectedWord && std::string(selectedWord) == "GTK",
                    "Smart Select double-click mapped the transformed pointer to the wrong text word");
            g_free(selectedWord);
            GtkTextIter textEnd;
            gtk_text_buffer_get_end_iter(buffer, &textEnd);
            gtk_text_buffer_place_cursor(buffer, &textEnd);
            capture("smart-text-editor-transformed");
            gtk_text_buffer_insert(buffer, &textEnd, "!", 1);
            finishTextEditing();
            require(state.texts.back().text == "GTK rich π!" && !state.texts.back().styleRuns.empty() &&
                            state.selectedTexts == std::vector<size_t>{newTextIndex} &&
                            std::abs(state.texts.back().xx - created.xx) < 1e-9 &&
                            std::abs(state.texts.back().yx - created.yx) < 1e-9 &&
                            std::abs(state.texts.back().xy - created.xy) < 1e-9 &&
                            std::abs(state.texts.back().yy - created.yy) < 1e-9,
                    "Editing transformed rich text lost its content, styles, or affine matrix");
            require(state.updateSelectedTexts([](prototype::TextItem& item) {
                        item.xx = 1;
                        item.yx = 0;
                        item.xy = 0;
                        item.yy = 1;
                    }),
                    "Text smoke could not restore the identity transform for later interactions");

            selectTool(Tool::Text);
            created = state.texts.back();
            fitMode = true;
            const auto zoomBeforeTextToolDoubleClick = state.zoom;
            const auto textToolEditAt = state.pageScreenPoint(created.page, created.x + created.layoutWidth * 0.12,
                                                              created.y + created.layoutHeight / 2);
            g_signal_emit_by_name(textClick, "released", 2, textToolEditAt.x, textToolEditAt.y);
            require(textEditor->active() && std::abs(state.zoom - zoomBeforeTextToolDoubleClick) < 1e-9,
                    "Text-tool double-click did not edit existing text at the existing canvas zoom");
            finishTextEditing();
            created = state.texts.back();
            const auto moveStart = state.pageScreenPoint(created.page, created.x + created.layoutWidth / 2,
                                                         created.y + created.layoutHeight / 2);
            g_signal_emit_by_name(textClick, "pressed", 1, moveStart.x, moveStart.y);
            g_signal_emit_by_name(mouse, "drag-begin", moveStart.x, moveStart.y);
            require(state.movingSelection() && state.selectedTexts == std::vector<size_t>{newTextIndex},
                    "Dragging existing text with the Text tool did not start its transform");
            g_signal_emit_by_name(mouse, "drag-update", 24.0 * state.zoom, 16.0 * state.zoom);
            g_signal_emit_by_name(mouse, "drag-end", 24.0 * state.zoom, 16.0 * state.zoom);
            g_signal_emit_by_name(textClick, "released", 1, moveStart.x, moveStart.y);
            require(!textEditor->active() && std::abs(state.texts.back().x - created.x - 24) < 1e-6 &&
                            std::abs(state.texts.back().y - created.y - 16) < 1e-6 &&
                            state.selectedTexts == std::vector<size_t>{newTextIndex},
                    "Text-tool drag did not move the object or incorrectly opened the inline editor");

            const auto beforeResize = state.texts.back();
            const auto resizeHandle = state.pageScreenPoint(beforeResize.page, state.selectionHandles()[4].x,
                                                            state.selectionHandles()[4].y);
            g_signal_emit_by_name(textClick, "pressed", 1, resizeHandle.x, resizeHandle.y);
            g_signal_emit_by_name(mouse, "drag-begin", resizeHandle.x, resizeHandle.y);
            require(state.movingSelection(), "A selected text corner did not start resizing in the Text tool");
            g_signal_emit_by_name(mouse, "drag-update", 20.0 * state.zoom, 20.0 * state.zoom);
            g_signal_emit_by_name(mouse, "drag-end", 20.0 * state.zoom, 20.0 * state.zoom);
            g_signal_emit_by_name(textClick, "released", 1, resizeHandle.x, resizeHandle.y);
            require(std::abs(state.texts.back().xx - beforeResize.xx) > 1e-5 &&
                            state.selectedTexts == std::vector<size_t>{newTextIndex},
                    "Text-tool corner drag did not resize text while retaining its selection");
            state.undo();
            require(std::abs(state.texts.back().xx - beforeResize.xx) < 1e-8 &&
                            state.selectedTexts == std::vector<size_t>{newTextIndex},
                    "Undo did not restore the text resize and its selection");
            update();
            break;
        }
        case 10:
            capture("text-edited");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toolButtons[0]), true);
            finishSmoke = true;
            break;
    }
    if (finishSmoke) {
        const auto p0 = state.pageScreenPoint(0, 80, 80);
        begin(Device::Mouse, p0.x, p0.y);
        gtk_widget_set_visible(window, false);
        require(!state.current && !state.pinching && !input.active() && !input.spaceHeld && state.ink.size() == 3,
                "Unmap dropped delivered ink or left temporary input active");
        std::cout << "PASS: GTK4 Cairo canvas, real PDF, colors, widths, highlight, undo/redo, eraser, "
                     "sidebar navigation/overlay, stylus/backlog/Space adapters, cancel, pinch, narrow "
                     "viewport, geometric drawing, shape recognition, stabilization, rich text, 2x raster cache"
                  << "\nframes=" << frames << " rasterizations=" << canvasRenderer.completedRequests()
                  << " device_scale=" << gtk_widget_get_scale_factor(canvas)
                  << " viewport_width=" << gtk_widget_get_width(canvas) << " cache_bytes=" << smokeCacheBytes << '\n';
        if (options.worker.empty()) {
            g_application_quit(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))));
            return G_SOURCE_REMOVE;
        }
        documentSmokeStep = 0;
        gtk_widget_set_halign(contentView, GTK_ALIGN_FILL);
        gtk_widget_set_size_request(contentView, -1, -1);
        gtk_window_present(GTK_WINDOW(window));
        fitted = false;
        update();
    }
    lastSmokePaint = paints;
    gtk_widget_queue_draw(window);
    return G_SOURCE_CONTINUE;
}

}  // namespace prototype::gtk4
