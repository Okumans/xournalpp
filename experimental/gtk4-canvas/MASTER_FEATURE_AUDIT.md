# GTK 4 migration feature audit

This audit tracks observable behavior from canonical Xournal++ master against
the separate GTK 4/libadwaita prototype. Source snapshot: `upstream/master`
`b2ef6316989d379cfcb6e1048363a0b6c2befa6b`, fetched on 2026-10-25. Update the
status and source reference as each migration slice lands.

Current ledger count: **53 task areas — 40 Partial, 1 Pending, 12 Covered, and 0
Obsolete.** Partial means working code exists with parity gaps listed below;
it does not mean the task is complete.

## Status rules

- **Covered**: behavior is implemented and has a regression check.
- **Partial**: a user-visible subset works; the table names the remaining gap.
- **Pending**: no compatible GTK 4 implementation exists yet.
- **Obsolete**: excluded only after an explicit decision records why the old
  behavior no longer belongs in the migration.

No user-facing master feature is classified obsolete yet. `TOOL_NONE` is an
internal sentinel, not a user feature. Keep every other behavior in scope until
its status changes with source and test evidence.

The fork baseline also contains a PDF highlight recolor toolbar implemented by
`PdfFloatingToolbox::showForObjectSelection`; it is absent from the canonical
upstream snapshot above. Treat that fork-only legacy behavior as outside this
master-parity audit. This does not classify a current master feature obsolete.

## Deferred user-requested follow-ups

Phase 90 added the Eraser Shift constraint. Phase 97 aligned Shift-click
release handling for Pen and all Eraser modes with upstream Pen behavior;
regression checks cover releasing Shift before pointer-up. Phase 96 replaced
the eager page preview rows with a GTK 4 `GtkListView`. A private 10,000-page
sparse-text PDF check bound 205 reusable preview rows, mapped 3 in the
viewport, reused the same row pool at pages 9,795–9,999, and navigated to the
last page. The fixture does not measure thumbnail latency on a content-heavy
real PDF; revisit that case if the reported stall persists.
Phase 98 also changed canvas painting and active-page selection to query only
viewport-intersecting pages, using binary search over the layout grid. The
private 10,000-page check also confirms Go to Page updates and scrolls the page
sidebar selection. Phase 99 moved page-sidebar PDF thumbnail rendering off the
GTK main loop. Phase 100 gives it an independent priority queue and isolated
renderer processes, starts the nearest visible page first, and cancels offscreen
work. The private 10,000-page check renders page 5000 while the canvas remains
on page 0 with its raster cache suppressed, verifies first dispatch, then drives
32 sidebar scroll frames. A no-input private GUI check against the user's
775-page PDF starts with the sidebar open from saved preferences and confirms
the first visible thumbnail renders and redraws, then rapidly scrolls to page 25
and waits for visible pages 25–27 to render while the canvas stays on page 1.
Phase 101 connects sidebar scroll adjustments to visible-page prioritization and
re-requests mapped visible thumbnails as the viewport changes. Its fast-scroll
check no longer calls the prioritizer manually and verifies nonblank pixels on
known content pages, so a cached white fallback cannot count as rendered.
The user confirms that the large-PDF sidebar freeze is resolved after Phase 101
PR #107. Thumbnail latency during continued fast scrolling through the user's
content-heavy PDF remains unmeasured.

Phase 103 corrects native text interaction: Smart Select single-click selects
objects and double-click edits text; the Text tool double-click selects the
clicked word. Entering text editing from Smart Select preserves the current
canvas zoom, and text/image selections draw their shared resize and rotation
handles. Phase 107 groups adjacent committed Pen/whiteout and Highlighter
strokes into bounded shared rasters. Phase 110 gives append-only active Pen,
Highlighter, and whiteout strokes viewport-bounded live rasters, appends new
segments, and repairs the previous join against the canonical Cairo path. It
keeps transformed selections and unsupported active shapes/fills vector
rendered, owns the cache per window, prunes obsolete visible-page runs, and
clears the cache on document replacement. The focused pixel test covers 40
same-bounds committed strokes and live Pen, dashed/pressure Pen, Highlighter,
and whiteout updates; the private Cage smoke passes in light and dark modes.
This does not measure long-running frame time on the user's PDF. Phase 113
reuses committed Pen, Highlighter, and Eraser rasters during rigid Smart Select
moves and rotations; resize previews remain vector rendered to preserve stroke
width changes.

Phase 117 rechecked the text editor after the user reported a wide initial box,
mis-sized bounds, and a hard-to-see caret. The private Cage smoke now snapshots
the full canvas overlay stack; its former fresh-drawing-area repaint covered
GTK overlay children and made the inline editor appear missing in screenshots.
The test captures the empty editor, checks its actual allocation as well as its
requested size, and captures a non-blinking caret frame while restoring the
normal blink setting immediately afterward. A new 24 pt editor measured 40 px
requested and 38 px allocated at 73% zoom; the existing text/layout check,
Smart Select double-click edit, rich formatting, save/reopen, and light/dark
smokes pass. The editing caret aspect ratio is now 0.5. Physical text-input
device behavior remains unverified.

Phase 118 tunes the synthetic scroll model before physical-device retesting. It
enables horizontal motion from the actual scaled page-grid bounds, requires a
2:1 horizontal intent ratio, damps horizontal travel to 18%, and shortens the
maximum momentum coast to about 217 logical pixels. Unit and private Cage
checks cover single-column fit/overflow, a two-column grid, diagonal drift,
horizontal movement, and edge clamping. These checks still do not establish
the user's touchpad sensitivity or physical momentum behavior.

