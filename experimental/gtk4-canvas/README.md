# GTK 4 canvas and input prototype

This migration experiment is built as a separate executable with GTK 4,
libadwaita, and Poppler. The normal Xournal++ build, executable, settings, and
document format are unchanged. There is no installation target.

The prototype opens a PDF or native `.xopp` document in a continuous vertical
multi-page viewport (`15 pt` inter-page gap, horizontal centering of narrower
pages, and automatic active-page tracking as you scroll) and adds pressure-aware pen
strokes, translucent highlights, and whiteout ink on any visible page. It supports custom colors, quick color
presets, Xournal++ discrete tool size presets (`Very Fine`..`Very Thick`) synced with
custom widths, three eraser modes (`Standard` partial `PaddedBox` splitting, `Whiteout`,
and `Delete Stroke`), undo/redo, and zoom/pan with cancellation. A thumbnail sidebar navigates pages without resetting zoom and becomes an overlay in
narrower windows. Its per-page Layers panel selects, shows/hides, adds, deletes,
and merges foreground layers. With a document worker, Open, Save, Save As, Export as PDF,
Insert Page After, and Delete Current Page run in the
background. New ink and text edits are saved as native objects in their active
foreground layers alongside the original content. Open and Close protect
unsaved changes with Save, Discard and Cancel. Save updates the associated
`.xopp` file; Save As changes its filename after a successful save; Export as PDF
writes a flattened PDF including live session ink and text edits without clearing
`.xopp` dirty state. Imported PDFs,
legacy files requiring conversion, symlink inputs and blank notebooks use Save As
for their first save. Without a worker, ink remains temporary.

Each stroke retains its own tool, color, and width. Pen, highlighter, and eraser
settings are remembered separately during a session, initialized from read-only
import of `<XDG_CONFIG_HOME>/xournalpp/settings.xml` (or `--config-dir DIRECTORY`)
when present, then overridden by the prototype's saved Preferences values. The
GTK 4 overlay never rewrites `settings.xml`. Missing values fall back cleanly to
`ToolHandler` `MEDIUM` presets (`1.41 pt`
royal-blue pen, `8.50 pt` yellow highlighter, `8.50 pt` standard eraser) and the
11-color `Palette::default_palette()` (`palettes/xournal.gpl`). Configured `.gpl`
palettes populate the multi-column swatch popover with bordered Cairo swatches.
Highlight segments are composited once per stroke to avoid dark joints; multiplying
the result preserves document text underneath. Whiteout draws opaque white
constant-width strokes serialized natively as `tool="eraser"`.
Undo/redo stores value-semantic stroke slice deltas across insertions, whole-stroke
deletions, and multi-sample partial splits while retaining session history. Thumbnails
include session ink, with up to 16 small PDF backgrounds cached at a time.

The header's Preferences button opens an `AdwPreferencesDialog` with Appearance,
Input, and Behavior pages. Color scheme, page-sidebar visibility, pressure
thresholds, stylus side-button actions, autosave cadence, and keyboard/wheel zoom
steps save immediately to `gtk4-canvas.ini` in the selected configuration
directory. These values take precedence over imported GTK 3 settings. Stylus
side buttons can temporarily select Pen, Highlighter, Eraser, or Hand while held;
releasing a mapped button restores the toolbar tool. Middle-mouse panning remains
available.

Shortcuts: `P` pen, `H` highlight, `E` eraser, `T` text (when a document worker
is enabled), `Ctrl+Z` undo, `Ctrl+Shift+Z` or
`Ctrl+Y` redo, `Page Up`/`Page Down` or `Shift+K`/`Shift+J` previous/next page,
`Ctrl+Home`/`Ctrl+End` first/last page, `Ctrl+G` or `:` (or clicking `1 / N`) Go
to Page, and `Ctrl+0` fit. Tool and navigation shortcuts
leave editable controls alone. Use the header's Pages button to show/hide the
sidebar. Selection modes use `Ctrl+Shift+R` (rectangle), `Ctrl+Shift+G` (lasso), or `S` (Smart Select); drag handles to resize/rotate, hold Shift to add a rectangle, and use `Ctrl+C`/`Ctrl+X`/`Ctrl+V` to copy, cut, or paste supported native strokes. The selection toolbar also has duplicate. With a worker, `Ctrl+O` opens a document, `Ctrl+S` saves,
`Ctrl+Shift+S` opens Save As, `Ctrl+E` exports a PDF, `Ctrl+D` inserts a page
after the active page, and `Ctrl+Shift+Delete` deletes the current page when at
least two pages exist.
The modified flag follows edit identities through undo/redo, including edits made
after undo. Saving preserves editable session ink and its undo history.
Widths are measured in document points; they remain consistent at
different zoom levels. For native documents, the worker extracts supported ink
and text from the editable suffix of every foreground layer, including hidden
layers. Drawing, erasing, and stroke selection target the active visible layer;
background and other layers are not affected. The Layers sidebar selects layers,
shows or hides them, and adds, deletes, or merges them down. Editable strokes can
also be resized, rotated, moved across pages, copied and pasted with native
`.xopp` pressure/color/style retained. Save and export preserve layer grouping,
visibility, and native object order. The clipboard reader limits input to 64 MiB
and one million samples, validates the entire supported selection before
inserting it, and leaves ink unchanged on malformed input.

