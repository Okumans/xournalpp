# Behavior parity ledger

The existing application is the behavior reference. UI modernization must not
silently replace its tool semantics. This experiment is not yet a drop-in GTK 4
version of Xournal++. The baseline source inspected is commit `83c0deb9b`.

## Source-grounded defaults

| Behavior | Existing implementation | Prototype status |
| --- | --- | --- |
| Highlighter opacity | `src/core/view/StrokeView.h`: `OPACITY_HIGHLIGHTER = 0.47` | Matched, with a pixel test |
| Keyboard zoom step | `Settings::loadDefault`: 10%; `Control` divides by 100 | Matched, including inverse zoom-out |
| Wheel zoom step | `Settings::loadDefault`: 2%; `ZoomControl::zoomScroll` | Matched |
| Wheel zoom focus | `onScrolledwindowMainScrollEvent` uses pointer coordinates | Matched; surface-to-canvas mapping checked with real toolbar/sidebar offsets |
| Pressure default | `Settings::loadDefault`: minimum 0.05, multiplier 1, guessing off | Matched in the normalized input adapter |
| Missing pressure | `PenInputHandler::filterPressure` retains the last device pressure | Matched, with invalid-axis and missing-axis tests |
| Pen release | `StrokeHandler::onButtonReleaseEvent` finalizes existing samples | Matched; release position does not append a hover tail |
| Small-motion filtering | `InputHandler::PIXEL_MOTION_THRESHOLD` is 0.3 page points; initial dot may thicken | Matched for this part of the pipeline |
| Stylus eraser | `StylusInputHandler::changeTool` applies the eraser mapping temporarily | Eraser-end routing preserves toolbar brush; custom mappings still absent |
| Temporary hand tool | `XournalView` space press/release and `PenInputHandler::temporaryPan` | Space-drag remains a hand gesture until pointer release |
| Outside-page drag | `PenInputHandler::actionStart` temporarily selects Hand | Matched for prototype page margins |
| Broken grab | `StylusInputHandler`/`MouseInputHandler` call `actionEnd` | Delivered points finalize; explicit gesture cancellation discards unfinished ink |
| Eraser undo | `EraseHandler` adds all deletions to one `DeleteUndoAction` | One whole-stroke eraser drag is one undo/redo action |
| Saved status | `UndoRedoHandler::savedUndo` tracks edit identity | Revision identities track saved state through undo, redo and history branching |
| Unsaved changes | `Control::close` offers Save, Discard and Cancel | Save/Save As, Discard and Cancel; failed saves do not continue pending actions |
| Save / Save As | `Control::saveImpl` associates the filename and `SaveJob` writes it | Matched for the prototype native model bridge; new/legacy/PDF inputs use Save As |
| Temporary backup | `SaveJob` uses `filename~`, removes it after success | Matched; existing backups are preserved, and staged attachment rollback is tested |
| Native files | `LoadHandler`, `SaveHandler`, `DocumentView`, `XojCairoPdfExport` | Reused in a separate worker; text/styles/layers and attachments round-trip tested |
| Undo retention | `UndoRedoHandler::addUndoAction` does not impose an edit-count cap | Removed the experiment's arbitrary 128-step cap |

GTK 3 disables motion-event compression for its custom widget. The GTK 4 adapter
consumes `GtkGestureStylus` backlog samples in chronological order before the
latest motion, checking axis flags and retaining missing pressure. These paths
are exercised with synthetic samples and controller signals, not physical pen
events. Per-device identities and configurable mappings still need migration.

These match the original **defaults**. Importing existing preferences, per-device
mappings, palettes, and toolbar configuration is still required for full parity.
The prototype uses an independent model for session edits and transfers them
to the real native model when saving.

## Known differences that block application replacement

- The original eraser supports standard partial erasing, whole-stroke deletion,
  and whiteout. This experiment only deletes whole session strokes.
- Existing stabilization, pressure inference and configuration, Shift drawing
  constraints, Shift-click connection, stroke/tap filtering, and device-specific
  button mappings have not been migrated.
- The original uses its document, selection, layer, undo, and rich-text models.
  The native worker retains those objects, while rendering flattens them into
  the prototype background. New session ink saves as native objects through
  the worker. Existing objects cannot yet be edited in the prototype.
- The prototype shows a single page at a time and refits on page changes. It
  does not reproduce continuous-page layout, scroll positioning, or page history.
- The prototype's initial brush presets and continuous width control still differ
  from the production toolbar's discrete size presets and saved tool settings.
- The normal application can configure touch behavior, hand recognition,
  scrolling, and gesture zoom. The experiment has only a small input subset.
- The original stroke renderer handles pressure geometry, caps, line styles,
  fill, and blend modes beyond the experiment's simple Cairo segments.
- Original shortcut bindings, clipboard, IME, plugins, accessibility, platform
  behavior, and release packaging still need a complete migration.
- The input adapter preserves core sequence behavior but still uses simplified
  stroke geometry and a single mouse/stylus owner category. It does not yet reuse
  the production device-ID routing, full stroke handler, or eraser geometry.

Each subsequent migration change should identify its source behavior, add a
regression check, and update this ledger. Passing synthetic input checks is not
evidence of physical pen, touchscreen, or touchpad parity.

## Save publication limits

Save compares content versions for the primary file and attached PDF/pixmap
sidecars before overwriting, and checks again during publication. It cannot make
multiple filesystem paths atomic against unrelated external writers. Save uses
a private staging directory, publishes the document last, and preserves recovery
copies if attachment rollback is unsafe. The tests inject failures before commit;
they do not simulate power loss. An existing `filename~` is not overwritten.
Autosave, recovery dialogs, settings import and platform-specific file semantics
remain migration work.
