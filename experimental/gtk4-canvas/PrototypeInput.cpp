#include "Prototype.h"

namespace prototype::gtk4 {

void Prototype::connectInput() {
    mouse = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mouse), GDK_BUTTON_PRIMARY);
    g_signal_connect(mouse, "drag-begin", G_CALLBACK(+[](GtkGestureDrag* gesture, double x, double y, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (mouseEvent(GTK_EVENT_CONTROLLER(gesture)) &&
                             self->state.drawingMode != prototype::DrawingMode::Spline) {
                             self->dragX = x;
                             self->dragY = y;
                             auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                             self->begin(Device::Mouse, x, y, std::nullopt, false,
                                         event ? gdk_event_get_time(event) : 0);
                         }
                     }),
                     this);
    g_signal_connect(mouse, "drag-update",
                     G_CALLBACK(+[](GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (mouseEvent(GTK_EVENT_CONTROLLER(gesture)) &&
                             self->state.drawingMode != prototype::DrawingMode::Spline) {
                             auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                             self->motion(Device::Mouse, self->dragX + dx, self->dragY + dy, std::nullopt,
                                          event ? gdk_event_get_time(event) : 0);
                         }
                     }),
                     this);
    g_signal_connect(mouse, "drag-end", G_CALLBACK(+[](GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (mouseEvent(GTK_EVENT_CONTROLLER(gesture)) &&
                             self->state.drawingMode != prototype::DrawingMode::Spline) {
                             self->finish(Device::Mouse, std::nullopt,
                                          prototype::Point{self->dragX + dx, self->dragY + dy});
                             self->update();
                         }
                     }),
                     this);
    addGesture(mouse, Device::Mouse);

    textClick = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(textClick), GDK_BUTTON_PRIMARY);
    g_signal_connect(textClick, "pressed",
                     G_CALLBACK(+[](GtkGestureClick* gesture, int presses, double x, double y, gpointer data) {
                         if (!mouseEvent(GTK_EVENT_CONTROLLER(gesture)))
                             return;
                         auto* self = static_cast<Prototype*>(data);
                         self->textDragHandled = false;
                         if (presses == 2 && self->state.brush.tool == Tool::SmartSelect) {
                             const auto hit = self->state.hitPage(x, y);
                             if (hit) {
                                 const auto object = self->state.pickSelectableObject(hit->page, hit->local);
                                 if (object && object->kind == prototype::SelectableObjectKind::Text &&
                                     !self->state.texts[object->index].isLink) {
                                     self->startTextEditing(x, y);
                                     self->selectTextWordAt(x, y);
                                     self->update();
                                     return;
                                 }
                                 if (object && object->kind == prototype::SelectableObjectKind::Image &&
                                     self->state.images[object->index].kind == prototype::CanvasImageKind::Latex) {
                                     self->selectTool(Tool::Latex);
                                     self->openLatexEditorAt(x, y, true);
                                     self->update();
                                     return;
                                 }
                             }
                         }
                         if (self->state.drawingMode == prototype::DrawingMode::Spline &&
                             (self->state.brush.tool == Tool::Pen || self->state.brush.tool == Tool::Highlighter)) {
                             self->input.splinePress(Device::Mouse, {x, y, std::nullopt}, presses >= 2);
                             self->update();
                         }
                     }),
                     this);
    g_signal_connect(
            textClick, "released",
            G_CALLBACK(+[](GtkGestureClick* gesture, int presses, double x, double y, gpointer data) {
                if (!mouseEvent(GTK_EVENT_CONTROLLER(gesture)))
                    return;
                auto* self = static_cast<Prototype*>(data);
                if (self->state.drawingMode == prototype::DrawingMode::Spline &&
                    (self->state.brush.tool == Tool::Pen || self->state.brush.tool == Tool::Highlighter)) {
                    self->input.finish(Device::Mouse);
                    self->update();
                    return;
                }
                if (self->textDragHandled) {
                    self->textDragHandled = false;
                    return;
                }
                const auto tool = self->state.brush.tool;
                if (presses > 1 && tool != Tool::Link) {
                    const auto hit = self->state.hitPage(x, y);
                    if (hit) {
                        if (const auto index = self->state.pickLink(hit->page, hit->local)) {
                            self->openLinkEditor(hit->page, {self->state.texts[*index].x, self->state.texts[*index].y},
                                                 index);
                            self->update();
                            return;
                        }
                    }
                }
                if (tool == Tool::Link) {
                    const auto modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
                    self->handleLinkClick(x, y, presses, (modifiers & GDK_CONTROL_MASK) != 0);
                    self->update();
                    return;
                }
                if (prototype::isPdfTextSelectionTool(tool) && (presses == 2 || presses == 3)) {
                    self->selectPdfTextAtPoint(x, y,
                                               presses == 2 ? prototype::PdfTextSelectionStyle::Word :
                                                              prototype::PdfTextSelectionStyle::Line);
                    return;
                }
                if (tool == Tool::Latex) {
                    if (presses == 1) {
                        self->openLatexEditorAt(x, y);
                    }
                    return;
                }
                if (tool == Tool::Text && presses == 2) {
                    const auto hit = self->state.hitPage(x, y);
                    if (hit && self->textAt(hit->page, hit->local)) {
                        if (!self->textEditor->active()) {
                            self->startTextEditing(x, y);
                        }
                        self->selectTextWordAt(x, y);
                        self->update();
                        return;
                    }
                }
                if (presses != 1)
                    return;
                if (tool == Tool::Text) {
                    self->startTextEditing(x, y);
                } else if (tool == Tool::SmartSelect) {
                    if (self->textEditor->active()) {
                        self->finishTextEditing();
                    }
                    const auto hit = self->state.hitPage(x, y);
                    const auto modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
                    if (hit) {
                        self->state.selectObjectAt(hit->page, hit->local, (modifiers & GDK_SHIFT_MASK) != 0);
                    } else if ((modifiers & GDK_SHIFT_MASK) == 0) {
                        self->state.clearSelection();
                        self->clearPdfTextSelection(false);
                    }
                    self->update();
                }
            }),
            this);
    g_signal_connect(textClick, "cancel", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->state.drawingMode == prototype::DrawingMode::Spline && self->input.active()) {
                             self->input.finish(Device::Mouse);
                             self->update();
                         }
                     }),
                     this);
    addGesture(GTK_GESTURE(textClick));

    pointerMotion = gtk_event_controller_motion_new();
    g_signal_connect(pointerMotion, "motion",
                     G_CALLBACK(+[](GtkEventControllerMotion* controller, double x, double y, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->trackPointerPosition(x, y);
                         if (self->state.brush.tool == Tool::Link) {
                             self->updateLinkHover(x, y);
                             return;
                         }
                         if (self->state.drawingMode != prototype::DrawingMode::Spline ||
                             (self->state.brush.tool != Tool::Pen && self->state.brush.tool != Tool::Highlighter)) {
                             return;
                         }
                         if (self->input.active()) {
                             auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
                             self->input.motion(Device::Mouse,
                                                {x, y, std::nullopt, event ? gdk_event_get_time(event) : 0});
                         } else {
                             self->input.splineHover(x, y);
                         }
                         self->update();
                     }),
                     this);
    g_signal_connect(
            pointerMotion, "leave", G_CALLBACK(+[](GtkEventControllerMotion*, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                const bool hadPointer = self->pointerPosition.has_value();
                self->pointerPosition.reset();
                if (hadPointer && (self->importedConfig.settings.highlightPosition || self->canvasOverviewMode())) {
                    gtk_widget_queue_draw(self->canvas);
                }
                self->scheduleFloatingToolboxHide();
                if (self->state.brush.tool == Tool::Link) {
                    self->linkInfoIndex.reset();
                    if (self->linkInfoPopover) {
                        gtk_popover_popdown(GTK_POPOVER(self->linkInfoPopover));
                    }
                    gtk_widget_set_cursor_from_name(self->canvas, "pointer");
                }
            }),
            this);
    gtk_widget_add_controller(canvas, pointerMotion);

    stylus = gtk_gesture_stylus_new();
    gtk_gesture_stylus_set_stylus_only(GTK_GESTURE_STYLUS(stylus), true);
    g_signal_connect(stylus, "down", G_CALLBACK(+[](GtkGestureStylus* gesture, double x, double y, gpointer data) {
                         auto* tool = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture)) ?
                                              gtk_gesture_stylus_get_device_tool(gesture) :
                                              nullptr;
                         const bool eraser = tool && prototype::isEraserEnd(gdk_device_tool_get_tool_type(tool));
                         auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                         static_cast<Prototype*>(data)->begin(Device::Stylus, x, y, pressure(gesture), eraser,
                                                              event ? gdk_event_get_time(event) : 0);
                     }),
                     this);
    g_signal_connect(stylus, "motion", G_CALLBACK(+[](GtkGestureStylus* gesture, double x, double y, gpointer data) {
                         static_cast<Prototype*>(data)->stylusMotion(gesture, x, y);
                     }),
                     this);
    g_signal_connect(stylus, "up", G_CALLBACK(+[](GtkGestureStylus* gesture, double x, double y, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->finish(Device::Stylus, pressure(gesture), prototype::Point{x, y});
                         self->update();
                     }),
                     this);
    addGesture(stylus, Device::Stylus);

    stylusButtonClick = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(stylusButtonClick), 0);
    g_signal_connect(stylusButtonClick, "pressed",
                     G_CALLBACK(+[](GtkGestureClick* gesture, int, double x, double y, gpointer data) {
                         auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                         if (!event || !gdk_event_get_device_tool(event)) {
                             return;
                         }
                         const auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
                         auto* self = static_cast<Prototype*>(data);
                         const auto next = button == GDK_BUTTON_MIDDLE ? self->importedConfig.settings.stylusButtonOne :
                                           button == GDK_BUTTON_SECONDARY ?
                                                                         self->importedConfig.settings.stylusButtonTwo :
                                                                         prototype::ButtonToolAction::None;
                         if (button != GDK_BUTTON_MIDDLE && button != GDK_BUTTON_SECONDARY) {
                             return;
                         }
                         if (next == self->activeStylusAction && self->activeStylusButton == button) {
                             return;
                         }
                         const bool restart = self->input.active(Device::Stylus) &&
                                              (self->activeStylusAction != prototype::ButtonToolAction::None ||
                                               next != prototype::ButtonToolAction::None);
                         if (restart) {
                             self->input.finish(Device::Stylus);
                         }
                         self->activeStylusButton = button;
                         self->activeStylusAction = next;
                         if (next == prototype::ButtonToolAction::FloatingToolbox) {
                             self->showFloatingToolbox(x, y);
                         } else if (restart) {
                             self->begin(Device::Stylus, x, y);
                         } else {
                             self->update();
                         }
                     }),
                     this);
    g_signal_connect(stylusButtonClick, "released",
                     G_CALLBACK(+[](GtkGestureClick* gesture, int, double x, double y, gpointer data) {
                         auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                         if (!event || !gdk_event_get_device_tool(event)) {
                             return;
                         }
                         auto* self = static_cast<Prototype*>(data);
                         const auto button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
                         if (button != self->activeStylusButton) {
                             return;
                         }
                         const bool restart = self->activeStylusAction != prototype::ButtonToolAction::None &&
                                              self->input.active(Device::Stylus);
                         if (restart) {
                             self->input.finish(Device::Stylus);
                         }
                         self->activeStylusButton = 0;
                         self->activeStylusAction = prototype::ButtonToolAction::None;
                         if (restart) {
                             self->begin(Device::Stylus, x, y);
                         } else {
                             self->update();
                         }
                     }),
                     this);
    addGesture(GTK_GESTURE(stylusButtonClick));

    // Each pan sequence keeps its own incremental offsets. A pinch or pen
    // sequence permanently suppresses that pan until a new drag begins.
    for (bool touch: {false, true}) {
        auto* pan = gtk_gesture_drag_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(pan), touch ? GDK_BUTTON_PRIMARY : GDK_BUTTON_MIDDLE);
        gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(pan), touch);
        g_object_set_data_full(
                G_OBJECT(pan), "pan-state", new PanState, +[](gpointer data) { delete static_cast<PanState*>(data); });
        g_object_set_data(G_OBJECT(pan), "touch-pan", GINT_TO_POINTER(touch));
        g_signal_connect(pan, "drag-begin", G_CALLBACK(+[](GtkGestureDrag* gesture, double, double, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             auto* pan = static_cast<PanState*>(g_object_get_data(G_OBJECT(gesture), "pan-state"));
                             const bool touch = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(gesture), "touch-pan"));
                             pan->x = 0;
                             pan->y = 0;
                             self->cancelScrollMomentum();
                             pan->enabled =
                                     !self->presentationMode && (touch || mouseEvent(GTK_EVENT_CONTROLLER(gesture))) &&
                                     !self->input.active() && !self->state.blocksScroll(false, g_get_monotonic_time());
                         }),
                         this);
        g_signal_connect(pan, "drag-update",
                         G_CALLBACK(+[](GtkGestureDrag* gesture, double dx, double dy, gpointer data) {
                             auto* self = static_cast<Prototype*>(data);
                             auto* pan = static_cast<PanState*>(g_object_get_data(G_OBJECT(gesture), "pan-state"));
                             if (self->presentationMode || self->input.active() ||
                                 self->state.blocksScroll(false, g_get_monotonic_time())) {
                                 pan->enabled = false;
                             }
                             if (pan->enabled) {
                                 self->applyCanvasScroll(pan->x - dx, pan->y - dy, false);
                             }
                             pan->x = dx;
                             pan->y = dy;
                         }),
                         this);
        g_signal_connect(pan, "cancel", G_CALLBACK(+[](GtkGesture* gesture, GdkEventSequence*, gpointer) {
                             static_cast<PanState*>(g_object_get_data(G_OBJECT(gesture), "pan-state"))->enabled = false;
                         }),
                         nullptr);
        addGesture(pan);
        panGestures.push_back(pan);
    }

    pinch = gtk_gesture_zoom_new();
    g_signal_connect(pinch, "begin", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->smartSelectPdfGesture.reset();
                         self->cancelScrollMomentum();
                         self->input.cancel();
                         self->state.beginPinch();
                         self->pinchZoom = self->state.zoom;
                         self->pinchX = gtk_widget_get_width(self->canvas) / 2.0;
                         self->pinchY = gtk_widget_get_height(self->canvas) / 2.0;
                         if (gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(self->pinch))) {
                             gtk_gesture_get_bounding_box_center(self->pinch, &self->pinchX, &self->pinchY);
                         }
                     }),
                     this);
    g_signal_connect(pinch, "scale-changed", G_CALLBACK(+[](GtkGestureZoom*, double scale, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->zoom(self->pinchZoom * scale, self->pinchX, self->pinchY, true);
                     }),
                     this);
    g_signal_connect(pinch, "end", G_CALLBACK(+[](GtkGesture*, GdkEventSequence*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->state.endPinch(g_get_monotonic_time());
                         self->finishZoomPreview();
                         self->update();
                     }),
                     this);
    addGesture(pinch);

    scrollController = GTK_EVENT_CONTROLLER_SCROLL(gtk_event_controller_scroll_new(
            static_cast<GtkEventControllerScrollFlags>(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES |
                                                        GTK_EVENT_CONTROLLER_SCROLL_KINETIC)));
    auto* scroll = GTK_EVENT_CONTROLLER(scrollController);
    g_signal_connect(scroll, "scroll-begin", G_CALLBACK(+[](GtkEventControllerScroll*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         self->cancelScrollMomentum();
                         self->scrollAxisLock.begin();
                         self->scrollGestureAllowsMomentum = false;
                     }),
                     this);
    g_signal_connect(
            scroll, "scroll",
            G_CALLBACK(+[](GtkEventControllerScroll* controller, double dx, double dy, gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
                const bool discrete = event && gdk_scroll_event_get_unit(event) == GDK_SCROLL_UNIT_WHEEL;
                const auto modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)) &
                                       gtk_accelerator_get_default_mod_mask();
                if (modifiers & ~(GDK_CONTROL_MASK | GDK_SHIFT_MASK)) {
                    self->scrollGestureAllowsMomentum = false;
                    return true;
                }
                if (self->input.active() || self->state.blocksScroll(discrete, g_get_monotonic_time())) {
                    self->scrollGestureAllowsMomentum = false;
                    return true;
                }
                if (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)) & GDK_CONTROL_MASK) {
                    self->cancelScrollMomentum();
                    self->scrollGestureAllowsMomentum = false;
                    if (dy != 0) {
                        const auto anchor = self->scrollAnchor(GTK_EVENT_CONTROLLER(controller));
                        const auto zoom = prototype::behavior::zoomFromScrollDelta(self->state.zoom, dy, discrete,
                                                                                   self->state.scrollZoomStep);
                        self->zoom(zoom, anchor.x, anchor.y, true);
                    }
                } else {
                    if (self->presentationMode) {
                        self->scrollGestureAllowsMomentum = false;
                        return true;
                    }
                    if (discrete) {
                        self->cancelScrollMomentum();
                        self->scrollGestureAllowsMomentum = false;
                        self->applyCanvasScroll(dx, dy, true);
                    } else {
                        self->scrollGestureAllowsMomentum = true;
                        const bool horizontalAllowed = self->state.hasHorizontalScrollRoom(
                                gtk_widget_get_width(self->canvas));
                        const auto delta =
                                self->scrollAxisLock.filter(dx, dy, horizontalAllowed);
                        self->applyCanvasScroll(delta.x, delta.y, false);
                    }
                }
                return true;
            }),
            this);
    g_signal_connect(scroll, "scroll-end", G_CALLBACK(+[](GtkEventControllerScroll*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (!self->scrollGestureAllowsMomentum || self->presentationMode || self->input.active()) {
                             return;
                         }
                         const bool horizontalAllowed = self->state.hasHorizontalScrollRoom(
                                 gtk_widget_get_width(self->canvas));
                         const auto delta = self->scrollAxisLock.finish(horizontalAllowed);
                         self->applyCanvasScroll(delta.x, delta.y, false);
                     }),
                     this);
    g_signal_connect(scroll, "decelerate",
                     G_CALLBACK(+[](GtkEventControllerScroll*, double velocityX, double velocityY, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         if (self->scrollGestureAllowsMomentum && !self->presentationMode && !self->input.active() &&
                             !self->state.blocksScroll(false, g_get_monotonic_time())) {
                             self->startScrollMomentum(velocityX, velocityY);
                         }
                         self->scrollGestureAllowsMomentum = false;
                         self->scrollAxisLock.begin();
                     }),
                     this);
    gtk_widget_add_controller(canvas, scroll);

    keys = gtk_event_controller_key_new();
    g_signal_connect(
            keys, "key-pressed",
            G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType modifiers,
                           gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                if (self->busy || self->choosingFile || self->messageDialog) {
                    return false;
                }
                if (key == GDK_KEY_Escape && self->searchController.isOpen()) {
                    self->closeSearchBar();
                    return true;
                }
                if ((modifiers & GDK_CONTROL_MASK) && !(modifiers & GDK_SHIFT_MASK) &&
                    !(modifiers & (GDK_ALT_MASK | GDK_SUPER_MASK)) && (key == GDK_KEY_f || key == GDK_KEY_F)) {
                    self->openSearchBar();
                    return true;
                }
                bool editingText = self->textEditor && self->textEditor->active();
                if (editingText) {
                    const bool control = (modifiers & GDK_CONTROL_MASK) != 0;
                    const bool shift = (modifiers & GDK_SHIFT_MASK) != 0;
                    if (control && (key == GDK_KEY_b || key == GDK_KEY_B)) {
                        self->applyTextFormat(prototype::TextEditorOverlay::Format::Bold);
                        return true;
                    }
                    if (control && (key == GDK_KEY_i || key == GDK_KEY_I)) {
                        self->applyTextFormat(prototype::TextEditorOverlay::Format::Italic);
                        return true;
                    }
                    if (control && (key == GDK_KEY_u || key == GDK_KEY_U)) {
                        self->applyTextFormat(prototype::TextEditorOverlay::Format::Underline);
                        return true;
                    }
                    if (control && shift && (key == GDK_KEY_k || key == GDK_KEY_K)) {
                        self->applyTextFormat(prototype::TextEditorOverlay::Format::Strikethrough);
                        return true;
                    }
                    if (control && key == GDK_KEY_period) {
                        self->applyTextFormat(prototype::TextEditorOverlay::Format::Superscript);
                        return true;
                    }
                    if (control && key == GDK_KEY_comma) {
                        self->applyTextFormat(prototype::TextEditorOverlay::Format::Subscript);
                        return true;
                    }
                    if (key == GDK_KEY_Escape || (control && key == GDK_KEY_Return)) {
                        self->finishTextEditing();
                        return true;
                    }
                    const bool documentAction =
                            (control && (key == GDK_KEY_o || key == GDK_KEY_O || key == GDK_KEY_s || key == GDK_KEY_S ||
                                         key == GDK_KEY_e || key == GDK_KEY_E || key == GDK_KEY_g || key == GDK_KEY_G ||
                                         key == GDK_KEY_d || key == GDK_KEY_D || key == GDK_KEY_0 ||
                                         key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_minus ||
                                         key == GDK_KEY_Home || key == GDK_KEY_End)) ||
                            (control && shift &&
                             (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete || key == GDK_KEY_g ||
                              key == GDK_KEY_G || key == GDK_KEY_r || key == GDK_KEY_R)) ||
                            key == GDK_KEY_Page_Down || key == GDK_KEY_Page_Up ||
                            ((modifiers & GDK_SHIFT_MASK) &&
                             (key == GDK_KEY_j || key == GDK_KEY_J || key == GDK_KEY_k || key == GDK_KEY_K));
                    if (documentAction) {
                        self->finishTextEditing();
                        editingText = false;
                    } else {
                        return false;
                    }
                }
                if (!editingText && !GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(self->window))) &&
                    self->pluginRuntime && self->pluginRuntime->handleKeyPress(key, modifiers, false)) {
                    self->update();
                    return true;
                }
                self->input.setModifiers(
                        (modifiers & GDK_SHIFT_MASK) || key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R,
                        (modifiers & GDK_CONTROL_MASK) || key == GDK_KEY_Control_L || key == GDK_KEY_Control_R,
                        (modifiers & GDK_ALT_MASK) || key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R);
                if (self->state.verticalSpacePreview() && (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R)) {
                    self->state.setVerticalSpaceDirection(self->input.controlHeld);
                    self->update();
                    return true;
                }
                if (self->input.splineInProgress && !editingText &&
                    !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
                    if (key == GDK_KEY_BackSpace) {
                        self->input.removeSplineKnot();
                        self->update();
                        return true;
                    }
                    if (key == GDK_KEY_Left || key == GDK_KEY_Right || key == GDK_KEY_Up || key == GDK_KEY_Down) {
                        const double delta = key == GDK_KEY_Left ? -1 : key == GDK_KEY_Right ? 1 : 0;
                        const double vertical = key == GDK_KEY_Up ? -1 : key == GDK_KEY_Down ? 1 : 0;
                        self->input.moveLastSplineKnot(delta, vertical);
                        self->update();
                        return true;
                    }
                    if (key == GDK_KEY_r || key == GDK_KEY_R) {
                        self->input.rotateLastSplineTangent(key == GDK_KEY_R ? -5 : 5);
                        self->update();
                        return true;
                    }
                    if (key == GDK_KEY_s || key == GDK_KEY_S) {
                        self->input.scaleLastSplineTangent(key == GDK_KEY_S ? 1.0 / 1.05 : 1.05);
                        self->update();
                        return true;
                    }
                }
                if (!editingText && !GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(self->window)))) {
                    if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_c || key == GDK_KEY_C)) {
                        if (self->pdfTextSelection && self->pdfTextSelection->finalized) {
                            self->copyPdfTextSelection(false);
                        } else {
                            self->copySelection();
                        }
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_x || key == GDK_KEY_X)) {
                        if (self->pdfTextSelection && self->pdfTextSelection->finalized) {
                            self->copyPdfTextSelection(false);
                        } else {
                            self->copySelection(true);
                        }
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_v || key == GDK_KEY_V)) {
                        self->pasteSelection();
                        return true;
                    }
                    if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
                        (key == GDK_KEY_g || key == GDK_KEY_G)) {
                        self->activateSelection(Tool::SelectLasso);
                        return true;
                    }
                    if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                        (key == GDK_KEY_s || key == GDK_KEY_S)) {
                        self->activateSelection(Tool::SmartSelect);
                        return true;
                    }
                }
                if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_o || key == GDK_KEY_O)) {
                    if (!self->options.worker.empty()) {
                        self->protectDocument([self] { self->chooseOpen(); });
                    }
                    return true;
                }
                if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_s || key == GDK_KEY_S)) {
                    if (modifiers & GDK_SHIFT_MASK) {
                        self->chooseSave();
                    } else {
                        self->save();
                    }
                    return true;
                }
                if ((modifiers & GDK_CONTROL_MASK) && !(modifiers & GDK_SHIFT_MASK) &&
                    (key == GDK_KEY_e || key == GDK_KEY_E)) {
                    self->chooseExportPdf();
                    return true;
                }
                if ((modifiers & GDK_CONTROL_MASK) && !(modifiers & GDK_SHIFT_MASK) &&
                    (key == GDK_KEY_g || key == GDK_KEY_G)) {
                    self->openGotoPageDialog();
                    return true;
                }
                if ((modifiers & GDK_CONTROL_MASK) && !(modifiers & GDK_SHIFT_MASK) &&
                    (key == GDK_KEY_d || key == GDK_KEY_D)) {
                    self->insertPageAfterCurrent();
                    return true;
                }
                if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
                    (key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete)) {
                    self->deleteCurrentPage();
                    return true;
                }
                if (key == GDK_KEY_Escape) {
                    if (self->pdfTextSelection) {
                        self->clearPdfTextSelection(false);
                    } else if (self->imageGesture) {
                        self->resetInput();
                        self->selectedImageIndex.reset();
                    } else if (self->selectedImageIndex) {
                        self->selectedImageIndex.reset();
                    } else if (self->input.splineInProgress) {
                        self->input.finalizeSpline();
                    } else {
                        self->resetInput();
                        self->state.clearSelection();
                    }
                } else if (editingText || GTK_IS_EDITABLE(gtk_window_get_focus(GTK_WINDOW(self->window)))) {
                    return false;
                } else if ((modifiers & GDK_CONTROL_MASK) && (modifiers & GDK_SHIFT_MASK) &&
                           (key == GDK_KEY_r || key == GDK_KEY_R)) {
                    self->activateSelection(Tool::SelectRectangle);
                } else if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_a || key == GDK_KEY_A)) {
                    self->selectedImageIndex.reset();
                    self->input.cancel();
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toolButtons[3]), true);
                    self->state.selectAllOnPage();
                } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_Delete || key == GDK_KEY_BackSpace) && self->selectedImageIndex) {
                    self->resetInput();
                    self->state.deleteImage(*self->selectedImageIndex);
                    self->selectedImageIndex.reset();
                } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_Delete || key == GDK_KEY_BackSpace) && self->state.hasSelection()) {
                    self->input.cancel();
                    self->state.deleteSelection();
                } else if (key == GDK_KEY_space && !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK))) {
                    self->input.spaceHeld = true;
                    gtk_widget_set_cursor_from_name(self->canvas, "grab");
                } else if ((modifiers & GDK_CONTROL_MASK) && (key == GDK_KEY_z || key == GDK_KEY_Z)) {
                    self->performHistoryStep((modifiers & GDK_SHIFT_MASK) != 0);
                } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_y) {
                    self->performHistoryStep(true);
                } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_0 && !self->presentationMode) {
                    self->fitted = false;
                } else if ((modifiers & GDK_CONTROL_MASK) &&
                           (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_minus)) {
                    self->zoom(self->state.steppedZoom(key != GDK_KEY_minus, false),
                               gtk_widget_get_width(self->canvas) / 2.0, gtk_widget_get_height(self->canvas) / 2.0,
                               true);
                } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_Home) {
                    self->navigateToPage(0);
                } else if ((modifiers & GDK_CONTROL_MASK) && key == GDK_KEY_End) {
                    self->navigateToPage(self->pageCount - 1);
                } else if (self->presentationMode &&
                           !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_Home || key == GDK_KEY_KP_Home)) {
                    self->navigateToPage(0);
                } else if (self->presentationMode &&
                           !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_End || key == GDK_KEY_KP_End)) {
                    self->navigateToPage(self->pageCount - 1);
                } else if (key == GDK_KEY_Page_Down || key == GDK_KEY_Page_Up) {
                    self->navigateToPage(self->state.page + (key == GDK_KEY_Page_Down ? 1 : -1));
                } else if (self->presentationMode &&
                           !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_Up || key == GDK_KEY_k || key == GDK_KEY_K || key == GDK_KEY_Left ||
                            key == GDK_KEY_h || key == GDK_KEY_H)) {
                    self->navigateToPage(self->state.page - 1);
                } else if (self->presentationMode &&
                           !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_Down || key == GDK_KEY_j || key == GDK_KEY_J || key == GDK_KEY_Right ||
                            key == GDK_KEY_l || key == GDK_KEY_L)) {
                    self->navigateToPage(self->state.page + 1);
                } else if ((modifiers & GDK_SHIFT_MASK) &&
                           !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_J || key == GDK_KEY_j || key == GDK_KEY_K || key == GDK_KEY_k)) {
                    self->navigateToPage(self->state.page + ((key == GDK_KEY_J || key == GDK_KEY_j) ? 1 : -1));
                } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) && key == GDK_KEY_colon) {
                    self->openGotoPageDialog();
                    return true;
                } else if (!(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
                           (key == GDK_KEY_p || key == GDK_KEY_h || key == GDK_KEY_e ||
                            ((key == GDK_KEY_t || key == GDK_KEY_l) && self->nativeSession))) {
                    if (key == GDK_KEY_t) {
                        self->selectTool(Tool::Text);
                        return true;
                    }
                    if (key == GDK_KEY_l) {
                        self->selectTool(Tool::Link);
                        return true;
                    }
                    const auto tool = key == GDK_KEY_p ? 0 : key == GDK_KEY_h ? 1 : 2;
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->toolButtons[tool]), true);
                } else {
                    return false;
                }
                self->update();
                return true;
            }),
            this);
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(
            keys, "key-released",
            G_CALLBACK(+[](GtkEventControllerKey*, guint key, guint, GdkModifierType modifiers, gpointer data) {
                auto* self = static_cast<Prototype*>(data);
                bool shift = modifiers & GDK_SHIFT_MASK;
                bool control = modifiers & GDK_CONTROL_MASK;
                bool alt = modifiers & GDK_ALT_MASK;
                if (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R)
                    shift = false;
                if (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R)
                    control = false;
                if (key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R)
                    alt = false;
                self->input.setModifiers(shift, control, alt);
                if (self->state.verticalSpacePreview() && (key == GDK_KEY_Control_L || key == GDK_KEY_Control_R)) {
                    self->state.setVerticalSpaceDirection(self->input.controlHeld);
                }
                if (key == GDK_KEY_space) {
                    self->input.spaceHeld = false;
                    gtk_widget_set_cursor_from_name(self->canvas, self->input.panning()                ? "grabbing" :
                                                                  self->state.brush.tool == Tool::Hand ? "grab" :
                                                                                                         nullptr);
                }
                self->update();
            }),
            this);
    gtk_widget_add_controller(window, keys);
}

