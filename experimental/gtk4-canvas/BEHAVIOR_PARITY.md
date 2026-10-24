# Behavior parity ledger

The existing application is the behavior reference. UI modernization must not
silently replace its tool semantics. This experiment is not yet a drop-in GTK 4
version of Xournal++. The baseline source inspected is commit `83c0deb9b`.

## Source-grounded defaults

| Behavior | Existing implementation | Prototype status |
| --- | --- | --- |
| Highlighter opacity | `src/core/view/StrokeView.h`: `OPACITY_HIGHLIGHTER = 0.47` | Matched, with a pixel test |
| Tool size presets | `ToolHandler::initTools`: Pen `0.42..5.67`, Highlighter `1.0..30.0`, Eraser `1.0..18.0` pt (`MEDIUM` default) | Matched, with bidirectional `Custom` spin sync and per-tool memory |
| Standard eraser | `EraseHandler` + `Stroke::intersectWithPaddedBox`: `PaddedBox` (`halfSize`, `+0.4 * width` cap padding) | Matched across session and active-layer native `.xopp` strokes, with linear split-pressure interpolation and closed-loop seam merging |
| Whiteout eraser | `InputHandler` + `SaveHandler::visitStroke`: opaque `#ffffffff` constant-width `tool="eraser"` stroke | Matched in Cairo rendering, stylus eraser-end routing, and lossless native `.xopp` extraction/serialization |
| Delete-stroke eraser | `EraseHandler` (`ERASER_TYPE_DELETE_STROKE`) | Matched across session and active-layer native `.xopp` strokes, with exact segment clipping + half stroke width at all segment lengths |
| Keyboard zoom step | `Settings::loadDefault`: 10%; `Control` divides by 100 | Matched; imported from `settings.xml` and editable in Preferences, including inverse zoom-out |
| Wheel zoom step | `Settings::loadDefault`: 2%; `ZoomControl::zoomScroll` | Matched; imported from `settings.xml` and editable in Preferences |
| Wheel zoom focus | `onScrolledwindowMainScrollEvent` uses pointer coordinates | Matched; surface-to-canvas mapping checked with real toolbar/sidebar offsets |
| Pressure default & config | `Settings::loadDefault` + `Settings.cpp`: `pressureSensitivity` (and legacy `presureSensitivity`), `minimumPressure` (`>= 0.01`, default `0.05`), `pressureMultiplier` (`1.0`), guessing off | Matched in the normalized input adapter; imported from `settings.xml` and editable in Preferences |
| Missing pressure | `PenInputHandler::filterPressure` retains the last device pressure | Matched, with invalid-axis and missing-axis tests |
| Pen release | `StrokeHandler::onButtonReleaseEvent` finalizes existing samples | Release-only hover coordinates are ignored; an enabled stabilizer may spline-finalize from its last delivered event |
| Small-motion filtering | `InputHandler::PIXEL_MOTION_THRESHOLD` is 0.3 page points; initial dot may thicken | Matched for this part of the pipeline |
| Geometric drawing tools | `BaseShapeHandler`, `LineHandler`, `RectangleHandler`, `EllipseHandler`, `ArrowHandler`, `CoordinateSystemHandler` | Line, rectangle, ellipse, arrow, double-arrow, and coordinate-axis paths preview during drag and commit through shared stroke undo. Shift snaps lines to 45° or locks square/circle proportions; Control/Alt center supported shapes. |
| Shape recognizer | `ShapeRecognizer` + `CircleRecognizer`; minimum size defaults to 40 pt | Read-only `strokeRecognizerMinSize` import; recognizes near-straight lines, circles/ellipses, closed triangles, and four-corner rectangles. It uses a compact geometric fit, not the full segment-inertia recognizer. |
| Stroke stabilization | `StrokeStabilizer`: `NONE`/`ARITHMETIC`/`VELOCITY_GAUSSIAN` with `NONE`/`DEADZONE`/`INERTIA`; defaults are no averaging and no preprocessor | Both dropdowns and the corresponding legacy settings values are supported. Arithmetic and velocity-Gaussian windows, deadzone/cusp filtering, inertia, and a smooth tail to the last delivered event are implemented with synthetic checks. Physical input latency and exact point-for-point equivalence remain unverified. |
| Spline tool | `SplineHandler`: multi-click knots, drag tangents, first-knot attraction, Backspace/arrows/R/S, double-click/Escape completion | Multi-click knot editing, cubic Bezier preview, tangent drags, 10 px close-to-first completion, Backspace/arrows/R/S editing, and double-click/Escape completion are implemented. The generated spline is stored as a normal editable stroke. |
| Stylus tool buttons | `StylusInputHandler::changeTool` applies configured `stylus`/`stylus2` actions temporarily | Pen, Highlighter, Eraser, and Hand side-button actions are editable and persisted; overrides restore the toolbar tool on release. Hardware event delivery remains unverified. |
| Temporary hand tool | `XournalView` space press/release and `PenInputHandler::temporaryPan` | Space-drag remains a hand gesture until pointer release |
| Outside-page drag | `PenInputHandler::actionStart` temporarily selects Hand | Matched across horizontal margins and the 15 pt vertical inter-page gap |
| Continuous vertical layout | `Layout.cpp`: `XOURNAL_PADDING_BETWEEN = 15`, horizontal centering, `Layout::updateVisibility` largest-intersection-area active page | Matched for single-column vertical layout; unified in document points (eliminating legacy `PixelCounter` mixed-unit zoom-anchor drift) with a 4-page LRU raster cache and zoom-preserving navigation |
| Page navigation & Go to Page | `ui/mainmenubar.xml` + `83c0deb9b`: `Page Up`/`Page Down`, `Shift+K`/`Shift+J`, `Ctrl+Home`/`Ctrl+End`, `Ctrl+G` & `:` | Matched, plus clicking the `1 / N` indicator button |
| Insert / Delete Page | `ui/mainmenubar.xml` + `PageBackgroundChangeController::insertNewPage` (`Ctrl+D`, `Ctrl+Shift+Delete`) | Matched via staged worker revisions; copies paper style/color and intentionally inserts clean `Lined` white paper after PDF/image pages instead of duplicating the PDF slide |
| Export as PDF | `ui/mainmenubar.xml` + `XojCairoPdfExport` (`Ctrl+E`) | Matched via staged atomic worker export including unsaved session ink without clearing `.xopp` modified state |
| Broken grab | `StylusInputHandler`/`MouseInputHandler` call `actionEnd` | Delivered points finalize; explicit gesture cancellation discards unfinished ink |
| Eraser undo | `EraseHandler` (`EraseUndoAction` / `DeleteUndoAction`) | One partial or whole-stroke eraser drag is one delta undo/redo action without raw `UndoAction*` pointers |
| Saved status | `UndoRedoHandler::savedUndo` tracks edit identity | Revision identities track saved state through undo, redo and history branching |
| Unsaved changes | `Control::close` offers Save, Discard and Cancel | Save/Save As, Discard and Cancel; failed saves do not continue pending actions |
| Save / Save As | `Control::saveImpl` associates the filename and `SaveJob` writes it | Matched for the prototype native model bridge; new/legacy/PDF inputs use Save As |
| Temporary backup | `SaveJob` uses `filename~`, removes it after success | Matched; existing backups are preserved, and staged attachment rollback is tested |
| Autosave interval | `Settings::loadDefault` + `Control::enableAutosave`: 3 min (`180s`), `AutosaveJob` (`Job`) | Matched (`autosaveEnabled` & `autosaveTimeout` minutes imported from `settings.xml` and editable in Preferences; `--autosave-seconds` overrides for tests); runs in background without blocking active strokes |
| Autosave paths & swap | `AutosaveJob::run` + `Util::getAutosaveFilepath`: `.name.autosave.xopp` or `<cache>/xournalpp/autosaves/<pid>.autosave.xopp` via `.swap` | Matched, with `UndoRedoHandler::isChangedAutosave` revision tracking and cleanup on Save, Discard and clean exit |
| Cache autosave guard | `Control.cpp` `shouldFileOpen`: refuses direct Open under `<cache>/xournalpp` | Matched, including symlink resolution into the cache tree |
| Crash recovery | `XournalMain::checkForEmergencySave`: `emergencysave.xopp` (`Delete file` / `Restore file`, unnamed & dirty) | Matched for `emergencysave.xopp`; extended to also prompt on dead-PID cache autosaves and `.name.autosave.xopp` sidecars |
| Layer management | `XojPage` layers and `LayerView` | The per-page sidebar selects foreground layers, toggles background/foreground visibility, adds and deletes layers, and merges a layer down. Layer changes participate in undo/redo. Merge into the background, rename, reorder, and lock controls are not implemented. |
| Native files | `LoadHandler`, `SaveHandler`, `DocumentView`, `XojCairoPdfExport` | Reused in a separate worker; supported stroke and text suffixes from every foreground layer are extracted for editing and saved back into their layer groups in order. Remaining native objects stay in the document snapshot and render in per-layer previews. |
| Native rich text | `Text`, `TextView`, `TextEditor`, `UndoRedoHandler` | Eligible non-audio native text loads into a Pango renderer and inline `GtkTextView`; UTF-8, font, color, wrap, alignment, transform, and supported style runs round-trip. Text tool creates items; Smart Select click edits them. Bold, italic, underline, strikethrough, superscript, and subscript work on selected text and join shared undo/redo. Text is not yet movable or resizable with object selection, and the editor overlay does not follow rotated/scaled transforms. |
| Preferences (`settings.xml` + `gtk4-canvas.ini`) | `Settings::load` + `ToolHandler::loadSettings`: `<XDG_CONFIG_HOME>/xournalpp/settings.xml` | Read-only import (`--config-dir` override) of active tool, per-tool `hex`/`int` colors & discrete sizes, eraser mode, pressure, zoom, sidebar, theme, autosave, palette, and `stylus`/`stylus2` mappings; editable Adw Preferences write only `gtk4-canvas.ini`, which overlays imported values. Existing GTK 3 settings are never changed. |
| Palette import (`.gpl`) | `Palette` (`ColorPalette.cpp`, `NamedColor.cpp`) + `palettes/xournal.gpl` | Read-only `.gpl` import (`Name:`, `Columns:`, `R G B Name`) with 11-color `Palette::default_palette()` default/fallback; eliminates legacy `parseCommentLine` `line.front()` UB on blank lines and refuses symlinks |
| Stroke selection | `Selector.cpp`, `LassoSelector.cpp`, `SmartSelect.cpp`, and `Stroke::isInSelection`: all-point rectangle/lasso winding containment with 1 pt page-edge extension; object picking follows stroke path distance | Rectangle (`Ctrl+Shift+R`), lasso (`Ctrl+Shift+G`), Smart Select (`S`), Shift-add rectangle, active-page `Ctrl+A`, Delete and Escape. Selects editable strokes from the active visible foreground layer. |
| Selection transforms | `EditSelection.cpp` affine transform, eight resize handles and rotation anchor; `Stroke::scale` applies geometric-mean width scaling | Move, axis-side resize, aspect-preserving corner resize, mirror, rotate and cross-page transfer use preview matrices. Preview leaves committed autosave geometry unchanged; release records one undo step and retains pressure/style. |
| Selection clipboard | `ClipboardHandler.cpp`: native `application/xournal` object stream, plus text/SVG/PNG targets | Bounded strict decoder and native encoder support editable pen, highlighter and opaque whiteout strokes. GDK Copy/Cut/Paste and duplicate append as one undoable group. Unsupported native objects, custom caps, fill, dash, audio and alpha are rejected atomically; SVG/PNG export, text/image clipboard and editable non-stroke objects remain pending. |
| Undo retention | `UndoRedoHandler::addUndoAction` does not impose an edit-count cap | Removed the experiment's arbitrary 128-step cap |

