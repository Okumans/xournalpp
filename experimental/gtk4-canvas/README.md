# GTK 4 canvas and input prototype

This migration experiment is built as a separate executable with GTK 4,
libadwaita, and Poppler. The normal Xournal++ build, executable, settings, and
document format are unchanged. There is no installation target.

The prototype opens a PDF or native `.xopp` document and adds pressure-aware pen
strokes and translucent highlights. It supports custom colors, quick color
presets, stroke width, whole-stroke erasing, undo/redo, and zoom/pan with
cancellation. A thumbnail sidebar navigates pages and becomes an overlay in
narrower windows. With a document worker, Open, Save and Save As run in the
background. New ink is saved as native objects alongside the original content. Open and Close protect
unsaved changes with Save, Discard and Cancel. Save updates the associated
`.xopp` file; Save As changes its filename after a successful save. Imported PDFs,
legacy files requiring conversion, symlink inputs and blank notebooks use Save As
for their first save. Without a worker, ink remains temporary.

Each stroke retains its own tool, color, and width. Pen and highlighter settings
are remembered separately during a session. Highlight segments are composited
once per stroke to avoid dark joints; multiplying the result preserves document
text underneath. Undo/redo stores stroke deltas and retains session history. Thumbnails
include session ink, with up to 16 small PDF backgrounds cached at a time.

Shortcuts: `P` pen, `H` highlight, `E` eraser, `Ctrl+Z` undo, `Ctrl+Shift+Z` or
`Ctrl+Y` redo, `Page Up`/`Page Down` navigation, and `Ctrl+0` fit. Tool shortcuts
leave editable controls alone. Use the header's Pages button to show/hide the
sidebar. With a worker, `Ctrl+O` opens a document, `Ctrl+S` saves, and
`Ctrl+Shift+S` opens Save As.
The modified flag follows edit identities through undo/redo, including edits made
after undo. Saving preserves editable session ink and its undo history.
Widths are measured in document points; they remain consistent at
different zoom levels. The eraser acts only on session ink.

Behavior preservation is tracked in [BEHAVIOR_PARITY.md](BEHAVIOR_PARITY.md).
Defaults now match the existing app's 47% highlighter opacity, 10% keyboard zoom,
and 2% pointer-centered wheel zoom. Hold Space and drag, or start a drag in a page
margin, to temporarily pan without changing the selected brush. A stylus eraser
end temporarily erases session strokes; an entire eraser drag is one undo step.

## What this experiment establishes

- A real Xournal++ document can be exported by the existing renderer in its own
  process, then displayed in a GTK 4 Cairo drawing area through Poppler.
- A bounded raster cache can separate PDF rasterization from interactive ink.
  The cache has at most 4096 pixels on each side (64 MiB maximum); one page is
  cached at a time. Pan and ink reuse that cache. Zoom and page changes refresh it.
- Mouse, stylus, scroll, key, drag, and pinch controllers can be wired to a
  document-coordinate input state, with cancellation and smooth-scroll cooldown.
- The experiment can be rendered and tested without connecting to the user's
  display, D-Bus session, settings, or installed application.

This is **not a port of the complete application or its custom `GtkXournal`
widget**. The session ink model is experimental and independent of the existing
stroke model. A separate document worker reuses the existing native loader, saver,
and renderer without linking GTK 3 into the GTK 4 process. Original layers and
editable text are flattened only for display; the worker retains their native data.
The current PDF rasterization runs synchronously on the prototype's UI thread.

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
`--autosave-seconds SECONDS`, `--dark`, and `--smoke-dir DIRECTORY`. A worker can
also accompany `--pdf`, or be used alone to create a blank A4 notebook. Without an
input it displays a blank page.

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

Each loaded document has a private native snapshot and rendered PDF, removed when
the session ends. The native object data is retained; external linked assets
remain external. Saving appends all current session ink to that snapshot, so
repeated saves do not duplicate previously saved ink. Reopening a saved copy
loads its ink as original objects, which are not yet editable in the frontend.

The worker exposes `inspect INPUT`, `render INPUT NEW.pdf`,
`open INPUT SNAPSHOT.xopp NEW.pdf`, `version PATH`,
`save INPUT TARGET.xopp INK_TRANSFER VERSION`,
`autosave INPUT AUTOSAVE_TARGET.xopp INK_TRANSFER`, and
`save-copy INPUT NEW.xopp INK_TRANSFER`.
Save operations accept the versioned ink packets in `DocumentTransfer.h`, append native
pen/highlighter objects to the top layer, and use the real saver. They stage the
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
Transfer serialization, background task/main-context checks and CLI rejection
checks plus filesystem backup/publication and autosave/recovery checks bring the
standalone suite to thirteen tests.

The graphical test runs actual GTK/libadwaita rendering with a real exported
document. It invokes controller **signals**, rather than injecting hardware
events. It checks visible styled ink, tool-specific presets, highlighter
selection, keyboard and button undo/redo, erasing, gesture cancellation, pinch
state, PDF cache reuse, zoom rerasterization, thumbnail page navigation, unmap
cleanup, and an actual narrower canvas. It also verifies sidebar collapse and
overlay dismissal when choosing a page. It explicitly renders a 2x raster cache and checks
its dimensions. This is **not** a compositor-at-2x or physical HiDPI display test;
the headless compositor reports device scale 1.

Physical stylus pressure/eraser behavior, touchpad and touchscreen routing, palm
rejection, input latency, IME, accessibility, GPU renderers, fractional display
scaling, Windows, and macOS remain unverified. The prototype has no rich-text
editor, selection tools, signal-handler emergency dump, full undo system, plugins, or toolbar
configuration import. Its eraser removes only session ink, not flattened PDF
content. Do not use its screenshots as evidence of full migration readiness or
improved performance relative to GTK 3.

The backend now recognizes a stylus eraser end and processes its motion backlog.
Synthetic tests cover the normalized adapter and axis conversion. The graphical
test also checks Space press/release, margin dragging, stylus signals with a
synthetic backlog, temporary erasing, release-tail prevention, and cleanup. These
checks do not verify actual tablet firmware or physical hardware event delivery.

With a worker, the graphical test also saves/reopens actual native copies,
checks retained text and pressure data, exercises the real unsaved-dialog buttons,
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

## Remaining migration work

The experiment still needs the real editing and selection models, full stroke
geometry and partial erasing, continuous-page layout, signal-crash emergency handling, clipboard, dialogs, IME, settings, plugins and
configurable toolbars. Platform packaging, accessibility, hardware input and GPU
validation remain required before replacing the existing application.