Phase 124 audited Eraser gestures against upstream `StrokeHandler`. GTK4
constrains Standard, Delete Stroke, and Whiteout Eraser movement to the saved
Shift axis, preserves the axis across gestures until Shift is released, and
gates Shift-click continuation on Shift remaining held through pointer release.
Standard/Delete Stroke erase the connecting segment as one undo action;
Whiteout adds a connecting stroke only while its endpoint remains live. The
same-page/layer/mode and release conditions are covered by `CanvasInputTest`,
and the private Cage canvas smoke passed in light and dark modes. Upstream
Shift-click continuation is limited to Pen/Highlighter; extending it to Eraser
modes is the requested GTK4 behavior. This closes the deferred Eraser gesture
item; production Eraser hit geometry and renderer details remain Partial.

Phase 125 is a maintenance-only ownership refactor. `PreferencesUiState` now
groups the Preferences dialog's owned root reference, child-widget references,
LaTeX theme IDs, tool-rail draft, and control-sync guard. Teardown still clears
the dialog at the same point in `Prototype` destruction, and private Cage
Preferences/LaTeX checks plus the full 41-test CTest suite pass. No behavior
parity status or ledger count changed.

## User-directed completion order

Phases 111–114 addressed text-box presentation/caret, synthetic touchpad axis
bounds and momentum, and Search highlight geometry on the user's PDF. The user
has requested the following completion order; keep this sequence current as
each phase lands:

1. Reopen text-box editing for a real UI check and fix any remaining reports:
   text/layout size mismatch, an overly wide initial box, a hard-to-see caret,
   and editor bounds that are missing or do not track the text. Phase 128 now
   requires the relevant no-worker screenshots and passes its GTK/Pango sizing,
   caret, and transformed-editor checks in both themes on the typed-text
   fixture; recheck the user's separate PDF if the issue is still visible there.
2. Verify and tune touchpad scrolling on the user's physical device. Preserve
   momentum, keep horizontal drift very low in a single-column document, allow
   horizontal travel only where the page grid provides it, and clamp movement
   to the grid bounds. Synthetic GTK controller signals do not establish
   physical-device sensitivity or momentum parity.
3. Finish the other deferred user reports below, including real-document ink
   frame-time, sidebar latency on content-heavy PDFs, raster-worker failures,
   Smart Select drag performance, and any remaining tool parity. Phase 122
   covers contextual cursor feedback. Search highlight geometry has been
   visually checked against the user's PDF in private Cage, and Phase 116 made
   Search page-at-a-time and asynchronous; update this list as each item is
   verified.
4. Phase 127 moved the default native `.xopp` and PDF document pipeline into
   the GTK 4 process. Loading, editing, saving, autosave, export, and page
   mutations reuse upstream model and serialization code through curated
   GTK 4 document targets. The optional `--worker` compatibility path remains,
   but ordinary use no longer needs a separate process. The GTK 4 executable
   links GTK 4 without pulling in the monolithic GTK 3 core.
5. Bring every remaining Partial audit item to Covered (or record an explicit
   obsolete decision with its rationale) before the final UI redesign.
6. Reimagine the completed GTK 4 UI as a modern, cohesive workspace. At that
   stage, use a GPT-6.1-Sol high-reasoning agent to plan the redesign, then
   implement and validate the chosen direction against the compact default
   tool layout and customizability requirements above.

Document search remains Enter-driven: typing edits the query and clears stale
results, while Enter or an explicit next/previous action starts a page-at-a-time
scan. Phase 109 implements the interaction. Phase 116 moved PDF text lookup to
a dedicated worker. Phase 120 submits only the next page, waits for its result,
then prepares and submits another; it never creates a document-wide PDF request
on the GTK thread. The worker retains Poppler documents across those page
requests for the same document revision. GTK adds editable-text matches and
updates overlays as each page arrives, so the first match can be used while the
remaining pages scan. Replacing or closing a query cancels the active
generation, and a native document session remains alive until its queued PDF
paths are no longer in use. The focused private Cage check accepts a real PDF,
query, and zero-based page, waits for a valid full-detail background raster,
verifies result rectangles against Poppler, and captures the visible overlay.
On the user's 775-page PDF, searching for `computer` on page 12 found eight
matches aligned with the chapter heading and body text. The page-by-page scan
returned its first result in 9.9 ms, completed in 9.13 seconds, and measured a
22.54 ms maximum GTK heartbeat gap. Physical-device input remains unverified.

Phase 127 routes the default `--document` and `--pdf` startup paths through an
in-process `DocumentSession`. Its native command implementation runs on the
existing document-job thread. The GTK 4 build compiles the upstream XOPP
loader/saver, PDF model/export, and reusable utilities as curated targets,
without linking the monolithic GTK 3 core. Open, edit, save, autosave, PDF
export, and page mutations work without a separate process. An optional
`--worker` compatibility path and standalone worker target remain available;
they are not needed for the GTK 4 application workflow. The private Cage
no-worker smoke passed native open/edit/save/reopen and the common document
actions. Remaining native object types are tracked in the audit below.

Phase 128 re-ran the no-worker GTK 4 canvas smoke in private Cage in light and
dark modes. The Python harness now requires screenshots for the initial text
editor, live text layout, caret, edited text, Smart Select selection, and
transformed text editor on its normal in-process path. GTK assertions checked
the compact initial allocation, growth from the Pango layout, active caret
sizing, and transformed editor coordinates; all passed. This validates the
bundled typed-text fixture, while the user's separate PDF and physical text
input remain unverified.

Phase 129 avoids regenerating a second PDF page by page when opening a source
PDF: the private background-preview bundle now reuses that PDF directly. The
fast path is limited to initial `.pdf` open; layer previews and other document
operations retain their existing rendering path. A private Cage run opened the
user's 775-page PDF without a worker, drained visible sidebar thumbnails, and
completed 40 Pen, 40 Highlighter, and Smart Select drag interactions. Software
renderer p95 frame times were 16.69 ms for Pen, 18.29 ms for Highlighter, and
17.48 ms for Smart Select movement; maximum ink frames were 23.48 ms. Input is
synthetic, so this does not establish physical-device or hardware-renderer
performance. The full CTest suite passed 40/40. The private harness now gives
the app's 90-second timeout a 15-second outer Cage margin and includes captured
subprocess output in timeout errors.