GTK 3 disables motion-event compression for its custom widget. The GTK 4 adapter
consumes `GtkGestureStylus` backlog samples in chronological order before the
latest motion, checking axis flags and retaining missing pressure. These paths
are exercised with synthetic samples and controller signals, not physical pen
events. Configurable stylus side-button mappings are implemented; the legacy
`deviceClasses` per-device identity table and physical tablet delivery remain
unverified.

Core tool presets, colors, zoom steps, pressure thresholds, shape-recognition
threshold, stabilizer settings, theme, autosave cadence, `.gpl` palettes, and
legacy stylus mappings are imported without modifying GTK 3 configuration
files. The interactive `AdwPreferencesDialog` stores supported edits separately
in `gtk4-canvas.ini`. Per-device `deviceClasses` mappings and `toolbar.ini`
layouts remain unmigrated. The prototype uses independent ink and text models
for session edits and transfers them to the real native model when saving.

## Known differences that block application replacement

- Shift line/aspect constraints and the main stabilizer families are migrated,
  but draw-direction modifier inversion, grid snapping, Shift-click stroke
  connection, the original inertia/cusp edge cases, and device-specific button
  mappings (`buttonConfig`, `deviceClasses`) still differ or remain absent.
- Shape recognition is a geometric subset: it does not port the original
  segment queue/inertia model, all recognition scores, or snap-to-grid result
  placement. Geometric tools output stroke paths rather than native rectangle,
  ellipse, or arrow model objects.