bool Prototype::applyCanvasScroll(double dx, double dy, bool discrete) {
    if (!canvas || presentationMode || (dx == 0 && dy == 0)) {
        return false;
    }

    const auto oldOffsetX = state.offsetX;
    const auto oldOffsetY = state.offsetY;
    state.scrollBy(dx, dy, discrete);
    const auto width = gtk_widget_get_width(canvas);
    const auto height = gtk_widget_get_height(canvas);
    state.constrainScrollOffsets(width, height);
    if (state.offsetX == oldOffsetX && state.offsetY == oldOffsetY) {
        return false;
    }

    fitted = true;
    fitMode = false;
    state.syncActivePage(width, height);
    update();
    return true;
}

void Prototype::startScrollMomentum(double velocityX, double velocityY) {
    cancelScrollMomentum();
    if (!canvas || presentationMode || input.active() || state.pinching ||
        state.blocksScroll(false, g_get_monotonic_time())) {
        return;
    }

    const bool horizontalAllowed = state.hasHorizontalScrollRoom(gtk_widget_get_width(canvas));
    const auto velocity = scrollAxisLock.filterVelocity(velocityX, velocityY, horizontalAllowed);
    if (!scrollMomentum.start(velocity.x, velocity.y)) {
        return;
    }

    scrollMomentumTick = gtk_widget_add_tick_callback(
            canvas,
            +[](GtkWidget*, GdkFrameClock* clock, gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                prototype::ScrollDelta delta;
                const bool continueMotion = self->scrollMomentum.advance(gdk_frame_clock_get_frame_time(clock), delta);
                if ((delta.x != 0 || delta.y != 0) && !self->applyCanvasScroll(delta.x, delta.y, false)) {
                    self->scrollMomentum.stop();
                    self->scrollMomentumTick = 0;
                    return G_SOURCE_REMOVE;
                }
                if (!continueMotion) {
                    self->scrollMomentumTick = 0;
                    return G_SOURCE_REMOVE;
                }
                return G_SOURCE_CONTINUE;
            },
            this, nullptr);
}