Phase 130 moves `CanvasState` page layout, coordinate conversion, visibility,
and hit-testing method bodies from the large header into the compiled
`CanvasStateGeometry.cpp` unit. The public methods, default arguments, model
ownership, and edit/history behavior are unchanged. The standalone app and
default build passed, all 40 CTests passed, and the full no-worker private Cage
canvas smoke passed. This is the first staged extraction from the remaining
large model header; selection, edit, and history ownership stay in the same
`CanvasState` instance.

Phase 131 moves canvas fit/presentation fit, scroll-bound constraints, page
navigation, and active-page synchronization into the same compiled model unit.
The 7 method bodies are token-identical to the parent implementation. The full
default build and 40 CTests passed, and the private no-worker Cage canvas smoke
passed. `CanvasState` and its history remain the single state owner.

Phase 132 moves the selection geometry, snapping, transform, intersection, and
eraser helpers into `CanvasStateSelection.cpp`, compiled as part of the same
model library. All 19 moved method bodies are token-identical to Phase 131;
state and history ownership remain unchanged. The full default build, 40
CTests, and the private Cage canvas, text, clipboard, selection, document, and
preferences smoke checks passed.

Phase 133 moves page-structure snapshots, insert/delete/clone operations, and
undo/redo application into `CanvasStateHistory.cpp` in that same model library.
The 21 moved method bodies are token-identical to Phase 132, and `CanvasState`
remains the sole document and history owner. The full default build, all 40
CTests, and the private Cage canvas, text, clipboard, selection, document, and
preferences smoke checks passed.

Phase 134 moves the laser-pointer lifecycle, drawing input, spline editing, and
current-stroke completion helpers into `CanvasStateInput.cpp`. All 20 moved
method bodies are token-identical to Phase 133; default arguments and input
state ownership remain on `CanvasState`. The full default build, all 40 CTests,
and the private Cage canvas, text, clipboard, selection, document, and
preferences smoke checks passed.

Phase 135 moves selection and vertical-space gesture handling, lasso edge
extension, selection clearing/deletion, and select-all methods into
`CanvasStateSelectionGestures.cpp`. All 15 moved method bodies are
token-identical to Phase 134, with selection state still owned by `CanvasState`.
The full default build, all 40 CTests, and the private Cage canvas, text,
clipboard, selection, document, and preferences smoke checks passed.

Phase 136 moves Smart Select, stroke/text/link/image picking, and object
selection into `CanvasStateObjectPicking.cpp`. All 7 moved method bodies are
token-identical to Phase 135; the selection model and hit-test behavior are
unchanged. The full default build, all 40 CTests, and the private Cage canvas,
text, clipboard, selection, document, and preferences smoke checks passed.

These user-requested follow-ups remain:
- Keep scrolling and zooming responsive on large documents. Phase 96 adds
  continuous Ctrl+touchpad zoom and reuses current page rasters while zoom
  input is active, then refreshes at the settled zoom. Phase 102 adds three
  configurable raster tiers: defaults are 4 full-detail pages, 12 medium pages
  (512 px and 32 MiB caps), and 128 low-detail pages (192 px and 32 MiB caps).
  Preferences can change each tier's page count, the medium/low memory budgets,
  the 35% overview and 18% low-detail zoom thresholds, and whether preloaded
  pages automatically follow the zoom threshold or always use medium/low detail.
  Full renders produce both lower tiers by downsampling; medium renders also
  produce a low snapshot. At overview zoom, full-detail PDF rendering stays on
  the pointer or active page, while other pages use cached snapshots or reduced-resolution
  background requests. Medium snapshots stay within a 1.5-viewport margin and
  low snapshots within a 6-viewport margin. Navigation preferences provide
  configurable page-preload offsets (3 pages before and 5 after by default);
  after a 150 ms settling delay, those pages queue medium or low-detail rasters
  after visible pages, favoring the current scroll direction. Sidebar previews
  use each PDF page's aspect ratio and a selected-page outline. A private GUI
  cache check is available; real-device touchpad behavior and long-run
  frame-time measurement remain outstanding. The private Cage check on the
  775-page PDF verified that the first offset page reaches the raster cache;
  fast-scroll previews settled on pages 25–27, and zoom-out reported 3 full,
  3 medium, and 11 low-detail cached pages.
- Verify and tune touchpad scrolling on the user's physical device. GTK4 now
  locks continuous gestures to their dominant axis, ignores horizontal drift
  in single-column layouts, damps deliberate horizontal movement in grids,
  clamps scroll and pan to the page-grid bounds, and applies release velocity
  as frame-clock momentum. Unit checks and a private Cage controller check use
  synthetic signals and do not establish physical-device sensitivity or
  momentum parity.
- Measure sidebar and canvas raster latency while rapidly scrolling the user's
  content-heavy PDF. The generated 10,000-page fixture verifies
  nearest-visible-first independent rendering and a synthetic 32-frame scroll,
  but not real-document latency under continued scrolling.
