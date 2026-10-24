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
| Pressure default | `Settings::loadDefault`: minimum 0.05, multiplier 1, guessing off | Input adapter work in progress |
| Pen release | `StrokeHandler::onButtonReleaseEvent` finalizes existing samples | Input adapter work in progress |
| Stylus eraser | `StylusInputHandler::changeTool` applies the eraser mapping temporarily | Input adapter work in progress |
| Temporary hand tool | `XournalView` space press/release and `PenInputHandler::temporaryPan` | Input adapter work in progress |

These match the original **defaults**. Importing existing preferences, per-device
mappings, palettes, and toolbar configuration is still required for full parity.
The prototype continues to use its independent session-only document model.

## Known differences that block application replacement

- The original eraser supports standard partial erasing, whole-stroke deletion,
  and whiteout. This experiment only deletes whole session strokes.
- Existing stabilization, pressure inference and configuration, Shift drawing
  constraints, Shift-click connection, stroke/tap filtering, and device-specific
  button mappings have not been migrated.
- The original uses its document, selection, layer, undo, and rich-text models.
  PDF export flattens those into the prototype background. Session ink cannot
  yet be saved to `.xopp` or edited by the production tools.
- The prototype shows a single page at a time and refits on page changes. It
  does not reproduce continuous-page layout, scroll positioning, or page history.
- The normal application can configure touch behavior, hand recognition,
  scrolling, and gesture zoom. The experiment has only a small input subset.
- The original stroke renderer handles pressure geometry, caps, line styles,
  fill, and blend modes beyond the experiment's simple Cairo segments.
- Original shortcut bindings, clipboard, IME, plugins, accessibility, platform
  behavior, and release packaging still need a complete migration.

Each subsequent migration change should identify its source behavior, add a
regression check, and update this ledger. Passing synthetic input checks is not
evidence of physical pen, touchscreen, or touchpad parity.
