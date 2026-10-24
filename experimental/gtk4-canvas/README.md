# GTK 4 canvas and input prototype

This is the first migration experiment, built as a separate executable with GTK 4,
libadwaita, and Poppler. The normal Xournal++ build, executable, settings, and
document format are unchanged. There is no installation target.

The prototype displays a PDF and adds temporary pressure-aware pen strokes and
translucent highlights. It supports custom colors, quick color presets, stroke
width, whole-stroke erasing, undo/redo, and zoom/pan with cancellation. A thumbnail
sidebar navigates pages and becomes an overlay in narrower windows. Ink is
session-only and is discarded on exit; the prototype never saves over an input
document.

Each stroke retains its own tool, color, and width. Pen and highlighter settings
are remembered separately during a session. Highlight segments are composited
once per stroke to avoid dark joints; multiplying the result preserves document
text underneath. Undo/redo stores stroke deltas and retains session history. Thumbnails
include session ink, with up to 16 small PDF backgrounds cached at a time.

Shortcuts: `P` pen, `H` highlight, `E` eraser, `Ctrl+Z` undo, `Ctrl+Shift+Z` or
`Ctrl+Y` redo, `Page Up`/`Page Down` navigation, and `Ctrl+0` fit. Tool shortcuts
leave editable controls alone. Use the header's Pages button to show/hide the
sidebar. Widths are measured in document points; they remain consistent at
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
stroke model. The GTK 3 renderer is reused through CLI PDF export, not linked into
the GTK 4 process. Original layers and editable text are flattened for display.
The current PDF rasterization runs synchronously on the prototype's UI thread.

## Build

Requirements: CMake 3.20+, a C++20 compiler, pkg-config, GTK 4.10+, libadwaita
1.4+, and Poppler GLib development libraries. This initial build targets Linux.

From this worktree's root:

```sh
cmake -S experimental/gtk4-canvas -B build-gtk4-canvas -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-gtk4-canvas -j2
ctest --test-dir build-gtk4-canvas --output-on-failure
```

The prototype accepts `--pdf FILE`, `--dark`, and `--smoke-dir DIRECTORY`. Without
a PDF it displays a blank page. It does not accept `.xopp` directly: export a
disposable PDF through the existing application, as the isolated test does.

## Test without disturbing the desktop

The graphical test additionally requires Python 3, Cage with its wlroots headless
backend and Pixman renderer, and `dbus-run-session`. Supply an existing Xournal++
binary for CLI export; the test does not build, install, or replace that binary.

```sh
python experimental/gtk4-canvas/test-isolated.py \
  --binary build-gtk4-canvas/xournalpp-gtk4-canvas \
  --renderer /home/okumnas/Tmp/xournalpp/build/xournalpp \
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
Two further CTest checks reject missing files and unknown options (five tests total).

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
editor, selection tools, `.xopp` persistence, full undo system, plugins, or toolbar
configuration import. Its eraser removes only session ink, not flattened PDF
content. Do not use its screenshots as evidence of full migration readiness or
improved performance relative to GTK 3.

The backend now recognizes a stylus eraser end and processes its motion backlog.
Synthetic tests cover the normalized adapter and axis conversion. The graphical
test also checks Space press/release, margin dragging, stylus signals with a
synthetic backlog, temporary erasing, release-tail prevention, and cleanup. These
checks do not verify actual tablet firmware or physical hardware event delivery.

The prototype's full color-chooser dialog is not opened in the isolated test;
the color button's actual color-change signal is exercised. This is a separate
native experiment, not an implementation of the main application's annotation
or toolbar configuration features.

## Proposed PR stack

1. **Canvas/input experiment (this branch).** Separate build, isolated tests,
   reproducible screenshots, and explicit limits. Establish hardware behavior
   and baseline measurements before proceeding with application integration.
2. **Rendering/input adapters.** Extract reusable interfaces around the current
   document renderer and normalized input events; retain GTK 3 behavior and
   tests. Replace the experimental ink implementation with the real model.
3. **Functional GTK 4 application port.** Port `GtkXournal`, toolbar construction,
   clipboard, dialogs, drag-and-drop, IME, and GtkSourceView; preserve action IDs,
   existing documents, editing behavior, and undo.
4. **Libadwaita application design.** Integrate the header/toolbar, adaptive
   sidebar, preferences, appearance, and contextual annotation controls.
5. **Platform and release validation.** Update packaging and CI, validate GPU and
   display scaling behavior, and run physical pen/touch and document round trips.

Each later branch should be based on the previous PR's head, with its PR base set
to that preceding branch. This experiment creates no remote branches or PRs.