With the worker, eligible native text in a foreground layer's editable suffix
opens in the canvas and stays in native z-order with strokes. Select the Text
tool and click to create text; Smart Select click edits an existing text item. The inline
GTK text editor preserves UTF-8 content and formatting runs, including bold,
italic, underline, strikethrough, superscript, and subscript. Font family and size
can be set from the toolbar. Text creation, edits, and deletion join undo/redo,
and Save, autosave, and PDF export retain the text content and style ranges.
Text objects are not yet movable or resizable through object selection, and
rotated/scaled text can be rendered but the inline editor does not follow its
transform. IME and accessibility behavior remain unverified.

Behavior preservation is tracked in [BEHAVIOR_PARITY.md](BEHAVIOR_PARITY.md).
Defaults now match the existing app's 47% highlighter opacity, 10% keyboard zoom,
and 2% pointer-centered wheel zoom. Hold Space and drag, or start a drag in a page
margin, to temporarily pan without changing the selected brush. A stylus eraser
end temporarily erases session strokes; an entire eraser drag is one undo step.

## What this experiment establishes

- A real Xournal++ document can be exported by the existing renderer in its own
process, then displayed in a GTK 4 Cairo drawing area through Poppler.
- A bounded 4-page LRU raster cache can separate PDF rasterization from
  interactive ink and continuous multi-page scrolling. Each cached page surface
  has at most 4096 pixels on each side (64 MiB maximum per surface, up to four
  recently visible pages retained). Pan, cross-page scrolling, page navigation,
  and ink reuse cached page rasters; zoom changes refresh visible pages.
- Mouse, stylus, scroll, key, drag, and pinch controllers can be wired to a
  document-coordinate input state, with cancellation and smooth-scroll cooldown.
- The experiment can be rendered and tested without connecting to the user's
  display, D-Bus session, settings, or installed application.

This is **not a port of the complete application or its custom `GtkXournal`
widget**. The session ink and text models are experimental and independent of the
existing document models. A separate document worker reuses the existing native
loader, saver, and renderer without linking GTK 3 into the GTK 4 process. The
worker extracts supported stroke and text suffixes from every foreground layer
for interactive editing; other original objects remain in their native snapshot
and render in per-layer previews. The current
PDF rasterization runs synchronously on the prototype's UI thread.

## Build

Requirements: CMake 3.20+, a C++20 compiler, pkg-config, GTK 4.10+, libadwaita
1.5+, and Poppler GLib development libraries. This initial build targets Linux.

From this worktree's root:

```sh
cmake -S experimental/gtk4-canvas -B build-gtk4-canvas -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-gtk4-canvas -j2
ctest --test-dir build-gtk4-canvas --output-on-failure
```

The prototype accepts `--pdf FILE`, `--document FILE --worker WORKER`,
`--config-dir DIRECTORY`, `--autosave-seconds SECONDS`, `--dark`, and
`--smoke-dir DIRECTORY`. It imports active tool, per-tool colors/sizes, eraser
mode, pressure sensitivity/minimum/multiplier, keyboard/wheel zoom steps,
`showSidebar`, `themeVariant` (`useSystem`/`forceLight`/`forceDark`),
`autosaveEnabled`/`autosaveTimeout`, and `.gpl` color palettes strictly read-only
from `settings.xml` without rewriting that file or mutating process environment
variables. Preferences edits are stored in `gtk4-canvas.ini` only after a value
changes. Explicit `--dark` and `--autosave-seconds` CLI flags override imported
values. A worker can also accompany `--pdf`, or be used alone to
create a blank A4 notebook. Without an input it displays a blank page.

To open native documents, build the optional worker using the normal application's
GTK 3 dependencies. It has no installation target and is disabled by default:

