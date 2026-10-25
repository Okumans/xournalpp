# GTK 4 migration feature audit

This audit tracks observable behavior from canonical Xournal++ master against
the separate GTK 4/libadwaita prototype. Source snapshot: `upstream/master`
`b2ef6316989d379cfcb6e1048363a0b6c2befa6b`, fetched on 2026-10-25. Update the
status and source reference as each migration slice lands.

Current ledger count: **53 task areas — 40 Partial, 9 Pending, 4 Covered, and 0
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
| `pen` | Partial | Match the full renderer: caps, fills, dashes, blend modes, and all native stroke styles. Physical tablet behavior is not verified. |
| `eraser` | Partial | Standard split, whiteout, and delete-stroke modes work; reproduce all production hit geometry and renderer interactions. |
| `highlighter` | Partial | Core opacity and native serialization work; reproduce production stroke geometry and blend behavior. |
| `text` | Partial | Rich editing, toolbar font/size/style/color controls, direct Text-tool drag-to-move and handle transforms, rectangle/lasso selection, and cross-page transfer work. Add text clipboard targets, transform-following editor placement, IME, and accessibility. |
| `image` | Partial | Insert, move, resize, rotate, delete, native save/reopen, PDF export, and editable native clipboard copy/cut/paste work. Multi-image copy/cut/paste and duplicate preserve relative transforms; PNG clipboard data carries the rendered image group. Mixed-object clipboard operations and unsupported native object editing remain. |
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
| `showFloatingToolbox` | Partial | A GTK4 floating panel opens at the pointer, clamps to the canvas, closes on pointer leave or tool choice, and works from canvas mode or a stylus mapping. Smart Select is pinned in the main left rail, while secondary tools are grouped under More tools. Four `toolbarFloat` rows import read-only from `toolbar.ini`, can be edited in Preferences > Toolbar, and persist separately in `gtk4-canvas.ini`; common pen/highlighter, laser/PDF, undo/redo, zoom, size, drawing-mode, and palette actions render. `TOOL_FILL` and unknown tokens are disabled pending parity. Add complete token coverage, mouse-button mappings, stroke-filter tap triggers, the legacy drag/drop/configuration workflow, and exact placement/dismiss/reopen behavior. |
| `drawSpline` | Partial | Knot, tangent, preview, and editing behavior work; verify generated geometry against the production handler. |
| `selectPdfTextLinear` | Partial | Linear drag, word/line click selection, copy, and PDF annotation actions work against the original PDF source. Match the GTK 3 floating toolbox placement and dismissal/reopen behavior. |
| `selectPdfTextRect` | Partial | Rectangular drag, word/line click selection, copy, and PDF annotation actions work against the original PDF source. Match the GTK 3 floating toolbox placement and dismissal/reopen behavior. |
| `laserPointerPen` | Partial | Transient pressure-aware laser trails use the master presets and imported fade delay; completed marks fade after the 500 ms default delay in 50 ms steps of 25/255 alpha, resume at full alpha when another stroke starts, and never enter save or undo history. Physical stylus routing and exact raster-mask behavior across zoom/visibility changes remain unverified. |
| `laserPointerHighlighter` | Partial | Transient pressure-independent highlighter trails use the master presets and imported fade delay; completed marks share the per-page trail and fade lifecycle without entering save or undo history. Physical stylus routing and exact raster-mask behavior across zoom/visibility changes remain unverified. |
| `link` | Partial | The Link tool creates and edits native links, renders them underlined, hit-tests them on the active visible layer, opens them with Ctrl-click, and preserves URL/font/layout through save and undo/redo. Match the production editor and scheme choices, verify external launcher behavior, and test physical input routing. |
| `latex` | Pending | Port LaTeX text creation, rendering, editing, and dependency/error behavior. |
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
| Text editing | Partial | Inline rich-text editing and toolbar font/size/style/color controls work; the Text tool directly moves and transforms text, and newly created or edited text remains selected. The editor overlay does not follow transforms. |
| Layer selection and operations | Partial | Add/delete/merge/duplicate/rename/reorder/visibility and visible-layer stroke/text/image selection work. The master model has no layer-lock action. |
| Native object picking | Partial | Smart Select and the distinct Select Object tool pick editable strokes, images, text, and links; Select Object uses nearest-hit selection and Shift-add. Unsupported native objects cannot be edited. |
| Clipboard | Partial | Bounded `application/xournal` stroke and multi-image copy/cut/paste works; multi-image duplicate is one undoable edit. Images also offer PNG and accept PNG clipboard paste. Mixed-object selection actions, remaining native types, text and SVG selection targets, and cross-process compatibility remain. |
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
| `BeamerPresentation` | Navigate animated Beamer PDFs and remove animation pages. | Pending |
| `ColorCycle` | Cycle through a configured color list. | Covered |
| `Example` | Demonstrate the plugin API. | Covered — manifest loading, menu registration, `app.openDialog`, and its asynchronous button-response callback work and are regression-tested. |
| `Export` | Export documents to PDF, SVG, and PNG. | Pending |
| `FitToContent` | Resize page content to fit a selection or layer. | Pending |
| `HighlightPosition` | Toggle a position highlight using a shortcut. | Covered — Alt+X toggles the pointer halo; the fill/border color, radius, and enabled state are editable in Preferences and persist. |
| `ImageActions` | Invert, resize, flip, rotate, make white pixels transparent, trim/crop, and split selected images into rows or columns. | Pending |
| `LayerActions` | Clone layers to following pages, hide layers, and add top layers. | Pending |
| `QuickScreenshot` | Capture a screen region to an image using supported platform utilities. | Pending |
| `SpaceForNotes` | Add empty pages for notes. | Pending |
| `ToggleGrid` | Toggle the page grid. | Covered — toggles grid snapping and switches the active page between plain and graph backgrounds; native document save, reopen, undo, and redo preserve the background. |
| `ToolShortcuts` | Add canvas-scoped tool and page-navigation shortcuts. | Partial — bundled shortcuts select common tools including Smart Select, navigate pages, open Go to Page, and invoke undo/redo. Search and some master actions are missing. |