- The original uses its document, selection, layer, undo, and rich-text models.
  When opening a `.xopp` via the worker, supported round-cap,
  unfilled, undashed, non-audio `pen`, `highlighter`, and `#ffffffff` `eraser`
  strokes plus valid non-empty native text are extracted from the contiguous
  editable suffix of every foreground layer, including hidden layers. Stroke
  pressure sentinels are normalized into positive
  per-point multipliers. Text retains UTF-8 content, supported style runs, and
  its transform. Both are removed from the native snapshot and written back in
  their original layer groups and order, avoiding duplicate objects. Objects
  preceding a non-editable suffix element, unsupported stroke types, audio-linked
  text, LaTeX, and images remain visible in per-layer previews but cannot yet be
  edited. Layer visibility, selection, add/delete, and merge-down are supported;
  rename, reorder, lock, and merge into the background are not.
  Text can be created and edited, but is not part of rectangle/lasso selection
  and cannot yet be moved or transformed in the canvas.
- The prototype renders a continuous single-column vertical multi-page layout
  and preserves zoom across page navigation, but horizontal, paired-page, and
  multi-column grid layout modes (`LayoutSettings`), scroll-position history
  stacks, and undo/redo for page insertion/deletion are not yet migrated.
- Relevant `settings.xml` preferences and `.gpl` palettes are imported read-only;
  supported edits are stored in the GTK 4 `gtk4-canvas.ini` overlay. Per-device
  `deviceClasses` mapping and `toolbar.ini` layout customization remain unmigrated.