```sh
cmake -S . -B build-document-worker -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_CPPTRACE=OFF -DENABLE_GTK4_DOCUMENT_WORKER=ON -DENABLE_GTEST=ON
cmake --build build-document-worker --target xournalpp-document-worker -j2
build-gtk4-canvas/xournalpp-gtk4-canvas --document notebook.xopp \
  --worker build-document-worker/xournalpp-document-worker
```

Native loading, snapshot creation, rendering and save jobs run on a
background task, with completion on the main context. The header shows progress,
editing is blocked during the operation, and Close waits for it to finish. A
failed open leaves the current document intact; a failed save retains its dirty
state and does not continue a pending Open or Close. Cancelling a file chooser
also preserves the current document.

Each loaded document has a private native snapshot and rendered previews, removed
when the session ends. When opening a native `.xopp`, the worker extracts the
contiguous suffix of lossless `pen`, `highlighter`, and `#ffffffff` `eraser`
strokes plus eligible non-audio native text from every foreground layer,
regardless of visibility. It writes strokes, text, and layer metadata to bounded
transfer files before creating the native snapshot and per-layer previews.
Images, filled/dashed/non-round strokes, unsupported objects, and text outside
the editable suffix stay in their original layer and z-order as visible
non-editable content. Save, autosave, and export reconstruct the layer groups and
reinsert editable ink and text in order, preserving visibility and avoiding
duplication.

