# GTK 4 migration feature audit

This audit tracks observable behavior from canonical Xournal++ master against
the separate GTK 4/libadwaita prototype. Source snapshot: `upstream/master`
`b2ef6316989d379cfcb6e1048363a0b6c2befa6b`, fetched on 2026-10-25. Update the
status and source reference as each migration slice lands.

Current ledger count: **53 task areas — 43 Partial, 1 Pending, 9 Covered, and 0
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
| `pen` | Partial | Photoshop-style Shift constraints lock freehand and shape-recognizer strokes to the dominant horizontal or vertical axis, retain the axis across successive strokes until Shift release, and project samples before stabilization. Shift-click can connect from the previous Pen or Highlighter endpoint when that stroke remains on the same page, layer, and tool. Match the full renderer's caps, fills, dashes, blend modes, and native stroke styles; physical tablet behavior is not verified. |
| `eraser` | Partial | Standard split, whiteout, and delete-stroke modes work; reproduce all production hit geometry and renderer interactions. |
| `highlighter` | Partial | Core opacity and native serialization work; shares the Pen Shift constraint and endpoint connection behavior. Reproduce production stroke geometry and blend behavior. |
| `text` | Partial | Rich editing, toolbar font/size/style/color controls, direct Text-tool drag-to-move and handle transforms, rectangle/lasso selection, cross-page transfer, and native text/link clipboard copy/cut/paste/duplicate work. Formatting runs and link URLs survive; mixed stroke/text/image selections preserve stacking order through native, PNG, SVG, and plain-text clipboard targets. Transform-following editor placement, IME, and accessibility remain. |
| `image` | Partial | Insert, move, resize, rotate, delete, native save/reopen, PDF export, and editable native clipboard copy/cut/paste work. Raster, LaTeX, and mixed-object groups preserve native image data and relative transforms; PNG and SVG carry the rendered selection. Unsupported native object editing remains. |
| `selectRect` | Partial | Selects editable strokes, text, and images on the active visible foreground layer; unsupported native objects are not included. |
| `selectRegion` | Partial | Lasso-selects editable strokes, text, and images on the active visible foreground layer; unsupported native objects are not included. |
| `selectMultiLayerRect` | Partial | Selects editable strokes, text, and images from the topmost visible foreground layer with matches; unsupported native objects are not included. |
| `selectMultiLayerRegion` | Partial | Lasso-selects editable strokes, text, and images from the topmost visible foreground layer with matches; unsupported native objects are not included. |
| `selectObject` | Partial | Select Object picks the closest editable stroke, text, or image on the active visible layer within the master 0.5 pt action radius, using native stroke caps/joins and transformed text/image bounds; exact overlapping hits prefer the frontmost object, and Shift adds to the current selection. Unsupported native object types remain. |
| `playObject` | Partial | Audio-linked strokes and rectangular objects can be played on the active layer within the native 15 pt action radius. Playback controls, the non-audio faded preview, and read-only `audioFolder`, `defaultSeekTime`, and `disableAudio` imports are implemented. Port audio recording and device preferences, verify physical input, and complete media persistence/reopen behavior across session boundaries. |
| `verticalSpace` | Partial | The selection-menu mode previews and shifts editable strokes, text, and images wholly above or below a dragged boundary on the active layer; Control switches sides, page-grid snapping applies, and the movement is one undoable edit. Unsupported native objects are not editable, and physical device behavior remains unverified. |
| `hand` | Partial | Dedicated Hand mode and Space/stylus temporary pan preserve the current selection. Physical device routing, touch gesture details, and kinetic scrolling remain. |
| `drawRect` | Partial | Drag preview, constraints, and undo work as an editable stroke path; preserve native geometric objects. |
| `drawEllipse` | Partial | Drag preview, constraints, and undo work as an editable stroke path; preserve native geometric objects. |
| `drawArrow` | Partial | Arrow path drawing works; match native geometry and styling. |
| `drawDoubleArrow` | Partial | Double-arrow path drawing works; match native geometry and styling. |
| `drawCoordinateSystem` | Partial | Coordinate-axis paths work; match native geometry and production modifiers. |
| `showFloatingToolbox` | Partial | A GTK4 floating panel opens at the pointer, clamps to the canvas, closes on pointer leave or tool choice, and works from canvas mode or a stylus mapping. Smart Select is pinned in the main left rail, while secondary tools are grouped under More tools. Four `toolbarFloat` rows import read-only from `toolbar.ini`, can be edited in Preferences > Toolbar, and persist separately in `gtk4-canvas.ini`; common pen/highlighter, laser/PDF, undo/redo, zoom, size, drawing-mode, palette, six rich-text formatting actions, left/center/right alignment, justification, registered Lua plugin (`Plugin::ID`) actions, and plugin placeholders render. Plugin actions use their registered label and available themed icon, then invoke the Lua callback. `app.registerPlaceholder` adds described labels, `app.setPlaceholderValue` updates them live, and empty values display the placeholder ID. `TOOL_FILL` and other unsupported tokens remain disabled. Complete built-in token coverage, mouse-button mappings, stroke-filter tap triggers, the legacy drag/drop/configuration workflow, and exact placement/dismiss/reopen behavior remain. |
| `drawSpline` | Partial | Knot, tangent, preview, and editing behavior work; verify generated geometry against the production handler. |
| `selectPdfTextLinear` | Partial | Linear drag, word/line click selection, copy, and PDF annotation actions work against the original PDF source. Match the GTK 3 floating toolbox placement and dismissal/reopen behavior. |
| `selectPdfTextRect` | Partial | Rectangular drag, word/line click selection, copy, and PDF annotation actions work against the original PDF source. Match the GTK 3 floating toolbox placement and dismissal/reopen behavior. |
| `laserPointerPen` | Partial | Transient pressure-aware laser trails use the master presets and imported fade delay; completed marks fade after the 500 ms default delay in 50 ms steps of 25/255 alpha, resume at full alpha when another stroke starts, and never enter save or undo history. Physical stylus routing and exact raster-mask behavior across zoom/visibility changes remain unverified. |
| `laserPointerHighlighter` | Partial | Transient pressure-independent highlighter trails use the master presets and imported fade delay; completed marks share the per-page trail and fade lifecycle without entering save or undo history. Physical stylus routing and exact raster-mask behavior across zoom/visibility changes remain unverified. |
| `link` | Partial | The Link tool creates and edits native links, renders them underlined, hit-tests them on the active visible layer, opens them with Ctrl-click, and preserves URL/font/layout through save and undo/redo. Match the production editor and scheme choices, verify external launcher behavior, and test physical input routing. |
| `latex` | Partial | The GTK4 LaTeX tool compiles the configured source with the selected generator/template, previews vector PDF output, inserts formulas, edits formulas and plain text in place, and preserves PDF bytes, source, native dimensions, transform, and one-step undo/redo through XOPP save/reopen. Smart Select double-click also opens a formula. Preferences expose and persist every legacy LaTeX setting; GtkSourceView 5 provides optional syntax highlighting, style schemes, indentation and line numbers, and external-editor mode waits for the configured editor before recompiling its source. A setup check compiles the default formula without changing the document. The default rail keeps LaTeX under More tools. Remaining: verify missing-generator/invalid-source recovery and physical stylus routing. The imported `autoCheckDependencies` setting is persisted, but current master has no controller use for it. |
| `smartSelect` | Partial | Picks editable strokes, text, and images within the native 0.5-point action radius using stroke caps/joins and transformed rectangular bounds; text clicks open the editor, PDF-text drags delegate to linear PDF selection after the native 10 px threshold, Alt-drag forces that route, Shift-drag starts rectangle selection, moves snap the nearest bounds corner to the page grid, resize handles snap the moving edge axes, and both honor the Alt override. Dragging a selection past the viewport edge pans at the configured legacy rate and multiplier. After a committed rotation, the handle frame remains axis-aligned and later resizes use the accumulated rotation for their scale axes; undo and redo preserve this state. Unsupported native object support and physical input validation remain. |

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
| Stroke/text/image move, resize, mirror, rotate | Partial | Shared undoable transforms work for ink, text, and images, including nearest-corner grid snapping during moves, axis-aware page-grid snapping during resize, configured edge-pan while moving beyond the viewport, direct Text-tool text movement, resize immediately after editing, mixed-object edits, and cross-page transfer. After rotation the visible frame stays axis-aligned while later resize transforms use the accumulated rotation; undo and redo restore both. Transform-following text editor placement and device behavior remain. |
| Text editing | Partial | Inline rich-text editing, left/center/right alignment, justification, and toolbar font/size/style/color controls work; the Text tool directly moves and transforms text, and newly created or edited text remains selected. Alignment and justification defaults import from legacy tool settings and persist in the GTK 4 overlay. The editor overlay does not follow transforms. |
| Layer selection and operations | Partial | Add/delete/merge/duplicate/rename/reorder/visibility and visible-layer stroke/text/image selection work. The master model has no layer-lock action. |
| Native object picking | Partial | Smart Select and the distinct Select Object tool pick editable strokes, images, text, and links; Select Object uses nearest-hit selection and Shift-add. Unsupported native objects cannot be edited. |
| Clipboard | Partial | Bounded `application/xournal` streams preserve ordered groups of supported strokes, rich text, links, raster images, and LaTeX images. Copy also offers rendered PNG and SVG, plus plain text when text is selected; external plain-text paste creates an editable text item. Mixed copy/cut/paste/duplicate and cross-type paste/undo are grouped edits. Unsupported native types, audio attachments, nonempty stroke dash styles, and cross-process compatibility verification remain. |
| PDF text selection and annotation | Partial | Linear and rectangular drag select PDF text; double/triple click selects words/lines. Copy, highlight, underline, and strikethrough work with color controls and marker opacity. The original PDF source is used for text extraction and retained in the native save path. Match GTK 3 floating-toolbox placement and dismissal/reopen behavior. |

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
| `LayerActions` | Clone layers to following pages, hide layers, and add top layers. | Partial — clone checks for a following PDF page, asks before replacing annotations, then copies the source page's foreground layers and editable objects onto that PDF page; it matches the master page-copy visibility reset. Hide selects the first foreground layer, hides higher foreground layers, and preserves each page's background visibility; add creates a top layer on every page with one undo step per page. Whole-page clone/replace is not undoable yet. |
| `QuickScreenshot` | Capture a screen region to an image using supported platform utilities. | Covered — launches `grim` + `slurp` on Wayland or `scrot`/`maim` on X11 without a shell; opens the GTK 4 Save dialog, or copies PNG to the clipboard when the X11 `maim`/`xclip` setup is present. Cancel and failure paths remove temporary files. The Lua runtime and private Cage checks exercise menu dispatch, the async Wayland capture/save path, and PNG clipboard publication. |
| `SpaceForNotes` | Add empty pages for notes. | Partial — both upstream modes run: add a lined page after each PDF page that is not already followed by a note page, or force a page after every page. Changes survive save/reopen; page insertions are not undoable yet. |
| `ToggleGrid` | Toggle the page grid. | Covered — toggles grid snapping and switches the active page between plain and graph backgrounds; native document save, reopen, undo, and redo preserve the background. |
| `ToolShortcuts` | Add canvas-scoped tool and page-navigation shortcuts. | Partial — bundled shortcuts select common tools including Smart Select, navigate pages, open Go to Page, invoke undo/redo, and open GTK 4 document Search. Search highlights original PDF and visible native text matches and wraps next/previous navigation across pages. Some other master actions remain missing. |