void Prototype::cancelScrollMomentum() {
    scrollMomentum.stop();
    if (scrollMomentumTick && canvas) {
        gtk_widget_remove_tick_callback(canvas, scrollMomentumTick);
    }
    scrollMomentumTick = 0;
}

void Prototype::addGesture(GtkGesture* gesture, std::optional<Device> inkOwner) {
    if (inkOwner) {
        g_object_set_data(G_OBJECT(gesture), "ink-owner", GUINT_TO_POINTER(*inkOwner == Device::Mouse ? 1 : 2));
    }
    g_signal_connect(gesture, "cancel", G_CALLBACK(+[](GtkGesture* gesture, GdkEventSequence*, gpointer data) {
                         auto* self = static_cast<Prototype*>(data);
                         const auto owner = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(gesture), "ink-owner"));
                         if (owner && !(owner == 1 && self->state.drawingMode == prototype::DrawingMode::Spline)) {
                             auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
                             if (event && gdk_event_get_event_type(event) == GDK_GRAB_BROKEN) {
                                 self->finish(owner == 1 ? Device::Mouse : Device::Stylus);
                             } else {
                                 self->input.cancel(owner == 1 ? Device::Mouse : Device::Stylus);
                             }
                         }
                         if (gesture == self->pinch && self->state.pinching) {
                             self->state.endPinch(g_get_monotonic_time());
                             self->finishZoomPreview();
                         }
                         self->update();
                     }),
                     this);
    gtk_widget_add_controller(canvas, GTK_EVENT_CONTROLLER(gesture));
}

