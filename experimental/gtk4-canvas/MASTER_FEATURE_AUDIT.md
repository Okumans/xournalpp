# GTK 4 migration feature audit

This audit tracks observable behavior from canonical Xournal++ master against
the separate GTK 4/libadwaita prototype. Source snapshot: `upstream/master`
`b2ef6316989d379cfcb6e1048363a0b6c2befa6b`, fetched on 2026-10-25. Update the
status and source reference as each migration slice lands.

## Status rules

- **Covered**: behavior is implemented and has a regression check.
- **Partial**: a user-visible subset works; the table names the remaining gap.
- **Pending**: no compatible GTK 4 implementation exists yet.
- **Obsolete**: excluded only after an explicit decision records why the old
  behavior no longer belongs in the migration.

No user-facing master feature is classified obsolete yet. `TOOL_NONE` is an
internal sentinel, not a user feature. Keep every other behavior in scope until
its status changes with source and test evidence.

## Master tools

The tool names below come from `src/core/control/ToolEnums.h` and the handler
registration in `ToolHandler::initTools`.

| Master tool | GTK 4 status | Remaining parity work |
| --- | --- | --- |
| `pen` | Partial | Match the full renderer: caps, fills, dashes, blend modes, and all native stroke styles. Physical tablet behavior is not verified. |
| `eraser` | Partial | Standard split, whiteout, and delete-stroke modes work; reproduce all production hit geometry and renderer interactions. |
| `highlighter` | Partial | Core opacity and native serialization work; reproduce production stroke geometry and blend behavior. |
| `text` | Partial | Rich editing and round-trip work. Add text object selection, move/resize/rotate, transform-following editor placement, IME, and accessibility. |
| `image` | Partial | Insert, move, resize, rotate, delete, native save/reopen, and PDF export work. Add image clipboard operations and integrate images into general object selection. |
| `selectRect` | Partial | Selects editable strokes on the active visible foreground layer; extend to the full production object and layer rules. |
| `selectRegion` | Partial | Lasso-selects editable strokes on the active visible foreground layer; extend to the full production object and layer rules. |
| `selectMultiLayerRect` | Pending | Select matching objects across layers with production visibility and ordering semantics. |
| `selectMultiLayerRegion` | Pending | Lasso-select matching objects across layers with production visibility and ordering semantics. |
| `selectObject` | Pending | Port the master object-selection behavior as a distinct tool. |
| `playObject` | Pending | Select and play supported audio objects with production controls and lifecycle. |
| `verticalSpace` | Pending | Insert vertical space with the same affected objects, undo behavior, and page boundaries. |
| `hand` | Partial | Space and stylus mappings can pan temporarily; add the dedicated hand-tool behavior. |
| `drawRect` | Partial | Drag preview, constraints, and undo work as an editable stroke path; preserve native geometric objects. |
| `drawEllipse` | Partial | Drag preview, constraints, and undo work as an editable stroke path; preserve native geometric objects. |
| `drawArrow` | Partial | Arrow path drawing works; match native geometry and styling. |
| `drawDoubleArrow` | Partial | Double-arrow path drawing works; match native geometry and styling. |
| `drawCoordinateSystem` | Partial | Coordinate-axis paths work; match native geometry and production modifiers. |
| `showFloatingToolbox` | Pending | Port floating toolbox creation, placement, persistence, and interaction. |
| `drawSpline` | Partial | Knot, tangent, preview, and editing behavior work; verify generated geometry against the production handler. |
| `selectPdfTextLinear` | Pending | Select PDF text by linear drag and expose the production copy/annotation actions. |
| `selectPdfTextRect` | Pending | Select PDF text by rectangular drag and expose the production copy/annotation actions. |
| `laserPointerPen` | Pending | Port timed laser-pen rendering and presentation behavior. |
| `laserPointerHighlighter` | Pending | Port timed laser-highlighter rendering and presentation behavior. |
| `link` | Pending | Create, edit, and activate links with production hit testing and persistence. |
| `latex` | Pending | Port LaTeX text creation, rendering, editing, and dependency/error behavior. |
| `smartSelect` | Partial | Picks editable strokes and images, and opens text editing. Add general object transforms and complete production hit ordering. |

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
| Stroke rectangle/lasso selection | Partial | Active visible layer only; add production cross-layer and complete object rules. |
| Stroke move, resize, mirror, rotate | Partial | Undoable transforms work; exact grid snapping, edge pan, persistent rotated handle frame, and device behavior remain. |
| Image move, resize, rotate | Partial | Undoable affine transforms and master angle snapping work; add general multi-object selection and image clipboard operations. |
| Text editing | Partial | Inline rich-text editing works; text is not an object-selection transform target. |
| Layer selection and operations | Partial | Add/delete/merge/duplicate/rename/reorder/visibility work. Multi-layer selection is pending; the master model has no layer-lock action. |
| Native object picking | Partial | Editable strokes, images, and text have separate picking paths; unsupported native objects cannot be edited. |
| Clipboard | Partial | Bounded `application/xournal` stroke copy/cut/paste works. Add remaining native types, image/text targets, SVG/PNG, and cross-process compatibility. |

## Built-in plugin inventory

The master repository bundles these directories under `plugins/`. The prototype
has no Lua runtime, loader, manager, plugin menu, plugin toolbar items, or plugin
shortcut dispatch, so each remains pending. `Example` is a reference plugin;
it stays listed because it is part of the master plugin distribution.

| Plugin | Observable behavior | GTK 4 status |
| --- | --- | --- |
| `BeamerPresentation` | Navigate animated Beamer PDFs and remove animation pages. | Pending |
| `ColorCycle` | Cycle through a configured color list. | Pending |
| `Example` | Demonstrate the plugin API. | Pending |
| `Export` | Export documents to PDF, SVG, and PNG. | Pending |
| `FitToContent` | Resize page content to fit a selection or layer. | Pending |
| `HighlightPosition` | Toggle a position highlight using a shortcut. | Pending |
| `ImageActions` | Invert, resize, flip, rotate, make white pixels transparent, trim/crop, and split selected images into rows or columns. | Pending |
| `LayerActions` | Clone layers to following pages, hide layers, and add top layers. | Pending |
| `QuickScreenshot` | Capture a screen region to an image using supported platform utilities. | Pending |
| `SpaceForNotes` | Add empty pages for notes. | Pending |
| `ToggleGrid` | Toggle the page grid. | Pending |
| `ToolShortcuts` | Add canvas-scoped tool and page-navigation shortcuts. | Pending |

## Cross-cutting application behavior

| Area | GTK 4 status | Remaining parity work |
| --- | --- | --- |
| Preferences and legacy settings | Partial | Existing settings import read-only and supported controls persist separately. Audit every user-facing setting, per-device `deviceClasses`, and `toolbar.ini` customization. |
| Page layout | Partial | Continuous vertical pages work. Port paired, horizontal, grid, and other configured layouts. |
| Page editing history | Partial | Insert/delete pages and document edits work; page insert/delete are not undoable. |
| Input devices | Partial | Synthetic mouse/stylus paths work. Port production device identity, physical stylus, touch drawing, hand gestures, and platform-specific routing. |
| Native documents | Partial | Supported strokes, text, and image suffixes round-trip. Make unsupported native object types editable without losing preservation. |
| Accessibility and platform behavior | Pending | Port accessibility semantics and verify each supported platform and packaging path. |

## Explicit exclusions

None recorded. Before excluding anything, add its source identifier, the reason
it is obsolete, and the decision that authorizes removal. Update this section,
the behavior ledger, and the affected regression checks together.