- Measure Smart Select drag frame time on annotation-heavy documents and reduce
  editable-stroke redraw cost if it misses a frame budget. Selection motion now
  skips full sidebar/search refreshes and keeps overview full-detail priority on
  the active page until release; a standalone debug Cairo run over 5,000
  128-point strokes measured about 421 ms for one full editable-ink draw.
  Phase 107 combines adjacent committed Pen/whiteout and Highlighter strokes
  into one cached draw-order run and appends new strokes to that raster without
  rescanning the earlier stroke geometry. Phase 110 adds an in-place path for
  same-bounds run extensions and a live raster for append-only active strokes;
  the 40-stroke pixel check confirms no prior-raster bytes are copied when
  bounds stay fixed. This targets the user's report that highlighter drawing
  becomes unusable after about 40 strokes, but does not establish interactive
  frame time on the user's PDF. Phase 113's private Cage check moved 40
  Highlighter strokes with 128 points each over 24 painted frames; its final
  software-renderer run measured 15.87 ms median, 18.17 ms p95, and 18.22 ms
  maximum. Phase 119 adds a private Cage run that loads a 775-page PDF, opens
  the Pages sidebar, drains its visible thumbnail queue, and draws 40 Pen plus
  40 Highlighter strokes through the GTK drag-controller path. Its
  software-renderer p95 was 16.68 ms for Pen, 18.05 ms for
  Highlighter, and 16.74 ms for Smart Select movement; the maximum ink frame
  was 19.17 ms. The append path copied about 9.7 MB for Pen and 10.1 MB for
  Highlighter. This did not reproduce the reported slowdown, but input samples
  remain synthetic and do not cover a physical pen or longer real-world
  sessions. Phase 126 repeated the run against the same PDF: Pen p95 was
  16.78 ms (24.95 ms max), Highlighter p95 was 17.79 ms (22.74 ms max), and
  Smart Select drag p95 was 16.35 ms. Phase 129 repeated the real-PDF run after
  removing the redundant per-page preview conversion: Pen p95 was 16.69 ms
  (20.12 ms max), Highlighter p95 was 18.29 ms (23.48 ms max), and Smart Select
  drag p95 was 17.48 ms (19.40 ms max). The 421 ms standalone debug measurement
  has not been repeated; editable-page snapshots and hardware-renderer
  measurements remain outstanding.
- A page-raster task previously failed as a whole if writing one of its optional
  medium/low-detail copies failed, discarding an already-rendered primary page.
  Optional tiers now fail independently and are omitted while the requested
  raster remains usable; primary write errors identify the tier, output path,
  Cairo status, and filesystem error when available. Parent-side worker errors
  also include the subprocess exit status or signal. A fault-injection test
  verifies that a blocked medium tier preserves full/low rasters and that a
  blocked primary output reports its tier/path. The original intermittent
  failure was not reproduced on the current machine. Phase 126 also opened the
  user's 775-page PDF in private Cage, rendered the initial sidebar thumbnail,
  and completed fast-scroll thumbnail rendering without a worker error. The
  private harness uses its own temporary directory, so the cause of the user's
  prior environment-specific write failures remains unconfirmed.
- Phase 122 centralizes GTK4 canvas cursors around the active tool and pointer
  action. The private Cage `--cursor-only` check reads the mapped widget cursor
  after synthetic motion over editable text, selection move/resize/rotate
  handles, erasing, and temporary/active pan. It does not verify cursor-theme
  artwork on the user's desktop or physical input-device routing.
- Phase 123 aligns the blue frame during rectangular PDF text-selection drags
  with the outward-rounded bounds used for the shaded preview. The pure geometry
  test covers reversed fractional bounds, and the private Cage toolbar check
  captures the active drag and asserts that its preview and frame extents match.

## Default UI target

Ship a compact, canvas-first default inspired by the established Xournal++
workspace: use the Adw header, one tool-and-page control strip, and a narrow
left rail for frequent tools and color swatches. Start with a dark charcoal
canvas surround and the page sidebar closed. Put infrequent actions in menus or
secondary tool sections. Keep Smart Select easy to find and preserve its native
text, PDF, object, and modifier behavior. Keep toolbar contents and placement
user-configurable, including the existing customization paths, as the visual
design evolves. Keep frequent main tools on a persistent, reorderable rail,
with Smart Select directly available by default; secondary tools can be grouped
without removing or disabling them.

## Master tools

The tool names below come from `src/core/control/ToolEnums.h` and the handler
registration in `ToolHandler::initTools`.