## Cross-cutting application behavior

| Area | GTK 4 status | Remaining parity work |
| --- | --- | --- |
| Preferences and legacy settings | Partial | Legacy `settings.xml` and `.gpl` inputs remain read-only; supported edits persist in the GTK 4 overlay, including page-grid snapping, selection edge-pan speed/multiplier, spacing, tolerance, pointer highlight enable/color/radius/border, laser tool colors/sizes, laser fade delay, stylus mappings, text alignment/justification defaults, audio playback folder/seek/disable settings, all LaTeX preferences, the customizable main tool rail, and editable floating-toolbar rows that can invoke registered plugin toolbar actions and placeholders. Audit per-device `deviceClasses`, the manual display-DPI override, audio device/recording controls, unsupported built-in toolbar actions, and the legacy drag/drop customization workflow. |
| Lua plugin runtime and manager | Partial | Discovers bundled, explicit `--plugin-dir`, and per-user plugin paths; reads `plugin.ini`, loads enabled Lua 5.4 scripts, exposes a Plugins menu and manager, persists enabled-path overrides, and dispatches registered menu actions, accelerators, and canvas shortcuts. `app.C` tool IDs, `app.registerUi` menu/shortcut registration, asynchronous `app.openDialog`, `app.export` with PDF/SVG/PNG options, `app.quickScreenshot`, and the complete `app.getDocumentStructure()` page/layer/background-color/PDF-path shape work. Registered `app.registerUi` toolbar IDs can be placed in editable floating-toolbox rows as `Plugin::ID`; enabled plugin buttons use the registered label/icon and invoke their Lua callback. `app.registerPlaceholder` adds described labels to those rows; `app.setPlaceholderValue` updates a label by bare or `Plugin::`-prefixed ID, and an empty value displays the ID. `app.getDisplayDpi` reports the active monitor's physical DPI, clamped to 36–720 with a 72-DPI fallback when physical dimensions are unavailable; the manual legacy override remains unported. `app.getFolder` creates per-plugin config, data, and state directories under the XDG user roots. `app.getZoom` and `app.setZoom` read and change zoom around the viewport center, leave fit mode, and use the canvas zoom limits. `app.setCurrentPage` clamps one-based page numbers and changes the active page without scrolling. `app.getPageLabel` returns a one-based PDF page label or `nil, message` when the page is out of range. `app.getColorPalette` returns ordered palette entries with names and 24-bit RGB colors. `app.refreshPage` invalidates page/layer render and thumbnail caches and redraws without adding undo history. `app.getActionState` reads zoom, selected tool, grid snapping, position highlighting, the active tool color in legacy ARGB form, active and per-tool size presets (`ToolSize_none` for custom or unsupported widths), eraser mode, and `tool-pen-line-style` (`plain` for the Pen tool or selected Pen strokes, otherwise `none`); other action states remain unsupported. `app.getToolInfo` returns the upstream active, Pen, Highlighter, Eraser, Text, and Selection query shapes. Selection `boundingBox`, `originalBounds`, and `snappedBounds` currently use the GTK4 selection rectangle; native padding and distinct source/snapping bounds remain to port. `app.getFonts`, `app.getFont`, and `app.setFont` list installed families, read the default family/size, and accept Pango descriptions or partial family/size tables; changes update defaults, apply to selected text or the active editor, and selected text changes are undoable. `app.setCurrentLayer(layerNr, change_visibility)` selects a foreground layer, clears selection, and optionally shows layers through that ID while hiding higher layers without changing background visibility or creating undo history. Layer ID zero retains the upstream all-hidden visibility edge case while the GTK model keeps its first foreground layer selected internally. `app.setBackgroundName` directly updates the current page background name with the upstream no-undo, no-dirty-state semantics. `app.getScrollPos` reports the canvas position and viewport dimensions; `app.scrollToPage` clamps absolute and relative page navigation; `app.scrollToPos` supports relative offsets and absolute coordinates in canvas pixels. Color changes, selected-stroke/text recoloring, common tool selection, grid snapping and position highlighting state, current-page plain/graph background changes, batched empty-page insertion, ImageActions transforms, page navigation, Go to Page, undo/redo, and default-tool actions work. `app.getStrokes`, `app.getTexts`, `app.getLinks`, and `app.getImages` query editable objects from the selection, current layer, current page, or whole document with the upstream fields and one-based page/layer metadata. Opaque element references survive vector growth and value-semantic edits; `app.addToSelection` resolves current-layer references and `app.clearSelection` releases the selection. `app.addStrokes`, `app.addSplines`, `app.addTexts`, `app.addLinks`, and `app.addImages` create editable objects on the current layer and return opaque references. `app.addSplines` flattens cubic segments with the master's adaptive tolerance, applies per-stroke styling, and shares stroke undo policies. Image insertion accepts file paths or binary image data, preserves the master sizing/aspect-ratio and page-fit behavior, and returns per-item soft errors. Text and link defaults use the configured font, size, and color; link alignment constants are available as `app.C.Alignment_*`. Grouped and individual batches preserve selection through undo/redo; no-undo stroke/text/link/image batches create an irreversible revision boundary and still mark the document dirty. Page resizing, layer visibility/name changes, and the FitToContent layer action are covered. Unsupported native objects remain outside these editable APIs, and stroke line styles beyond plain/solid remain unsupported. Port the other master Lua APIs, remaining image/object mutation, plugin-specific actions and state, and remaining bundled plugins. |
| Page layout | Partial | Continuous vertical pages work. Port paired, horizontal, grid, and other configured layouts. |
| Page editing history | Partial | Insert/delete pages and document edits work; page insert/delete are not undoable. |
| Input devices | Partial | Smooth touchpad surface deltas now move the canvas one logical pixel per reported pixel; discrete mouse-wheel detents retain the existing 35-pixel step. Conversion is covered by the canvas-state unit test. The user reports that the current build still moves too quickly and jumps unpredictably on a touchpad; reproduce and tune this later on the user's physical device. Physical-device motion stability, production device identity, physical stylus, touch drawing, hand gestures, kinetic scrolling, and platform-specific routing still need verification or porting. |
| Native documents | Partial | Supported strokes, text, links, and image suffixes round-trip. PDF-backed text selection uses the original PDF source and page mapping. Make unsupported native object types editable without losing preservation. |
| Accessibility and platform behavior | Pending | Port accessibility semantics and verify each supported platform and packaging path. |

## Explicit exclusions

None recorded. Before excluding anything, add its source identifier, the reason
it is obsolete, and the decision that authorizes removal. Update this section,
the behavior ledger, and the affected regression checks together.