void Prototype::scheduleSelectionEdgePan(double x, double y) {
    if (!state.selectionCanEdgePan()) {
        stopSelectionEdgePan();
        return;
    }
    selectionEdgePanPointer = prototype::Point{x, y};
    if (selectionEdgePanSource) {
        return;
    }
    selectionEdgePanSource = g_timeout_add(
            1000 / 30,
            +[](gpointer data) -> gboolean {
                auto* self = static_cast<Prototype*>(data);
                if (!self->selectionEdgePanPointer || !self->state.selectionCanEdgePan() || !self->canvas) {
                    self->selectionEdgePanSource = 0;
                    return G_SOURCE_REMOVE;
                }
                const auto width = gtk_widget_get_width(self->canvas);
                const auto height = gtk_widget_get_height(self->canvas);
                const auto pointer = *self->selectionEdgePanPointer;
                if (!self->state.edgePanSelectionAtPointer(pointer.x, pointer.y, width, height, self->input.altHeld)) {
                    self->selectionEdgePanSource = 0;
                    return G_SOURCE_REMOVE;
                }
                self->fitted = true;
                self->fitMode = false;
                self->state.syncActivePage(width, height);
                self->update();
                return G_SOURCE_CONTINUE;
            },
            this);
}

void Prototype::stopSelectionEdgePan() {
    if (selectionEdgePanSource) {
        g_source_remove(selectionEdgePanSource);
        selectionEdgePanSource = 0;
    }
    selectionEdgePanPointer.reset();
}