| Master tool | GTK 4 status | Remaining parity work |
| --- | --- | --- |
| `pen` | Partial | Photoshop-style Shift constraints lock freehand and shape-recognizer strokes to the dominant horizontal or vertical axis, retain the axis across successive strokes until Shift release, and project samples before stabilization. Shift-click can connect from the previous Pen or Highlighter endpoint when that stroke remains on the same page, layer, and tool, provided Shift stays held through pointer release. Solid, dash, dash-dot, dot, and custom `cust:` styles are selectable, rendered for constant-width and pressure strokes, editable on selected Pen strokes, persisted through GTK 4 settings, native save/reopen, and the internal clipboard. Exact production cap geometry, blend modes, shape objects, and remaining stroke-renderer details remain. Physical tablet behavior is not verified. |
| `eraser` | Partial | Standard split, whiteout, and delete-stroke modes work. All three support the Pen-style Shift axis lock and same-page/layer/mode Shift-click continuation; the connection is applied only if Shift remains held through pointer release. Standard/Delete Stroke erase along a sampled connecting segment in one undo action, while Whiteout creates an editable connecting stroke only if its previous endpoint still exists. Reproduce all production hit geometry and renderer interactions. |
| `highlighter` | Partial | Core opacity and native serialization work; shares the Pen Shift constraint and endpoint connection behavior. Reproduce production stroke geometry and blend behavior. |
| `text` | Partial | Rich editing, toolbar font/size/style/color controls, direct Text-tool drag-to-move and handle transforms, rectangle/lasso selection, cross-page transfer, and native text/link clipboard copy/cut/paste/duplicate work. Formatting runs and link URLs survive; mixed stroke/text/image selections preserve stacking order through native, PNG, SVG, and plain-text clipboard targets. The editor follows transformed text; IME and accessibility behavior remain. |
| `image` | Partial | Insert, move, resize, rotate, delete, native save/reopen, PDF export, and editable native clipboard copy/cut/paste work. Raster, LaTeX, and mixed-object groups preserve native image data and relative transforms; PNG and SVG carry the rendered selection. Unsupported native object editing remains. |
| `selectRect` | Partial | Selects editable strokes, text, and images on the active visible foreground layer; unsupported native objects are not included. |
| `selectRegion` | Partial | Lasso-selects editable strokes, text, and images on the active visible foreground layer; unsupported native objects are not included. |
| `selectMultiLayerRect` | Partial | Selects editable strokes, text, and images from the topmost visible foreground layer with matches; unsupported native objects are not included. |
| `selectMultiLayerRegion` | Partial | Lasso-selects editable strokes, text, and images from the topmost visible foreground layer with matches; unsupported native objects are not included. |
| `selectObject` | Partial | Select Object picks the closest editable stroke, text, or image on the active visible layer within the master 0.5 pt action radius, using native stroke caps/joins and transformed text/image bounds; exact overlapping hits prefer the frontmost object, and Shift adds to the current selection. Unsupported native object types remain. |
| `playObject` | Partial | Audio-linked strokes and rectangular objects can be played on the active layer within the native 15 pt action radius. Playback controls, the non-audio faded preview, and read-only `audioFolder`, `defaultSeekTime`, and `disableAudio` imports are implemented. Port audio recording and device preferences, verify physical input, and complete media persistence/reopen behavior across session boundaries. |
| `verticalSpace` | Partial | The selection-menu mode previews and shifts editable strokes, text, and images wholly above or below a dragged boundary on the active layer; Control switches sides, page-grid snapping applies, and the movement is one undoable edit. Unsupported native objects are not editable, and physical device behavior remains unverified. |
| `hand` | Partial | Dedicated Hand mode and Space/stylus temporary pan preserve the current selection. Continuous touchpad scroll locks to the dominant axis; horizontal motion is enabled only when the scaled page grid exceeds the viewport, requires 2:1 horizontal intent, and is damped to 18%. Scroll/pan stays inside the grid, and GTK release velocity drives a shorter decaying momentum coast. Unit and private Cage checks use synthetic signals; physical device tuning, touch gesture details, and platform routing remain. |
| `drawRect` | Partial | Drag preview, constraints, and undo work as an editable stroke path; preserve native geometric objects. |
| `drawEllipse` | Partial | Drag preview, constraints, and undo work as an editable stroke path; preserve native geometric objects. |
| `drawArrow` | Partial | Arrow path drawing works; match native geometry and styling. |
| `drawDoubleArrow` | Partial | Double-arrow path drawing works; match native geometry and styling. |
| `drawCoordinateSystem` | Partial | Coordinate-axis paths work; match native geometry and production modifiers. |
| `showFloatingToolbox` | Partial | A GTK4 floating panel opens at the pointer, clamps to the canvas, closes on pointer leave or tool choice, and works from canvas mode or a stylus mapping. Smart Select is pinned in the main left rail, while secondary tools are grouped under More tools. Four `toolbarFloat` rows import read-only from `toolbar.ini`, can be edited in Preferences > Toolbar, and persist separately in `gtk4-canvas.ini`; common pen/highlighter, laser/PDF, undo/redo, zoom, size, palette, six rich-text formatting actions, left/center/right alignment, justification, registered Lua plugin (`Plugin::ID`) actions, and plugin placeholders render. All eight master drawing-mode actions (`RULER`, rectangle, ellipse, arrow, double arrow, coordinate system, spline, and stroke recognizer) select their corresponding mode. The `DRAW` combo token offers a synchronized selector for Freehand and all eight drawing modes. Plugin actions use their registered label and available themed icon, then invoke the Lua callback. `app.registerPlaceholder` adds described labels, `app.setPlaceholderValue` updates them live, and empty values display the placeholder ID. `TOOL_FILL` toggles per-tool Pen/Highlighter fill, applies configured opacity to new strokes, updates selected strokes with undo, and persists in `gtk4-canvas.ini`; `FILL_OPACITY` opens a modal live-preview chooser for the active Pen or Highlighter, applies 5–100% opacity, and persists each value in `gtk4-canvas.ini`. `app.activateAction("fill-opacity")` opens that chooser. `PAGE_SPIN` provides a debounced one-based page spinner synchronized with current page, page count, and the mapped source PDF page when available. `LAYER` displays the active layer and opens grouped foreground-layer selection, background/foreground visibility toggles, Show/Hide All, and Add Above/Below actions; adding below is undoable. Other remaining built-in tokens, mouse-button mappings, stroke-filter tap triggers, the legacy drag/drop/configuration workflow, and exact placement/dismiss/reopen behavior remain. |
| `drawSpline` | Partial | Knot, tangent, preview, and editing behavior work; verify generated geometry against the production handler. |
| `selectPdfTextLinear` | Partial | Linear drag, word/line click selection, copy, and PDF annotation actions work against the original PDF source. The action toolbar uses release-point placement with edge flipping and clamping, hides on input/zoom while retaining the selection, and reopens when selected text is clicked. Mouse checks use synthetic events; physical device routing remains unverified. |
| `selectPdfTextRect` | Partial | Rectangular drag, word/line click selection, copy, and PDF annotation actions work against the original PDF source. The action toolbar uses release-point placement with edge flipping and clamping, hides on input/zoom while retaining the selection, and reopens when selected text is clicked. Mouse checks use synthetic events; physical device routing remains unverified. |
| `laserPointerPen` | Partial | Transient pressure-aware laser trails use the master presets and imported fade delay; completed marks fade after the 500 ms default delay in 50 ms steps of 25/255 alpha, resume at full alpha when another stroke starts, and never enter save or undo history. Physical stylus routing and exact raster-mask behavior across zoom/visibility changes remain unverified. |
| `laserPointerHighlighter` | Partial | Transient pressure-independent highlighter trails use the master presets and imported fade delay; completed marks share the per-page trail and fade lifecycle without entering save or undo history. Physical stylus routing and exact raster-mask behavior across zoom/visibility changes remain unverified. |
| `link` | Partial | The Link tool creates and edits native links, renders them underlined, hit-tests them on the active visible layer, opens them with Ctrl-click, and preserves URL/font/layout through save and undo/redo. Match the production editor and scheme choices, verify external launcher behavior, and test physical input routing. |
| `latex` | Partial | The GTK4 LaTeX tool compiles the configured source with the selected generator/template, previews vector PDF output, inserts formulas, edits formulas and plain text in place, and preserves PDF bytes, source, native dimensions, transform, and one-step undo/redo through XOPP save/reopen. Smart Select double-click also opens a formula. Preferences expose and persist every legacy LaTeX setting; GtkSourceView 5 provides optional syntax highlighting, style schemes, indentation and line numbers, and external-editor mode waits for the configured editor before recompiling its source. A setup check compiles the default formula without changing the document. The default rail keeps LaTeX under More tools. Remaining: verify missing-generator/invalid-source recovery and physical stylus routing. The imported `autoCheckDependencies` setting is persisted, but current master has no controller use for it. |
| `smartSelect` | Partial | Picks editable strokes, text, and images within the native 0.5-point action radius using stroke caps/joins and transformed rectangular bounds; stroke bounds reject distant candidates before segment-distance checks, and candidate order is ranked without a per-click sort. Text clicks open the editor, PDF-text drags delegate to linear PDF selection after the native 10 px threshold, Alt-drag forces that route, Shift-drag starts rectangle selection, moves snap the nearest bounds corner to the page grid, resize handles snap the moving edge axes, and both honor the Alt override. Dragging a selection past the viewport edge pans at the configured legacy rate and multiplier. After a committed rotation, the frame and handles rotate with the selection and later resizes use the accumulated rotation for their scale axes; undo and redo preserve this state. Unsupported native object support and physical input validation remain. |