The worker exposes `inspect INPUT`, `render INPUT NEW.pdf`,
`export-pdf INPUT OUT.pdf INK_TRANSFER [TEXT_TRANSFER]`,
`open INPUT SNAPSHOT.xopp NEW.pdf [EXTRACTED_INK [EXTRACTED_TEXT]]`,
`insert-page INPUT SNAPSHOT.xopp NEW.pdf AFTER_PAGE`,
`delete-page INPUT SNAPSHOT.xopp NEW.pdf PAGE_INDEX`, `version PATH`,
`save INPUT TARGET.xopp INK_TRANSFER VERSION [TEXT_TRANSFER]`,
`autosave INPUT AUTOSAVE_TARGET.xopp INK_TRANSFER [TEXT_TRANSFER]`, and
`save-copy INPUT NEW.xopp INK_TRANSFER [TEXT_TRANSFER]`.
Save operations accept versioned ink, bounded text packets, and layer metadata;
they rebuild the foreground stack and place native pen/highlighter/whiteout
strokes and text in their recorded layers and order, then use the real saver.
They stage the
file and attached backgrounds in the destination directory and publish the
native document last. Normal Save checks a content version for the file and
attached PDF/pixmap sidecars, including a recheck before publication. External
changes produce an error while preserving the current edits. Explicit Save As
uses the chosen destination version, allowing a confirmed replacement of that
filename. GTK 4 handles overwrite confirmation in Save-mode file choosers
([migration guide](https://docs.gtk.org/gtk4/migrating-3to4.html)). Existing document
and attachment permissions are retained.

For replacement, Save creates a temporary `filename~` backup, removes it after
success, and retains it after a failed publication. Attachment originals are
copied into the private staging directory before replacement, restored on a
pre-commit failure, and retained with a recovery path in the error if rollback
cannot safely restore them. An already existing backup is preserved and blocks
replacement; use Save As or recover that backup first. Save-copy remains a
backend operation that refuses existing filenames and sidecars.

The current publication code targets Linux and requires hard links plus atomic
replacement via rename. The multi-file document/attachment publication is not a
single filesystem transaction. Power-loss durability and Windows/macOS behavior
have not been validated.

With a worker, periodic background autosave runs every 180 seconds by default
(`--autosave-seconds` overrides the interval for deterministic tests) without
disabling the canvas or interrupting in-progress strokes. Associated native
documents autosave to `.name.autosave.xopp` via `.swap` replacement without
touching the saved `.xopp` or `filename~`; unsaved notebooks autosave to
`<XDG_CACHE_HOME>/xournalpp/autosaves/<pid>.autosave.xopp`. Direct `Open` inside
`<XDG_CACHE_HOME>/xournalpp` is refused. Normal Save, Save As, Discard and clean
exit remove the active autosave artifact. Opening a document with an existing
`.name.autosave.xopp` offers `Recover`, `Discard Autosave`, or `Cancel`; blank
startup checks `<XDG_CONFIG_HOME>/xournalpp/emergencysave.xopp` and orphaned
dead-PID cache autosaves (`Restore file` or `Delete file`), keeping restored
documents dirty until explicitly saved. Signal-handler crash dumps are not yet
installed in the GTK 4 frontend.

```sh
python experimental/gtk4-canvas/test-document-worker.py \
  --worker build-document-worker/xournalpp-document-worker --fixtures test/files \
  --report build-document-worker/evidence/validation.json
```

The worker test covers native content/style/layer preservation, pressure and
highlighter serialization, image/PDF attachments, cross-directory external PDF
paths, malformed input, collisions and unchanged source fixtures. Host image
loaders must be allowed to decode the test images.

## Test without disturbing the desktop

The graphical test additionally requires Python 3, Cage with its wlroots headless
backend and Pixman renderer, and `dbus-run-session`. Supply an existing Xournal++
binary for CLI export; the test does not build, install, or replace that binary.

```sh
python experimental/gtk4-canvas/test-isolated.py \
  --binary build-gtk4-canvas/xournalpp-gtk4-canvas \
  --renderer /home/okumnas/Tmp/xournalpp/build/xournalpp \
  --worker build-document-worker/xournalpp-document-worker \
  --evidence build-gtk4-canvas/evidence
```

The harness:

1. Removes inherited display, compositor, and session-bus connections.
2. Creates private runtime, settings, data, cache, state, and temporary folders.
3. Starts a private session bus with desktop-service activation disabled.
4. Starts Cage with `WLR_BACKENDS=headless`, `WLR_RENDERER=pixman`, and GTK's Cairo
   renderer. It never falls back to a desktop backend.
5. Copies the repository's typed-text benchmark into a disposable two-page
   fixture, exports it with the existing renderer, and checks the source hash.
6. Exercises the prototype in light and dark styles and records PNGs, metrics,
   and logs in the evidence directory. A timeout terminates the private process
   group. Temporary settings are removed after completion.

No windows appear on the user's display, no input is injected into their
session, and no recent-document entry is written into their settings.

Some execution sandboxes prohibit local sockets. In that case the private
Wayland/D-Bus test needs permission to create sockets; it still uses the same
headless isolation. Setting `XOURNALPP_PROTOTYPE_GDB=1` runs the prototype under
GDB inside that isolated environment to diagnose failures.

## Coverage and limits

The state test checks anchor-preserving zoom, coordinate conversion, pressure,
device ownership, sparse-segment erasing, undo/redo, page isolation, cancellation,
bounded pinch cooldown, and narrow-view fitting. It also verifies per-stroke
styles, ordered multi-stroke erase restoration, redo invalidation, and the undo
history retention. A separate normalized-input test covers pressure fallback,
pen-release handling, dot thickening, stylus eraser routing, grouped erase undo,
temporary pan, cancellation, and backlog axis flags/order. A separate Cairo pixel test checks colors, widths, uniform
highlight opacity, text visibility beneath highlighting, and renderer state.
Transfer serialization, geometric drawing/recognition/spline/stabilizer checks,
rich-text transfer/rendering, background task/main-context checks, read-only
`settings.xml` and `.gpl` palette import/fallback checks, CLI rejection checks,
filesystem backup/publication and autosave/recovery checks bring the standalone
suite to twenty-one tests.

The graphical test runs actual GTK/libadwaita rendering with a real exported
document. It invokes controller **signals**, rather than injecting hardware
events. It checks visible styled ink, tool-specific presets, highlighter
selection, keyboard and button undo/redo, erasing, gesture cancellation, pinch
state, geometric drawing controls, live Shift constraints, shape recognition,
interactive spline knots/tangents and Escape completion, PDF cache reuse, zoom
rerasterization, thumbnail page navigation, unmap cleanup, and an actual narrower
canvas. It also verifies sidebar collapse, overlay dismissal, page navigation
at the current zoom, and the inline rich-text editor's create, edit, and style
path. It explicitly renders a 2x raster cache
and checks its dimensions. This is **not** a compositor-at-2x or physical HiDPI display test;
the headless compositor reports device scale 1.

Physical stylus pressure/eraser behavior, touchpad and touchscreen routing, palm
rejection, input latency, IME, accessibility, GPU renderers, fractional display
scaling, Windows, and macOS remain unverified. Rich-text editing does not include
object selection or transforms; full object-selection behavior, signal-handler
emergency dump, full document undo, plugins, and toolbar configuration import
remain migration work. Its eraser edits session ink and supported native strokes
on the active layer; it cannot erase flattened PDF content or unsupported native
objects. Do not use its screenshots as evidence of full migration readiness or
improved performance relative to GTK 3.

The backend now recognizes a stylus eraser end and processes its motion backlog.
Synthetic tests cover the normalized adapter and axis conversion. The graphical
test also checks Space press/release, margin dragging, stylus signals with a
synthetic backlog, temporary erasing, release-tail prevention, and cleanup. These
checks do not verify actual tablet firmware or physical hardware event delivery.

With a worker, the graphical test also creates, edits, formats, saves, and reopens
native text and strokes, checks retained text and pressure data, exercises the real unsaved-dialog buttons,
verifies failed-open/save retention and Close protection, and cancels native
Open/Save choosers. It also checks Ctrl+S replacement, Save As filename association,
backup cleanup, and rejected saves after a separate worker changes the file.
A 10 ms test timer must execute while document work is active;
this checks main-loop responsiveness, not an input latency target. Tests select
save destinations directly for deterministic persistence checks. Platform portal
chooser selection remains unverified.

The prototype's full color-chooser dialog is not opened in the isolated test;
the color button's actual color-change signal is exercised. This is a separate
native experiment, not an implementation of the main application's annotation
or toolbar configuration features.

## Drawing tools and stabilization

The Drawing section is inside the existing quick-color popover, so the narrow
toolbar layout stays usable. It provides Freehand, Line, Rectangle, Ellipse,
Arrow, Double arrow, Coordinate axes, Shape recognizer, and Spline modes.
Shift snaps a line to 45° or locks a rectangle/ellipse/coordinate-axis gesture
to equal dimensions. Control centers line/rectangle/ellipse/arrow paths and
reverses the coordinate-axis orientation; Alt also centers the supported
geometric paths.

Shape recognition replaces a sufficiently large trace with a fitted line,
circle/ellipse, triangle, or four-corner rectangle. Its minimum size imports
from `strokeRecognizerMinSize` (40 pt by default). Spline clicks add knots,
dragging a knot sets its tangent, and the first knot closes the path when
clicked within 10 screen pixels. Double-click or Escape commits the stroke;
Backspace removes the last knot, arrows move it, `R`/`r` rotate its tangent, and
`S`/`s` scale it.

The two stabilization controls import the legacy averaging and preprocessor
settings. Arithmetic and velocity-Gaussian averaging can combine with deadzone
or inertia preprocessing; both default to off, matching Xournal++ defaults.
The stabilizer smooths the final segment to the last delivered motion event and
does not use a release-only hover coordinate.

These paths are editable pen/highlighter strokes. The full original recognizer,
grid snapping, draw-direction modifier inversion, native shape objects, and
point-for-point stabilizer equivalence are not claimed. The isolated tests use
synthetic GTK input in a private render session; physical tablet behavior is
still unverified.

## Remaining migration work

The experiment still needs editing of unsupported native objects, layer rename/reorder/lock, multi-column/paired-page layout modes, page-level undo/redo, image insertion and transformation, other clipboard formats and dialogs, IME, device-class-specific mappings, plugins and configurable toolbars. Native geometric objects and the full recognizer/grid-snap behavior also remain outside this stroke-path implementation.
Platform packaging, accessibility, hardware input and GPU validation remain
required before replacing the existing application.

## Stroke selection

Rectangle (`Ctrl+Shift+R`), lasso (`Ctrl+Shift+G`), and Smart Select (`S`) select editable strokes from the active visible foreground layer. Rectangle and lasso selection require every point in a stroke to be inside the selection; paths within one point of a page edge extend to that edge. Lasso uses winding containment, while Smart Select picks the topmost stroke by path distance. Hold Shift to add a rectangle to an existing selection.

Eight handles resize or mirror a selection; the round handle rotates it. Corner scaling preserves aspect ratio, side scaling affects one axis, and pen width scales by `sqrt(abs(sx * sy))`. Move, resize and rotate previews leave the saved model unchanged until release. Cross-page drags transfer the group above destination-page strokes. Each completed move, resize, rotation, delete or paste is one undo step.

`Ctrl+C`, `Ctrl+X` and `Ctrl+V`, plus the toolbar's Duplicate action, support native round-cap pen, highlighter and opaque whiteout strokes through bounded `application/xournal` streams. Unsupported clipboard records are rejected atomically. `Ctrl+A` selects the active page, Delete/Backspace removes selected strokes, and Escape clears selection. Unsupported native objects remain preserved and visible in the document but are not selectable yet; shapes drawn by the prototype are ordinary editable stroke paths. The committed rotation resets the handles to an axis-aligned frame; grid snapping, edge auto-pan, other clipboard formats and native object transforms remain pending. GUI checks use synthetic GTK events and a private GDK clipboard; physical input delivery and exchange with another running Xournal++ process remain unverified.