- The normal application can configure touch behavior, hand recognition,
  scrolling, and gesture zoom. The experiment has only a small input subset.
- The original stroke renderer handles pressure geometry, caps, line styles,
  fill, and blend modes beyond the experiment's simple Cairo segments.
- Remaining work includes other clipboard formats and native object types, IME, plugins, accessibility, platform behavior, and release packaging.
- The input adapter preserves core sequence behavior but uses a compact cubic
  spline path and a single mouse/stylus owner category. It does not yet reuse
  production device-ID routing, the full stroke handler, or exact eraser and
  stabilizer finalization geometry.

Each subsequent migration change should identify its source behavior, add a
regression check, and update this ledger. Passing synthetic input checks is not
evidence of physical pen, touchscreen, or touchpad parity.

## Save and autosave publication limits

Save compares content versions for the primary file and attached PDF/pixmap
sidecars before overwriting, and checks again during publication. It cannot make
multiple filesystem paths atomic against unrelated external writers. Save uses
a private staging directory, publishes the document last, and preserves recovery
copies if attachment rollback is unsafe. The tests inject failures before commit;
they do not simulate power loss. An existing `filename~` is not overwritten.

Background autosaves stage into the target directory and replace `.name.autosave.xopp`
or `<cache>/xournalpp/autosaves/<pid>.autosave.xopp` via `.swap` without modifying the
primary saved document or its `~` backup. Opening a recovery artifact never marks the
recovery path itself as a direct `Ctrl+S` target: sidecar recovery re-associates the
saved `.xopp` while keeping the session dirty until saved, and startup recovery leaves
the document unnamed (`Save As`) and adopts a current-PID cache autosave before removing
the recovered artifact. Unlike GTK 3 `CrashHandler`, the prototype does not write
`emergencysave.xopp` from an async-signal handler; uncatchable crashes (`SIGKILL`)
recover up to the latest completed autosave, and orphaned cache detection uses POSIX
`kill(pid, 0)` liveness checks. Interactive settings persistence and platform-specific
file semantics remain migration work.

## Selection scope

Selection and transforms apply to editable standard strokes in the active visible foreground layer. Rectangle and lasso use all-point containment; both extend a boundary within one point of the page edge. Smart Select picks the topmost editable stroke by path distance, while a Smart Select click opens eligible native text for editing. The native clipboard encoder/decoder supports pen, highlighter and opaque whiteout strokes in bounded `application/xournal` streams; it validates records before allocation/insertion and rejects unsupported elements as a whole.

Move, side/corner resize, mirroring, rotation and cross-page transfer preserve pressure/style and commit as one undoable edit. Preview geometry is separate from the committed autosave snapshot. Cross-page strokes append above existing destination ink. Finite points outside page bounds remain editable and round-trip through the native document worker, matching transform freedom in the original model. After rotation, the prototype returns to an axis-aligned handle frame; it does not yet retain the native selection angle or grid snapping/edge-pan behavior. Text can be edited but cannot yet be selected, moved, resized, or rotated; unsupported native objects cannot be selected or edited; other clipboard targets, physical input, and cross-process clipboard exchange remain unverified or pending.