void Prototype::resetInput(bool interrupted) {
    cancelScrollMomentum();
    stopSelectionEdgePan();
    imageGesture.reset();
    smartSelectPdfGesture.reset();
    state.cancelVerticalSpace();
    if (textSelectionOwner) {
        state.cancelSelection();
    }
    textSelectionOwner.reset();
    pendingStylusTextTap.reset();
    textDragHandled = false;
    if (interrupted) {
        input.interrupt();
    } else {
        input.reset();
    }
    for (auto* pan: panGestures) {
        static_cast<PanState*>(g_object_get_data(G_OBJECT(pan), "pan-state"))->enabled = false;
    }
    if (canvas) {
        gtk_widget_set_cursor_from_name(canvas, state.brush.tool == Tool::Hand ? "grab" : nullptr);
    }
}

void Prototype::playObjectAt(double screenX, double screenY) {
    constexpr double actionRadius = 15.0;
    const auto hit = state.hitPage(screenX, screenY);
    if (!hit) {
        return;
    }
    state.selectActivePage(hit->page);
    const auto& pageLayers = state.layersForPage(hit->page);
    if (pageLayers.selectedLayer == 0 || pageLayers.selectedLayer > pageLayers.layers.size()) {
        return;
    }

    const auto& activeOrigins = pageLayers.layers[pageLayers.selectedLayer - 1].origins;
    std::optional<std::string> playbackError;
    for (size_t origin: activeOrigins) {
        for (const auto& target: audioTargets) {
            if (target.page != hit->page || target.originLayer != origin ||
                !prototype::audioTargetIntersectsArea(target, hit->local.x - actionRadius, hit->local.y - actionRadius,
                                                      2 * actionRadius, 2 * actionRadius) ||
                prototype::audioTargetDistance(target, hit->local.x, hit->local.y) >= actionRadius) {
                continue;
            }
            std::string error;
            const auto filename = target.playbackFilename.empty() ? target.filename : target.playbackFilename;
            if (!audioPlayback || !audioPlayback->start(filename, target.timestampMs, error)) {
                playbackError = error.empty() ? "Audio playback is unavailable" : std::move(error);
            } else {
                playbackError.reset();
            }
        }
    }
    if (playbackError) {
        showAlert("Unable to play audio recording", *playbackError);
    }
    update();
}