## Cross-cutting application behavior

| Area | GTK 4 status | Remaining parity work |
| --- | --- | --- |
| Preferences and legacy settings | Partial | Existing settings import read-only and supported controls persist separately, including page-grid snapping, selection edge-pan speed/multiplier, spacing, tolerance, pointer highlight enable/color/radius/border, laser tool colors/sizes, laser fade delay, stylus Floating Toolbox mapping, audio playback folder/seek/disable settings, the customizable main tool rail, and editable floating-toolbar rows. Audit every user-facing setting, per-device `deviceClasses`, audio device/recording controls, unsupported toolbar actions, and the legacy drag/drop customization workflow. |
| Lua plugin runtime and manager | Partial | Discovers bundled, explicit `--plugin-dir`, and per-user plugin paths; reads `plugin.ini`, loads enabled Lua 5.4 scripts, exposes a Plugins menu and manager, persists enabled-path overrides, and dispatches registered menu actions, accelerators, and canvas shortcuts. `app.C` tool IDs, `app.registerUi`, asynchronous `app.openDialog`, color changes, selected-stroke/text recoloring, common tool selection, grid snapping and position highlighting state, current-page plain/graph background changes, page navigation, Go to Page, undo/redo, and default-tool actions work. Port the remaining master Lua API, toolbar integration, plugin-specific actions and state, and remaining bundled plugins. |
| Page layout | Partial | Continuous vertical pages work. Port paired, horizontal, grid, and other configured layouts. |
| Page editing history | Partial | Insert/delete pages and document edits work; page insert/delete are not undoable. |
| Input devices | Partial | Synthetic mouse/stylus paths work. Match touchpad pan/scroll sensitivity and motion stability to the original app; port production device identity, physical stylus, touch drawing, hand gestures, and platform-specific routing. |
| Native documents | Partial | Supported strokes, text, links, and image suffixes round-trip. PDF-backed text selection uses the original PDF source and page mapping. Make unsupported native object types editable without losing preservation. |
| Accessibility and platform behavior | Pending | Port accessibility semantics and verify each supported platform and packaging path. |

## Explicit exclusions

None recorded. Before excluding anything, add its source identifier, the reason
it is obsolete, and the decision that authorizes removal. Update this section,
the behavior ledger, and the affected regression checks together.