`DrawingType` also includes `default`, `line`, `rectangle`, `ellipse`,
`arrow`, `doubleArrow`, `drawCoordinateSystem`, `strokeRecognizer`, and
`spline`. These drawing modes are implemented as ordinary stroke paths in the
prototype; native shape objects and full recognizer behavior remain pending.
`GeometryToolType` adds the physical setsquare and compass controllers, which
are not implemented in the prototype.

## Object editing and selection

The modern object-editing contract spans selection, clipboard, and transforms;
it is not represented by a single `ToolType`.

| Behavior | GTK 4 status | Remaining parity work |
| --- | --- | --- |
| Rectangle/lasso object selection | Partial | Active-layer and visible-layer modes select editable strokes, text, and images. Multi-layer mode searches top to bottom, skips hidden layers, and selects every matching object from the first visible layer with matches; unsupported native objects remain outside general selection. Rectangular objects require all four transformed corners inside the selector, matching master behavior. |
| Stroke/text/image move, resize, mirror, rotate | Partial | Shared undoable transforms work for ink, text, and images, including nearest-corner grid snapping during moves, axis-aware page-grid snapping during resize, configured edge-pan while moving beyond the viewport, direct Text-tool text movement, resize immediately after editing, mixed-object edits, and cross-page transfer. Text and image selections draw the shared resize and rotation handles. After rotation the visible frame and handles rotate with the selection while later resize transforms use the accumulated rotation; undo and redo restore both. Physical input behavior remains. |
| Text editing | Partial | Inline rich-text editing, left/center/right alignment, justification, and toolbar font/size/style/color controls work; the Text tool directly moves and transforms text, opens existing text on click or double-click, and selects the clicked word on double-click. Smart Select single-click selects objects and double-click edits native text while preserving the current canvas zoom. The inline editor follows the text item's affine transform, and transformed word picking maps the pointer through the inverse matrix. New unwrapped text editors request a compact 40 px width and resize from the live Pango layout; wrapped editors use their configured width. The caret uses the accent color and a 0.5 aspect ratio only while editing, restoring the prior GTK setting afterward. Newly created or edited text remains selected. Alignment and justification defaults import from legacy tool settings and persist in the GTK 4 overlay. |
| Layer selection and operations | Partial | Add/delete/merge/duplicate/rename/reorder/visibility and visible-layer stroke/text/image selection work. The master model has no layer-lock action. |
| Native object picking | Partial | Smart Select and the distinct Select Object tool pick editable strokes, images, text, and links; Select Object uses nearest-hit selection and Shift-add. Unsupported native objects cannot be edited. |
| Clipboard | Partial | Bounded `application/xournal` streams preserve ordered groups of supported strokes, rich text, links, raster images, and LaTeX images, including dash arrays in the internal codec. Copy also offers rendered PNG and SVG, plus plain text when text is selected; external plain-text paste creates an editable text item. Mixed copy/cut/paste/duplicate and cross-type paste/undo are grouped edits. Unsupported native types, audio attachments, and cross-process compatibility verification remain. |
| PDF text selection and annotation | Partial | Linear and rectangular drag select PDF text; double/triple click selects words/lines. Copy, highlight, underline, and strikethrough work with color controls and marker opacity. The original PDF source is used for text extraction and retained in the native save path. The action toolbar opens beside the release point with edge flipping/clamping, hides on pointer input and zoom while retaining the selection, reopens when selected text is clicked, and clears when the user clicks outside a page. Phase 127's private Cage check runs without the GTK 3 worker and passes PDF selection, toolbar placement/dismissal/reopen, clipboard, annotations, undo/redo, and native PDF-backed XOPP save. Physical device routing remains unverified. |

## Built-in plugin inventory

The master repository bundles these directories under `plugins/`. The prototype
now discovers plugin manifests, loads enabled Lua scripts, dispatches registered
menu callbacks and canvas shortcuts, and offers a manager that persists enabled
plugin paths. The host implements only part of the master Lua API, so most
plugins remain pending. `Example` is a reference plugin; it stays listed because
it is part of the master plugin distribution.