void Prototype::syncAudioControls() {
    if (!audioControls || !audioPlayback) {
        return;
    }
    const bool active = audioPlayback->isActive();
    gtk_widget_set_visible(audioControls, active);
    if (audioPauseButton) {
        const bool paused = audioPlayback->isPaused();
        gtk_button_set_icon_name(GTK_BUTTON(audioPauseButton),
                                 paused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic");
        gtk_widget_set_tooltip_text(audioPauseButton, paused ? "Resume audio playback" : "Pause audio playback");
    }
    for (auto* control: {audioBackButton, audioPauseButton, audioForwardButton, audioStopButton}) {
        if (control) {
            gtk_widget_set_sensitive(control, active);
        }
    }
}

void Prototype::trackPointerPosition(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    if (pointerPosition && pointerPosition->x == x && pointerPosition->y == y) {
        return;
    }
    pointerPosition = prototype::Point{x, y};
    if (canvasOverviewMode()) {
        gtk_widget_queue_draw(canvas);
    }
    if (importedConfig.settings.highlightPosition && canvas) {
        gtk_widget_queue_draw(canvas);
    }
}

void Prototype::begin(Device device, double x, double y, std::optional<double> pressure, bool eraserEnd,
                      uint32_t timeMs) {
    cancelScrollMomentum();
    if (device == Device::Mouse && state.brush.tool == Tool::SmartSelect) {
        textDragHandled = true;
    }
    trackPointerPosition(x, y);
    hidePdfActionsPreservingSelection();
    if (state.brush.tool == Tool::Latex) {
        gtk_widget_grab_focus(canvas);
        if (device == Device::Mouse) {
            return;
        }
        if (device != Device::Stylus || (!eraserEnd && activeStylusAction == prototype::ButtonToolAction::None)) {
            openLatexEditorAt(x, y);
            return;
        }
    }
    if (state.brush.tool == Tool::FloatingToolbox ||
        (device == Device::Stylus && !eraserEnd &&
         activeStylusAction == prototype::ButtonToolAction::FloatingToolbox)) {
        showFloatingToolbox(x, y);
        gtk_widget_grab_focus(canvas);
        return;
    }
    if (state.brush.tool == Tool::PlayObject) {
        gtk_widget_grab_focus(canvas);
        playObjectAt(x, y);
        return;
    }
    if (state.brush.tool == Tool::VerticalSpace &&
        !(device == Device::Stylus && activeStylusAction != prototype::ButtonToolAction::None)) {
        state.beginVerticalSpace(x, y, input.controlHeld);
        gtk_widget_grab_focus(canvas);
        update();
        return;
    }
    if (state.brush.tool == Tool::Link) {
        gtk_widget_grab_focus(canvas);
        return;
    }
    if (state.brush.tool == Tool::Text && device == Device::Stylus && !eraserEnd &&
        activeStylusAction == prototype::ButtonToolAction::None && !(textEditor && textEditor->active())) {
        pendingStylusTextTap = prototype::Point{x, y};
        gtk_widget_grab_focus(canvas);
        return;
    }
    if (beginTextTransform(device, x, y)) {
        gtk_widget_grab_focus(canvas);
        update();
        return;
    }
    if (prototype::isPdfTextSelectionTool(state.brush.tool)) {
        if (reopenPdfActionsForSelection(x, y)) {
            gtk_widget_grab_focus(canvas);
            update();
            return;
        }
        gtk_widget_grab_focus(canvas);
        beginPdfTextSelection(device, x, y);
        return;
    }
    if (beginSmartSelectPdfGesture(device, x, y)) {
        gtk_widget_grab_focus(canvas);
        return;
    }
    if (beginImageGesture(device, x, y)) {
        gtk_widget_grab_focus(canvas);
        update();
        return;
    }
    const bool preserveSelection =
            state.brush.tool == Tool::Hand || input.spaceHeld ||
            (device == Device::Stylus && !eraserEnd && activeStylusAction == prototype::ButtonToolAction::Hand) ||
            !state.hitPage(x, y);
    if (!preserveSelection) {
        selectedImageIndex.reset();
    }
    gtk_widget_grab_focus(canvas);
    const auto toolbarBrush = state.brush;
    const bool previousSpaceHeld = input.spaceHeld;
    if (device == Device::Stylus && !eraserEnd) {
        switch (activeStylusAction) {
            case prototype::ButtonToolAction::Pen:
                state.brush = penBrush;
                break;
            case prototype::ButtonToolAction::Highlighter:
                state.brush = highlightBrush;
                break;
            case prototype::ButtonToolAction::Eraser:
                state.brush = eraserBrush;
                break;
            case prototype::ButtonToolAction::Hand:
                input.spaceHeld = true;
                break;
            case prototype::ButtonToolAction::FloatingToolbox:
                break;
            case prototype::ButtonToolAction::None:
                break;
        }
    }
    input.begin(device, {x, y, pressure, timeMs}, eraserEnd);
    state.brush = toolbarBrush;
    input.spaceHeld = previousSpaceHeld;
    if (input.panning()) {
        gtk_widget_set_cursor_from_name(canvas, "grabbing");
    } else if (state.brush.tool == Tool::Hand || input.spaceHeld) {
        gtk_widget_set_cursor_from_name(canvas, "grab");
    }
    update();
}

void Prototype::motion(Device device, double x, double y, std::optional<double> pressure, uint32_t timeMs) {
    trackPointerPosition(x, y);
    if (state.brush.tool == Tool::Link) {
        updateLinkHover(x, y);
        return;
    }
    if (smartSelectPdfGesture && smartSelectPdfGesture->owner == device) {
        smartSelectPdfGesture->latest = {x, y};
        const auto& start = smartSelectPdfGesture->start;
        if (std::max(std::abs(x - start.x), std::abs(y - start.y)) < 10.0) {
            return;
        }
        const auto gesture = *smartSelectPdfGesture;
        smartSelectPdfGesture.reset();
        beginPdfTextSelection(device, gesture.start.x, gesture.start.y);
        updatePdfTextSelection(device, x, y);
        return;
    }
    if (state.verticalSpacePreview()) {
        state.moveVerticalSpace(x, y, input.controlHeld);
        update();
        return;
    }
    if (device == Device::Stylus && pendingStylusTextTap) {
        const auto start = *pendingStylusTextTap;
        if (std::hypot(x - start.x, y - start.y) < 3.0) {
            return;
        }
        pendingStylusTextTap.reset();
        if (beginTextTransform(device, start.x, start.y)) {
            state.moveSelectionPointer(x, y, input.altHeld);
            scheduleSelectionEdgePan(x, y);
            update();
        }
        return;
    }
    if (textSelectionOwner && *textSelectionOwner == device) {
        state.moveSelectionPointer(x, y, input.altHeld);
        scheduleSelectionEdgePan(x, y);
        gtk_widget_queue_draw(canvas);
        return;
    }
    if (pdfTextSelection && pdfTextSelection->dragging && pdfTextSelection->owner == device) {
        updatePdfTextSelection(device, x, y);
        return;
    }
    if (imageGesture && imageGesture->owner == device) {
        updateImageGesture(x, y);
        update();
        return;
    }
    input.motion(device, {x, y, pressure, timeMs});
    if (input.panning()) {
        fitted = true;
        fitMode = false;
        state.syncActivePage(gtk_widget_get_width(canvas), gtk_widget_get_height(canvas));
    }
    if (state.selecting() || (state.current && !input.panning())) {
        gtk_widget_queue_draw(canvas);
    } else {
        update();
    }
}

void Prototype::finish(Device device, std::optional<double> releasePressure,
                       std::optional<prototype::Point> releasePoint) {
    if (releasePoint) {
        trackPointerPosition(releasePoint->x, releasePoint->y);
    }
    if (smartSelectPdfGesture && smartSelectPdfGesture->owner == device) {
        if (releasePoint) {
            smartSelectPdfGesture->latest = *releasePoint;
        }
        const auto gesture = *smartSelectPdfGesture;
        smartSelectPdfGesture.reset();
        const auto& end = gesture.latest;
        const bool exceededThreshold =
                std::max(std::abs(end.x - gesture.start.x), std::abs(end.y - gesture.start.y)) >= 10.0;
        if (exceededThreshold) {
            beginPdfTextSelection(device, gesture.start.x, gesture.start.y);
            updatePdfTextSelection(device, end.x, end.y);
            finishPdfTextSelection(device, end);
        } else {
            state.clearSelection();
            selectedImageIndex.reset();
            clearPdfTextSelection(false);
            update();
        }
        return;
    }
    if (device == Device::Stylus && state.brush.tool == Tool::Link && releasePoint) {
        handleLinkClick(releasePoint->x, releasePoint->y, 1, input.controlHeld);
        update();
        return;
    }
    if (state.verticalSpacePreview()) {
        state.finishVerticalSpace();
        update();
        return;
    }
    if (textSelectionOwner && *textSelectionOwner == device) {
        stopSelectionEdgePan();
        if (releasePoint) {
            state.moveSelectionPointer(releasePoint->x, releasePoint->y, input.altHeld);
        }
        state.finishSelection();
        textSelectionOwner.reset();
        gtk_widget_set_cursor_from_name(canvas, nullptr);
        update();
        return;
    }
    if (device == Device::Stylus && pendingStylusTextTap) {
        const auto point = *pendingStylusTextTap;
        pendingStylusTextTap.reset();
        startTextEditing(point.x, point.y);
        update();
        return;
    }
    if (pdfTextSelection && pdfTextSelection->dragging && pdfTextSelection->owner == device) {
        finishPdfTextSelection(device, releasePoint);
        return;
    }
    if (imageGesture && imageGesture->owner == device) {
        finishImageGesture();
        return;
    }
    input.finish(device, releasePressure, releasePoint);
    if (!input.active()) {
        gtk_widget_set_cursor_from_name(canvas, input.spaceHeld || state.brush.tool == Tool::Hand ? "grab" : nullptr);
    }
    update();
}

std::optional<double> Prototype::pressure(GtkGestureStylus* stylus) {
    double value;
    if (gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(stylus)) &&
        gtk_gesture_stylus_get_axis(stylus, GDK_AXIS_PRESSURE, &value) && std::isfinite(value)) {
        return value;
    }
    return std::nullopt;
}

void Prototype::consumeBacklog(std::span<const GdkTimeCoord> samples) {
    for (const auto& sample: samples) {
        if (const auto point = prototype::historySample(sample)) {
            if (pendingStylusTextTap || (smartSelectPdfGesture && smartSelectPdfGesture->owner == Device::Stylus) ||
                (textSelectionOwner && *textSelectionOwner == Device::Stylus)) {
                motion(Device::Stylus, point->x, point->y, point->pressure, point->timeMs);
            } else {
                input.motion(Device::Stylus, *point);
            }
        }
    }
}

void Prototype::stylusMotion(GtkGestureStylus* gesture, double x, double y) {
    if (gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture))) {
        GdkTimeCoord* samples = nullptr;
        guint count = 0;
        if (gtk_gesture_stylus_get_backlog(gesture, &samples, &count)) {
            consumeBacklog({samples, count});
        }
        g_free(samples);
    }
    auto* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
    motion(Device::Stylus, x, y, pressure(gesture), event ? gdk_event_get_time(event) : 0);
}

}  // namespace prototype::gtk4