| Plugin | Observable behavior | GTK 4 status |
| --- | --- | --- |
| `BeamerPresentation` | Navigate animated Beamer PDFs and remove animation pages. | Covered — next/previous advance the PDF background when adjacent PDF page labels match and navigate between document pages at slide boundaries; cleanup removes repeated-label pages. The actions persist through native save/reopen. |
| `ColorCycle` | Cycle through a configured color list. | Covered |
| `Example` | Demonstrate the plugin API. | Covered — manifest loading, menu registration, `app.openDialog`, and its asynchronous button-response callback work and are regression-tested. |
| `Export` | Export documents to PDF, SVG, and PNG. | Covered — the three bundled menu actions include current unsaved edits; PDF supports page/layer ranges, background choice, progressive mode, and backend selection; SVG/PNG preserve master page-range filenames, layer/background options, and PNG DPI/width/height priority. Worker, Lua option, and private Cage menu checks cover outputs and failure safety. |
| `FitToContent` | Resize page content to fit a selection or layer. | Covered — both menu actions read stroke geometry, hide the source layer, add and name a destination layer, move a copy into page-relative coordinates, and resize the page. Grouped undo/redo covers each host operation, and save/reopen retains the resized dimensions and copied strokes. |
| `HighlightPosition` | Toggle a position highlight using a shortcut. | Covered — Alt+X toggles the pointer halo; the fill/border color, radius, and enabled state are editable in Preferences and persist. |
| `ImageActions` | Invert, resize, flip, rotate, make white pixels transparent, trim/crop, and split selected images into rows or columns. | Covered — all ten upstream menu actions run through the GTK 4 host's optional libvips pipeline; grouped replacement, selection, undo/redo, and save/reopen are regression-tested. |
| `LayerActions` | Clone layers to following pages, hide layers, and add top layers. | Covered — clone checks for a following PDF page, asks before replacing annotations, copies the source page's foreground layers and editable objects onto that PDF page, and matches the master page-copy visibility reset. Undo restores the replaced page and then removes the clone; redo restores the clone and then removes the replaced page. Native page-history snapshots preserve unsupported objects and layer structure while canvas history restores supported editable objects. Hide selects the first foreground layer, hides higher foreground layers, and preserves each page's background visibility; add creates a top layer on every page with one undo step per page. Worker and private Cage checks cover the confirmation, clone, save/reopen, undo/redo, hide, and add paths. |
| `QuickScreenshot` | Capture a screen region to an image using supported platform utilities. | Covered — launches `grim` + `slurp` on Wayland or `scrot`/`maim` on X11 without a shell; opens the GTK 4 Save dialog, or copies PNG to the clipboard when the X11 `maim`/`xclip` setup is present. Cancel and failure paths remove temporary files. The Lua runtime and private Cage checks exercise menu dispatch, the async Wayland capture/save path, and PNG clipboard publication. |
| `SpaceForNotes` | Add empty pages for notes. | Covered — both upstream modes run: add a lined page after each PDF page that is not already followed by a note page, or force a page after every page. Each insertion participates in chronological undo/redo, and changes survive save/reopen. |
| `ToggleGrid` | Toggle the page grid. | Covered — toggles grid snapping and switches the active page between plain and graph backgrounds; native document save, reopen, undo, and redo preserve the background. |
| `ToolShortcuts` | Add canvas-scoped tool and page-navigation shortcuts. | Partial — bundled shortcuts select common tools including Smart Select, navigate pages, open Go to Page, invoke undo/redo, and open GTK 4 document Search. Search highlights original PDF and visible native text matches and wraps next/previous navigation across pages. Some other master actions remain missing. |

## Cross-cutting application behavior

| Area | GTK 4 status | Remaining parity work |
| --- | --- | --- |
| Preferences and legacy settings | Partial | Legacy `settings.xml` and `.gpl` inputs remain read-only; supported edits persist in the GTK 4 overlay, including page-grid snapping, selection edge-pan speed/multiplier, spacing, tolerance, pointer highlight enable/color/radius/border, laser tool colors/sizes, laser fade delay, stylus mappings, text alignment/justification defaults, audio playback folder/seek/disable settings, all LaTeX preferences, and editable floating-toolbar rows that can invoke registered plugin toolbar actions and placeholders. The main tool rail supports drag/drop ordering and add/remove between the visible rail and More tools; Smart Select stays pinned. Audit per-device `deviceClasses`, the manual display-DPI override, audio device/recording controls, unsupported built-in toolbar actions, and the legacy drag/drop customization workflow for floating toolbars. |
| Lua plugin runtime and manager | Partial | Discovers bundled, explicit `--plugin-dir`, and per-user plugin paths; reads `plugin.ini`, loads enabled Lua 5.4 scripts, exposes a Plugins menu and manager, persists enabled-path overrides, and dispatches registered menu actions, accelerators, and canvas shortcuts. `app.C` tool IDs, `app.registerUi` menu/shortcut registration, asynchronous `app.openDialog`, `app.export` with PDF/SVG/PNG options, `app.quickScreenshot`, and the complete `app.getDocumentStructure()` page/layer/background-color/PDF-path shape work. Registered `app.registerUi` toolbar IDs can be placed in editable floating-toolbox rows as `Plugin::ID`; enabled plugin buttons use the registered label/icon and invoke their Lua callback. `app.registerPlaceholder` adds described labels to those rows; `app.setPlaceholderValue` updates a label by bare or `Plugin::`-prefixed ID, and an empty value displays the ID. `app.getDisplayDpi` reports the active monitor's physical DPI, clamped to 36–720 with a 72-DPI fallback when physical dimensions are unavailable; the manual legacy override remains unported. `app.getFolder` creates per-plugin config, data, and state directories under the XDG user roots. `app.getZoom` and `app.setZoom` read and change zoom around the viewport center, leave fit mode, and use the canvas zoom limits. `app.setCurrentPage` clamps one-based page numbers and changes the active page without scrolling. `app.getPageLabel` returns a one-based PDF page label or `nil, message` when the page is out of range. `app.getColorPalette` returns ordered palette entries with names and 24-bit RGB colors. `app.refreshPage` invalidates page/layer render and thumbnail caches and redraws without adding undo history. `app.getActionState` reads zoom, selected tool, grid snapping, position highlighting, the active tool color in legacy ARGB form, active and per-tool size presets (`ToolSize_none` for custom or unsupported widths), eraser mode, and `tool-pen-line-style` for the Pen tool or selected Pen strokes (including mixed-style `none`); `app.changeActionState("tool-pen-line-style", style)` applies named and custom patterns to the active Pen or selected Pen strokes; `tool-fill` reads and changes fill state, while other action states remain unsupported. `app.getToolInfo` returns the upstream active, Pen, Highlighter, Eraser, Text, and Selection query shapes; active reports its line style, Pen reports its named/custom line style, and Highlighter omits the Pen-only line-style field; Pen and Highlighter include their fill-enabled state and opacity. Selection `boundingBox`, `originalBounds`, and `snappedBounds` currently use the GTK4 selection rectangle; native padding and distinct source/snapping bounds remain to port. `app.getFonts`, `app.getFont`, and `app.setFont` list installed families, read the default family/size, and accept Pango descriptions or partial family/size tables; changes update defaults, apply to selected text or the active editor, and selected text changes are undoable. `app.setCurrentLayer(layerNr, change_visibility)` selects a foreground layer, clears selection, and optionally shows layers through that ID while hiding higher layers without changing background visibility or creating undo history. Layer ID zero retains the upstream all-hidden visibility edge case while the GTK model keeps its first foreground layer selected internally. `app.setBackgroundName` directly updates the current page background name with the upstream no-undo, no-dirty-state semantics. `app.getScrollPos` reports the canvas position and viewport dimensions; `app.scrollToPage` clamps absolute and relative page navigation; `app.scrollToPos` supports relative offsets and absolute coordinates in canvas pixels. Color changes, selected-stroke/text recoloring, common tool selection, grid snapping, position highlighting, and fill state, current-page plain/graph background changes, batched empty-page insertion, ImageActions transforms, page navigation, Go to Page, undo/redo, and default-tool actions work. `app.getStrokes`, `app.getTexts`, `app.getLinks`, and `app.getImages` query editable objects from the selection, current layer, current page, or whole document with the upstream fields and one-based page/layer metadata. Opaque element references survive vector growth and value-semantic edits; `app.addToSelection` resolves current-layer references and `app.clearSelection` releases the selection. `app.addStrokes` accepts named/custom line styles per stroke; `app.addStrokes`, `app.addSplines`, `app.addTexts`, `app.addLinks`, and `app.addImages` create editable objects on the current layer and return opaque references. `app.addSplines` flattens cubic segments with the master's adaptive tolerance, applies per-stroke styling, and shares stroke undo policies. Image insertion accepts file paths or binary image data, preserves the master sizing/aspect-ratio and page-fit behavior, and returns per-item soft errors. Text and link defaults use the configured font, size, and color; link alignment constants are available as `app.C.Alignment_*`. Grouped and individual batches preserve selection through undo/redo; no-undo stroke/text/link/image batches create an irreversible revision boundary and still mark the document dirty. Page resizing, layer visibility/name changes, and the FitToContent layer action are covered. Unsupported native objects remain outside these editable APIs. Port the other master Lua APIs, remaining image/object mutation, plugin-specific actions and state, and remaining bundled plugins. |
| Page layout | Partial | Vertical and horizontal flow, fixed row/column grids, paired spreads and offsets, and right-to-left/bottom-to-top reading directions use the upstream page-to-grid mapping and persist in GTK 4 Preferences. Legacy layout settings import into the same model. Presentation Mode (F5) forces vertical single-page flow, fits and centers each active page with the upstream 14 pt allowance, locks zoom/layout changes, and supports Page Up/Down or unmodified arrows/HJKL navigation. Fullscreen (F11) and default/fullscreen/presentation visibility presets are editable and persist. A private three-page GUI check verifies these transitions and settings. Worker-backed visual review with multi-page PDFs remains. |
| Page editing history | Covered | Manual page insertion/deletion, SpaceForNotes insertion, and LayerActions clone/replacement share chronological undo/redo. Deleted or replaced pages restore from native one-page snapshots with their PDF mapping, layer structure, and unsupported native content; canvas history restores supported editable objects. LayerActions undo restores the replaced page before removing the clone, and redo restores the clone before removing the replaced page. |
| Input devices | Partial | Continuous touchpad scrolling locks to the dominant axis; horizontal movement is enabled only when the scaled page grid exceeds the viewport, requires 2:1 horizontal intent, and is damped to 18%. Scroll and pan clamp to the true page-grid edges. GTK release velocity drives momentum capped at 1 logical pixel/ms with a 220 ms decay constant. Discrete mouse-wheel detents retain their 35-pixel step. Ctrl+touchpad zoom remains fractional, pointer-anchored, and cache-previewed. Unit and private Cage checks cover synthetic controller signals; physical sensitivity and momentum tuning remain. Production device identity, physical stylus, touch drawing, hand gestures, and platform-specific routing still need verification or porting. |
| Native documents | Partial | Supported strokes, text, links, and image suffixes round-trip. PDF-backed text selection uses the original PDF source and page mapping. Make unsupported native object types editable without losing preservation. |
| Accessibility and platform behavior | Pending | Port accessibility semantics and verify each supported platform and packaging path. |

## Explicit exclusions

None recorded. Before excluding anything, add its source identifier, the reason
it is obsolete, and the decision that authorizes removal. Update this section,
the behavior ledger, and the affected regression checks together.
