#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "BehaviorDefaults.h"
#include "CanvasState.h"

using namespace prototype;

static void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static void brushAndHistory() {
    CanvasState state;
    state.brush = {Tool::Pen, {1, 0, 0}, 4};
    state.begin(Device::Mouse, 40, 40);
    state.brush = {Tool::Highlighter, {1, 1, 0}, 18};
    state.append(Device::Mouse, 100, 40);
    state.finish(Device::Mouse);
    check(state.ink[0].brush.width == 4 && state.ink[0].brush.color[0] == 1 && state.ink[0].brush.tool == Tool::Pen,
          "changing the selected brush cannot restyle a stroke in progress");
    state.begin(Device::Mouse, 40, 40);
    state.append(Device::Mouse, 100, 40, 0.3);
    state.finish(Device::Mouse);
    check(state.ink[1].brush.tool == Tool::Highlighter && state.ink[1].brush.width == 18,
          "new strokes capture the selected brush");
    state.undo();
    check(state.ink.size() == 1 && state.canRedo(), "undo exposes a redo step");
    state.erase(500, 500);
    check(state.canRedo(), "a no-op eraser must preserve redo");
    state.redo();
    check(state.ink.size() == 2 && state.ink[1].brush.tool == Tool::Highlighter,
          "redo restores stroke style and ordering");
    state.eraserMode = EraserMode::DeleteStroke;
    state.erase(70, 40);
    check(state.ink.empty(), "delete-stroke eraser can remove multiple overlapping strokes");
    state.undo();
    check(state.ink.size() == 2 && state.ink[0].brush.tool == Tool::Pen && state.ink[1].brush.tool == Tool::Highlighter,
          "erase undo restores all strokes in their original order");
    state.redo();
    check(state.ink.empty(), "erase redo removes the same strokes");
    state.undo();
    state.undo();
    state.begin(Device::Mouse, 200, 80);
    state.finish(Device::Mouse);
    check(!state.canRedo(), "a new edit replaces the undone history branch");

    CanvasState bounded;
    for (int i = 0; i < 140; ++i) {
        bounded.begin(Device::Mouse, i, 20);
        bounded.finish(Device::Mouse);
    }
    for (int i = 0; i < 140; ++i) {
        bounded.undo();
    }
    check(bounded.ink.empty() && !bounded.canUndo(),
          "history must retain edits beyond the old prototype's 128-step cap");
    for (int i = 0; i < 140; ++i) {
        bounded.redo();
    }
    check(bounded.ink.size() == 140 && !bounded.canRedo(), "retained history remains fully redoable");
}

static void fillStrokesAndSelection() {
    CanvasState state;
    state.brush = {Tool::Pen, {0.8, 0.1, 0.2}, 4, true, 96};
    state.drawingMode = DrawingMode::Rectangle;
    check(state.begin(Device::Mouse, 20, 20), "filled Pen rectangle did not start");
    state.append(Device::Mouse, 100, 80);
    state.finish(Device::Mouse);
    check(state.ink.size() == 1 && state.ink[0].fill == 96,
          "A filled Pen drawing did not retain its configured opacity");

    state.selected = {0};
    check(state.hasSelectedFillableInk() && state.setSelectedInkFill(180, 72) && state.ink[0].fill == 180,
          "Fill did not update a selected Pen stroke");
    state.undo();
    check(state.ink[0].fill == 96, "Undo did not restore the selected stroke fill");
    state.redo();
    check(state.ink[0].fill == 180, "Redo did not restore the selected stroke fill");
    check(state.setSelectedInkFill(-1, -1) && state.ink[0].fill == -1,
          "Fill could not be removed from a selected Pen stroke");
    state.undo();
    check(state.ink[0].fill == 180, "Undo did not restore a removed fill");

    state.brush = {Tool::Highlighter, {1, 1, 0}, 12, true, 72};
    state.drawingMode = DrawingMode::Freehand;
    check(state.begin(Device::Mouse, 130, 40), "filled Highlighter stroke did not start");
    state.append(Device::Mouse, 180, 40);
    state.finish(Device::Mouse);
    check(state.ink.size() == 2 && state.ink[1].fill == 72,
          "A filled Highlighter stroke did not retain its configured opacity");
}

static void savedHistory() {
    CanvasState state;
    check(state.revision() == 0, "new state starts at the clean revision");
    state.begin(Device::Mouse, 20, 20);
    state.finish(Device::Mouse);
    const auto saved = state.revision();
    state.begin(Device::Mouse, 40, 40);
    state.finish(Device::Mouse);
    check(state.revision() != saved, "an additional edit changes the revision");
    state.undo();
    check(state.revision() == saved, "undo back to saved state must be clean");
    state.undo();
    check(state.revision() == 0, "undo to initial state restores revision zero");
    state.redo();
    check(state.revision() == saved, "redo to saved state must be clean");
    state.undo();
    state.begin(Device::Mouse, 80, 80);
    state.finish(Device::Mouse);
    check(state.revision() != saved && !state.canRedo(), "same-depth replacement is a new revision");
    const auto current = state.revision();
    state.beginErase();
    state.finishErase();
    check(state.revision() == current, "no-op erase must not dirty a saved state");
    state.beginErase();
    state.erase(80, 80);
    state.finishErase();
    check(state.revision() != current, "one grouped eraser drag changes the revision");
    state.undo();
    check(state.revision() == current, "undo eraser restores the saved revision");
}

static void editableImageHistory() {
    CanvasState state;
    CanvasImage image;
    image.page = 0;
    image.pixelWidth = 40;
    image.pixelHeight = 20;
    image.transform = {2, 0, 0, 2, 30, 50};
    image.encodedData = "bounded image bytes";
    check(state.insertImage(image) && state.images.size() == 1 && state.images[0].layerOrder == 0,
          "image insertion should assign the active page and editable layer order");
    check(state.revision() == 1, "image insertion should advance the saved revision");

    auto resized = state.images[0];
    resized.transform.xx = 3;
    resized.transform.yy = 3;
    resized.transform.x = 45;
    resized.transform.y = 65;
    check(state.replaceImage(0, resized) && state.images[0].transform.xx == 3,
          "image resize should replace its transform");
    state.undo();
    check(state.images[0].transform.xx == 2 && state.images[0].transform.x == 30,
          "image resize should restore its prior transform on undo");
    state.redo();
    check(state.images[0].transform.xx == 3 && state.images[0].transform.x == 45,
          "image resize should be redoable");

    check(state.addLayerAboveCurrent() && state.images[0].layerId == 1 &&
                  state.layersForPage(0).selectedLayer == 2,
          "adding a layer above the image should preserve its original layer");
    state.undo();
    check(state.images[0].layerId == 1 && state.layersForPage(0).selectedLayer == 1 &&
                  state.layersForPage(0).layers.size() == 1,
          "undoing a layer insertion should restore image layer assignment");
    check(state.deleteImage(0) && state.images.empty(), "image deletion should remove the selected object");
    state.undo();
    check(state.images.size() == 1 && state.images[0].encodedData == "bounded image bytes",
          "image deletion should restore image bytes and transform");
}

static void groupedImageReplacementHistory() {
    CanvasState state;
    for (int index = 0; index < 3; ++index) {
        CanvasImage image;
        image.page = 0;
        image.pixelWidth = 2;
        image.pixelHeight = 2;
        image.transform.x = index * 10;
        image.encodedData = "source-" + std::to_string(index);
        check(state.insertImage(std::move(image)), "grouped image test should insert source images");
    }
    const auto original = state.images;
    state.selectedImages = {0, 2};

    auto first = original[0];
    first.encodedData = "split-first";
    auto second = first;
    second.encodedData = "split-second";
    second.transform.y = 20;
    auto third = original[2];
    third.encodedData = "replaced-third";
    third.transform.x = 25;

    check(state.replaceImageGroup({{0, {first, second}}, {2, {third}}}),
          "selected image replacements should apply together");
    check(state.images.size() == 4 && state.images[0].encodedData == "split-first" &&
                  state.images[1].encodedData == "split-second" && state.images[2].encodedData == "source-1" &&
                  state.images[3].encodedData == "replaced-third" &&
                  state.selectedImages == std::vector<size_t>({0, 1, 3}),
          "grouped replacement should preserve order and select every transformed output");

    state.undo();
    check(state.images.size() == original.size() && state.images[0].encodedData == "source-0" &&
                  state.images[1].encodedData == "source-1" && state.images[2].encodedData == "source-2" &&
                  state.selectedImages == std::vector<size_t>({0, 2}),
          "one undo should restore all original images and their selection");
    state.redo();
    check(state.images.size() == 4 && state.images[0].encodedData == "split-first" &&
                  state.images[1].encodedData == "split-second" && state.images[3].encodedData == "replaced-third" &&
                  state.selectedImages == std::vector<size_t>({0, 1, 3}),
          "one redo should restore every replacement and output selection");
}

static void multiImagePasteHistory() {
    CanvasState state;
    state.setPages({{300, 400}});
    CanvasImage first;
    first.page = 0;
    first.pixelWidth = 40;
    first.pixelHeight = 20;
    first.transform = {1, 0, 0, 1, 20, 30};
    first.encodedData = "first image bytes";
    CanvasImage second = first;
    second.transform = {0, 1, -1, 0, 90, 60};
    second.encodedData = "second image bytes";
    const double relativeX = second.transform.x - first.transform.x;
    const double relativeY = second.transform.y - first.transform.y;

    check(state.pasteImages({first, second}, 0, {160, 180}) && state.images.size() == 2 &&
                  state.selectedImages == std::vector<size_t>{0, 1},
          "Pasting multiple images should insert and select the group in one edit");
    check(state.images[1].transform.x - state.images[0].transform.x == relativeX &&
                  state.images[1].transform.y - state.images[0].transform.y == relativeY,
          "Pasting multiple images should preserve their relative placement");
    state.undo();
    check(state.images.empty(), "One undo should remove the complete multi-image paste");
    state.redo();
    check(state.images.size() == 2 && state.selectedImages == std::vector<size_t>{0, 1},
          "Redo should restore every pasted image and its selection");

    const auto bounds = state.selectionBox();
    check(bounds.has_value(), "Pasted images should have a group selection bound");
    auto duplicates = state.images;
    check(state.pasteImages(std::move(duplicates), bounds->page,
                            {bounds->x0 / 2 + bounds->x1 / 2 + 20,
                             bounds->y0 / 2 + bounds->y1 / 2 + 20}) && state.images.size() == 4,
          "Duplicating multiple images should add the group as one edit");
    state.undo();
    check(state.images.size() == 2, "One undo should remove the complete multi-image duplicate");
    state.redo();
    check(state.images.size() == 4 && state.selectedImages == std::vector<size_t>{2, 3},
          "Redo should restore every duplicated image and its selection");
}

static void verticalSpaceMovement() {
    CanvasState state;
    state.setPages({{200, 300}});
    state.pageLayers[0].layers = {{"Lower", true, {0}}, {"Upper", true, {1}}};
    state.pageLayers[0].selectedLayer = 2;
    state.snapGridEnabled = false;

    auto stroke = [](double y, size_t layerId, size_t origin) {
        Ink item{0, {{20, y}, {80, y}}, {Tool::Pen, {0.1, 0.2, 0.3}, 2}, false};
        item.layerId = layerId;
        item.originLayer = origin;
        return item;
    };
    state.ink = {stroke(20, 1, 0), stroke(20, 2, 1), stroke(80, 2, 1), stroke(50, 2, 1)};

    auto text = [](double y, size_t layerId, size_t origin, const char* content) {
        TextItem item;
        item.page = 0;
        item.layerId = layerId;
        item.originLayer = origin;
        item.text = content;
        item.x = 10;
        item.y = y;
        item.layoutWidth = 40;
        item.layoutHeight = 8;
        return item;
    };
    state.texts = {text(20, 1, 0, "lower"), text(20, 2, 1, "above"),
                   text(80, 2, 1, "below"), text(46, 2, 1, "crossing")};

    auto image = [](double y, size_t layerId, size_t origin) {
        CanvasImage item;
        item.page = 0;
        item.layerId = layerId;
        item.originLayer = origin;
        item.pixelWidth = 20;
        item.pixelHeight = 8;
        item.transform.x = 10;
        item.transform.y = y;
        item.encodedData = "image bytes";
        return item;
    };
    state.images = {image(20, 1, 0), image(20, 2, 1), image(80, 2, 1), image(46, 2, 1)};

    const auto lowerStroke = state.ink[0];
    const auto lowerText = state.texts[0];
    const auto lowerImage = state.images[0];
    const auto beforeRevision = state.revision();
    auto screen = state.pageScreenPoint(0, 10, 50);
    check(state.beginVerticalSpace(screen.x, screen.y), "Vertical-space drag should begin on a page");
    screen = state.pageScreenPoint(0, 10, 60);
    state.moveVerticalSpace(screen.x, screen.y, false);
    const auto* preview = state.verticalSpacePreview();
    check(preview && preview->inkIndices == std::vector<size_t>{2} &&
                  preview->textIndices == std::vector<size_t>{2} &&
                  preview->imageIndices == std::vector<size_t>{2},
          "Moving space below the line should affect only fully contained objects below it on the active layer");
    check(state.displayText(2).y == 90 && state.displayImage(2).transform.y == 90 &&
                  state.selectionMatrix(2).y0 == 10 && state.ink[2].points.front().y == 80,
          "Vertical-space preview should translate ink, text and images without changing committed ink");
    check(state.finishVerticalSpace() && state.revision() != beforeRevision,
          "Finishing vertical space should commit one document edit");
    check(state.ink[2].points.front().y == 90 && state.texts[2].y == 90 && state.images[2].transform.y == 90 &&
                  state.ink[0].points == lowerStroke.points && state.ink[0].layerId == lowerStroke.layerId &&
                  state.texts[0] == lowerText && state.images[0].transform.y == lowerImage.transform.y,
          "Vertical space should move the supported active-layer objects and preserve other layers");
    const auto movedRevision = state.revision();
    state.undo();
    check(state.revision() == beforeRevision && state.ink[2].points.front().y == 80 && state.texts[2].y == 80 &&
                  state.images[2].transform.y == 80,
          "Undo should restore the entire vertical-space movement as one action");
    state.redo();
    check(state.revision() == movedRevision && state.ink[2].points.front().y == 90 && state.texts[2].y == 90 &&
                  state.images[2].transform.y == 90,
          "Redo should reapply the entire vertical-space movement");

    screen = state.pageScreenPoint(0, 10, 50);
    check(state.beginVerticalSpace(screen.x, screen.y), "A second vertical-space drag should begin");
    screen = state.pageScreenPoint(0, 10, 65);
    state.moveVerticalSpace(screen.x, screen.y, true);
    preview = state.verticalSpacePreview();
    check(preview && preview->inkIndices == std::vector<size_t>{1} &&
                  preview->textIndices == std::vector<size_t>{1} &&
                  preview->imageIndices == std::vector<size_t>{1},
          "Switching to Control direction should reselect objects fully above the boundary");
    check(state.finishVerticalSpace() && state.ink[1].points.front().y == 35 && state.texts[1].y == 35 &&
                  state.images[1].transform.y == 35 && state.ink[3].points.front().y == 50,
          "Moving space above the line should move only fully contained active-layer objects");

    state.snapGridEnabled = true;
    state.snapGridSize = 10;
    state.snapGridTolerance = 0.5;
    state.pageLayers[0].gridYOffset = 80;
    const auto beforeCancelledPreview = state.revision();
    screen = state.pageScreenPoint(0, 10, 102);
    check(state.beginVerticalSpace(screen.x, screen.y), "Grid-snapped vertical-space drag should begin");
    check(std::abs(state.verticalSpacePreview()->startY - 100) < 1e-9,
          "Vertical-space start should snap using the page's grid offset and user grid size");
    screen = state.pageScreenPoint(0, 10, 112);
    state.moveVerticalSpace(screen.x, screen.y, false);
    check(std::abs(state.verticalSpacePreview()->endY - 110) < 1e-9,
          "Vertical-space end should use the configured grid spacing and tolerance");
    state.cancelVerticalSpace();
    check(!state.finishVerticalSpace() && state.revision() == beforeCancelledPreview,
          "Cancelling a vertical-space preview should leave committed history intact");
}

static void imageRotationGeometry() {
    CanvasImage image;
    image.page = 0;
    image.pixelWidth = 40;
    image.pixelHeight = 20;
    image.transform = {2, 0, 0, 1, 10, 20};
    image.encodedData = "bounded image bytes";

    const auto beforeCenter = canvasImageCenter(image);
    const auto rotationHandle = canvasImageRotationHandle(image, 2);
    check(std::abs(rotationHandle.x - 50) < 1e-9 && std::abs(rotationHandle.y - 11) < 1e-9,
          "the image rotation handle should sit above the transformed top edge");
    const double snappedAngle = snapCanvasImageRotation(std::acos(-1.0) / 6.0 + 0.01, 0.30);
    check(std::abs(snappedAngle - std::acos(-1.0) / 6.0) < 1e-9,
          "image rotation should snap to the nearest 15-degree angle within tolerance");
    const double startAngle = -80 * std::acos(-1.0) / 180.0;
    const double currentAngle = -62 * std::acos(-1.0) / 180.0;
    check(std::abs(canvasImageRotationDelta(startAngle, currentAngle, true, 0.30) -
                   20 * std::acos(-1.0) / 180.0) < 1e-9,
          "rotation snapping should align the resulting image angle to a global 15-degree increment");
    check(std::abs(canvasImageRotationDelta(startAngle, currentAngle, false, 0.30) -
                   18 * std::acos(-1.0) / 180.0) < 1e-9,
          "disabled rotation snapping should preserve the unrounded pointer angle");
    check(rotateCanvasImage(image, std::acos(-1.0) / 2), "a finite rotation should update a valid image");
    const auto afterCenter = canvasImageCenter(image);
    check(std::abs(afterCenter.x - beforeCenter.x) < 1e-9 && std::abs(afterCenter.y - beforeCenter.y) < 1e-9,
          "rotating an image should keep its center anchored");
    check(std::abs(image.transform.xx) < 1e-9 && std::abs(image.transform.xy + 1) < 1e-9 &&
                  std::abs(image.transform.yx - 2) < 1e-9 && std::abs(image.transform.yy) < 1e-9,
          "image rotation should compose with the existing affine transform");

    const auto savedTransform = image.transform;
    check(!rotateCanvasImage(image, std::numeric_limits<double>::infinity()) &&
                  image.transform.xx == savedTransform.xx && image.transform.xy == savedTransform.xy &&
                  image.transform.yx == savedTransform.yx && image.transform.yy == savedTransform.yy &&
                  image.transform.x == savedTransform.x && image.transform.y == savedTransform.y,
          "an invalid rotation should leave the original image transform unchanged");
}

static void editableTextHistory() {
    CanvasState state;
    state.setPages({{595, 842}, {595, 842}});

    TextItem first;
    first.page = 0;
    first.text = "héllo";
    first.x = 40;
    first.y = 80;
    check(state.insertText(first), "text insertion should create an editable item");
    const auto insertedRevision = state.revision();
    check(state.texts.size() == 1 && state.texts[0].layerOrder == 0 && state.texts[0].text == "héllo" &&
                  state.selectedTexts == std::vector<size_t>{0},
          "new text should append to the page layer order and remain selected for immediate transforms");

    auto changed = state.texts[0];
    changed.text = "edited";
    changed.styleRuns.push_back({0, 6, "Sans Bold", 14, std::nullopt, 0});
    check(state.replaceText(0, changed) && state.texts[0].layerOrder == 0 && state.texts[0].text == "edited",
          "editing text should preserve its native layer position");
    const auto editedRevision = state.revision();
    check(editedRevision != insertedRevision, "a text edit should create a new document revision");
    state.undo();
    check(state.texts[0].text == "héllo" && state.texts[0].styleRuns.empty() &&
                  state.selectedTexts == std::vector<size_t>{0} &&
                  state.revision() == insertedRevision,
          "undo should restore text content, inline styles and selection");
    state.redo();
    check(state.texts[0].text == "edited" && state.texts[0].styleRuns.size() == 1 &&
                  state.revision() == editedRevision,
          "redo should restore edited text and inline styles");

    TextItem second;
    second.page = 1;
    second.text = "later page";
    check(state.insertText(second) && state.texts[1].layerOrder == 0 &&
                  state.selectedTexts == std::vector<size_t>{1},
          "layer order is tracked independently and the inserted text remains selected");
    state.insertPageAfter(0, {595, 842});
    check(state.texts[0].page == 0 && state.texts[1].page == 2,
          "inserting a page shifts text on later pages");
    state.undo();
    check(state.texts.size() == 1 && state.texts[0].page == 0,
          "text history remains undoable after an earlier page is inserted");
    state.redo();
    check(state.texts.size() == 2 && state.texts[1].page == 2,
          "text history remains redoable after an earlier page is inserted");
    check(state.deleteText(0) && state.texts.size() == 1 && state.texts[0].text == "later page" &&
                  state.selectedTexts == std::vector<size_t>{0},
          "text deletion removes only the requested item and repairs remaining selection indices");
    state.undo();
    check(state.texts.size() == 2 && state.texts[0].text == "edited" &&
                  state.selectedTexts == std::vector<size_t>{1},
          "undo restores a deleted text item at its original position and selection");
}

static void latexReplacementHistory() {
    CanvasState state;
    TextItem text;
    text.page = 0;
    text.text = "plain text";
    text.x = 36;
    text.y = 72;
    check(state.insertText(text), "plain text should be available for LaTeX replacement");
    const auto textRevision = state.revision();

    CanvasImage formula;
    formula.page = 0;
    formula.kind = CanvasImageKind::Latex;
    formula.nativeWidth = 44;
    formula.nativeHeight = 18;
    formula.pixelWidth = 44;
    formula.pixelHeight = 18;
    formula.encodedData = "%PDF-1.7\\nLaTeX fixture";
    formula.latexSource = "\\text{plain text}";
    formula.transform = {1, 0, 0, 1, text.x, text.y};

    check(state.replaceTextWithImage(0, formula) && state.texts.empty() && state.images.size() == 1 &&
                  state.images[0].layerOrder == text.layerOrder && state.images[0].transform.x == text.x &&
                  state.selectedTexts.empty() && state.selectedImages == std::vector<size_t>{0},
          "text to LaTeX replacement should preserve position and select the new formula");
    const auto formulaRevision = state.revision();
    check(formulaRevision != textRevision, "text to LaTeX replacement should create one document revision");

    state.undo();
    check(state.texts.size() == 1 && state.texts[0].text == "plain text" && state.images.empty() &&
                  state.selectedTexts == std::vector<size_t>{0} && state.selectedImages.empty() &&
                  state.revision() == textRevision,
          "undo should restore the original text and its selection in one step");
    state.redo();
    check(state.texts.empty() && state.images.size() == 1 && state.images[0].latexSource == "\\text{plain text}" &&
                  state.selectedImages == std::vector<size_t>{0} && state.revision() == formulaRevision,
          "redo should restore the vector formula and select it in one step");
}

static void selectedTextAppearanceHistory() {
    CanvasState state;
    TextItem item;
    item.page = 0;
    item.text = "styled";
    item.layoutWidth = 42;
    item.layoutHeight = 14;
    item.styleRuns = {{0, 6, "Sans Bold", 12, 0xff102030U, 1U}};
    check(state.insertText(item) && state.selectedTexts == std::vector<size_t>{0},
          "text insertion should expose the new object to toolbar formatting");

    const auto original = state.texts[0];
    check(state.recolorSelectedTexts(0xff405060U) && state.texts[0].color == 0xff405060U &&
                  state.texts[0].styleRuns[0].color == 0xff405060U && state.selectedTexts == std::vector<size_t>{0},
          "text color should apply to the selected object while preserving selection");
    state.undo();
    check(state.texts[0] == original && state.selectedTexts == std::vector<size_t>{0},
          "text color undo should restore base and inline colors");
    state.redo();
    check(state.texts[0].color == 0xff405060U && state.texts[0].styleRuns[0].color == 0xff405060U,
          "text color redo should restore base and inline colors");

    check(state.updateSelectedTexts([](TextItem& selected) {
              selected.fontSize = 18;
              selected.layoutWidth = 64;
              selected.layoutHeight = 21;
          }) && state.texts[0].fontSize == 18 && state.texts[0].layoutWidth == 64,
          "font controls should update a selected text item as one edit");
    state.undo();
    check(state.texts[0].fontSize == 12 && state.texts[0].layoutWidth == 42 &&
                  state.texts[0].color == 0xff405060U,
          "font control undo should leave earlier color formatting intact");
    state.redo();
    check(state.texts[0].fontSize == 18 && state.texts[0].layoutHeight == 21,
          "font control redo should restore text geometry metadata");
}

static void partialAndWhiteoutEraser() {
    check(behavior::penWidths == std::array<double, 5>{0.42, 0.85, 1.41, 2.26, 5.67},
          "pen width presets must match ToolHandler::initTools");
    check(behavior::highlighterWidths == std::array<double, 5>{1.0, 2.83, 8.50, 19.84, 30.0},
          "highlighter width presets must match ToolHandler::initTools");
    check(behavior::eraserWidths == std::array<double, 5>{1.0, 2.83, 8.50, 12.0, 18.0},
          "eraser width presets must match ToolHandler::initTools");
    check(behavior::matchToolSize(behavior::penWidths, 1.41) == behavior::ToolSize::Medium &&
                  !behavior::matchToolSize(behavior::penWidths, 6.0).has_value(),
          "preset matching must distinguish standard presets from custom widths");

    CanvasState state;
    check(state.eraserMode == EraserMode::Standard && std::abs(state.eraserWidth - 8.50) < 1e-9 &&
                  std::abs(state.brush.width - 1.41) < 1e-9,
          "default tool sizes and eraser mode must match Xournal++ medium presets");

    // Surround a pressure-sensitive stroke with two untouched neighbor strokes to check z-order.
    state.brush = {Tool::Pen, {0, 0, 0}, 2};
    state.begin(Device::Mouse, 20, 30);
    state.append(Device::Mouse, 180, 30);
    state.finish(Device::Mouse);

    state.brush = {Tool::Pen, {1, 0, 0}, 4};
    state.begin(Device::Stylus, 20, 100, 0.2);
    state.current->pressureSensitive = true;
    state.append(Device::Stylus, 180, 100, 1.0);
    state.finish(Device::Stylus);

    state.brush = {Tool::Highlighter, {1, 1, 0}, 8.50};
    state.begin(Device::Mouse, 20, 170);
    state.append(Device::Mouse, 180, 170);
    state.finish(Device::Mouse);

    const auto beforeEraseRev = state.revision();
    // Grazing outerBox (outerHalf = 8.5 + 0.4 * 4 = 10.1) without entering innerBox (8.5) must not erase.
    state.erase(100, 109.5);
    check(state.ink.size() == 3 && state.revision() == beforeEraseRev,
          "grazing the padded outer box without hitting the inner eraser square must not split ink");

    // Grouped Standard erase drag: split the middle stroke twice (into 3 pieces) and delete a dot.
    state.brush = {Tool::Pen, {0, 0.5, 0}, 3};
    state.begin(Device::Mouse, 240, 100);
    state.finish(Device::Mouse);
    const auto savedRev = state.revision();
    check(state.ink.size() == 4, "expected four strokes before grouped partial erase");

    state.beginErase();
    state.erase(70, 100);
    state.erase(130, 100);
    state.erase(240, 100);
    state.finishErase();

    // Stroke 0 (y=30) untouched, Stroke 1 (y=100) split into 3 pieces at indices 1..3,
    // Stroke 2 (y=170) now at index 4, Stroke 3 (dot at 240,100) completely removed -> 5 strokes total.
    check(state.ink.size() == 5, "grouped partial erase must split contacted stroke and remove dot");
    check(state.ink[0].points.front().y == 30 && state.ink[4].brush.tool == Tool::Highlighter,
          "partial erase must preserve z-order of surrounding strokes");
    // Expected outerHalf = 8.5 + 0.4 * 4 = 10.1 -> first gap [59.9, 80.1], second gap [119.9, 140.1].
    check(std::abs(state.ink[1].points.back().x - 59.9) < 1e-6 &&
                  std::abs(state.ink[2].points.front().x - 80.1) < 1e-6 &&
                  std::abs(state.ink[2].points.back().x - 119.9) < 1e-6 &&
                  std::abs(state.ink[3].points.front().x - 140.1) < 1e-6,
          "partial erase must split at exact PaddedBox boundaries");
    check(state.ink[1].pressureSensitive && state.ink[2].pressureSensitive && state.ink[3].pressureSensitive &&
                  state.ink[1].points.front().pressure < state.ink[1].points.back().pressure &&
                  state.ink[3].points.front().pressure < state.ink[3].points.back().pressure,
          "split sub-strokes must preserve pressure mode and interpolated pressures");

    state.undo();
    check(state.ink.size() == 4 && state.revision() == savedRev && state.ink[1].points.size() == 2 &&
                  state.ink[1].points.front().x == 20 && state.ink[1].points.back().x == 180 &&
                  state.ink[3].points.front().x == 240,
          "one undo must restore all split and deleted strokes in original order");
    state.redo();
    check(state.ink.size() == 5 && std::abs(state.ink[2].points.front().x - 80.1) < 1e-6,
          "one redo must reapply the grouped partial erase");

    // Whiteout mode draws opaque white Tool::Eraser strokes without altering Pen brush when eraserEnd is used.
    state.eraserMode = EraserMode::Whiteout;
    state.eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::Thick)];
    state.brush = {Tool::Pen, {0.2, 0.2, 0.8}, 1.41};
    check(state.begin(Device::Stylus, 40, 100, 0.3, true), "stylus eraser end in whiteout mode must begin stroke");
    state.append(Device::Stylus, 120, 100, 0.9);
    state.finish(Device::Stylus, 0.9);
    check(state.brush.tool == Tool::Pen && state.ink.back().brush.tool == Tool::Eraser &&
                  state.ink.back().brush.color == behavior::whiteoutColor &&
                  std::abs(state.ink.back().brush.width - 12.0) < 1e-9 && !state.ink.back().pressureSensitive,
          "whiteout must commit a constant-width white Tool::Eraser stroke and preserve toolbar brush");
    state.eraserMode = EraserMode::DeleteStroke;
    state.erase(80, 100);
    check(state.ink.back().brush.tool != Tool::Eraser, "whiteout strokes must be erasable like native strokes");

    // DeleteStroke must hit the visible painted body of a 30pt Very Thick stroke even with a 1pt Very Fine eraser.
    state.brush = {Tool::Highlighter, behavior::defaultHighlighterColor,
                   behavior::highlighterWidths[static_cast<size_t>(behavior::ToolSize::VeryThick)]};
    state.begin(Device::Mouse, 20, 220);
    state.append(Device::Mouse, 180, 220);
    state.finish(Device::Mouse);
    const auto countWithThick = state.ink.size();
    state.eraserWidth = behavior::eraserWidths[static_cast<size_t>(behavior::ToolSize::VeryFine)];
    state.erase(100, 232);  // 12pt from centerline y=220, well inside the 15pt half-width of a 30pt stroke.
    check(state.ink.size() == countWithThick - 1,
          "delete-stroke eraser must hit visible painted thickness of wide strokes");
}

static void continuousMultiPageLayout() {
    CanvasState state;
    state.setPages({{595, 842}, {792, 612}});  // Portrait A4 followed by Landscape Letter.
    check(state.pageCount() == 2 && std::abs(state.pageGap - 15.0) < 1e-9,
          "multi-page layout must expose page count and 15pt Layout.cpp inter-page gap");
    check(std::abs(state.layoutWidth() - 792.0) < 1e-9 && std::abs(state.layoutHeight() - (842.0 + 15.0 + 612.0)) < 1e-9,
          "layout dimensions must encompass widest page and vertical page gaps");
    check(std::abs(state.pageLeft(0) - 0.5 * (792.0 - 595.0)) < 1e-9 && std::abs(state.pageLeft(1)) < 1e-9,
          "narrower pages must center horizontally within mixed-width layouts");
    check(std::abs(state.pageTop(0)) < 1e-9 && std::abs(state.pageTop(1) - 857.0) < 1e-9,
          "page tops must stack vertically by page height plus pageGap");

    state.fit(1200, 900);
    // Pointer-centered zoom anchored on Page 2 must have zero drift (fixing legacy PixelCounter mixed-unit drift).
    const auto p2Screen = state.pageScreenPoint(1, 220, 180);
    const auto beforeLocal = state.pageLocalPoint(1, p2Screen.x, p2Screen.y);
    state.zoomAt(2.2, p2Screen.x, p2Screen.y);
    const auto afterLocal = state.pageLocalPoint(1, p2Screen.x, p2Screen.y);
    check(std::hypot(beforeLocal.x - afterLocal.x, beforeLocal.y - afterLocal.y) < 1e-9,
          "pointer-centered zoom must preserve exact anchor coordinates on subsequent pages");

    // Inter-page gap and horizontal margins must reject direct ink begins.
    const auto gapScreen = state.pageScreenPoint(0, 100, 842.0 + 7.5);
    check(!state.hitPage(gapScreen.x, gapScreen.y).has_value() && !state.begin(Device::Mouse, gapScreen.x, gapScreen.y),
          "the 15pt vertical inter-page gap must not accept ink");
    const auto narrowMarginScreen = state.pageScreenPoint(0, -10.0, 100.0);
    check(!state.hitPage(narrowMarginScreen.x, narrowMarginScreen.y).has_value(),
          "horizontal margin beside a narrower page must not hit the page");

    // Drawing directly on Page 1 and Page 2 stores local coordinates and promotes active page.
    const auto s0a = state.pageScreenPoint(0, 40, 800);
    const auto s0b = state.pageScreenPoint(0, 200, 800);
    check(state.begin(Device::Mouse, s0a.x, s0a.y), "must begin on Page 1");
    state.append(Device::Mouse, s0b.x, s0b.y);
    state.finish(Device::Mouse);
    check(state.page == 0 && state.ink.back().page == 0 && std::abs(state.ink.back().points.front().x - 40.0) < 1e-9,
          "Page 1 stroke must store Page 1 local coordinates");

    const auto s1a = state.pageScreenPoint(1, 40, 60);
    const auto s1b = state.pageScreenPoint(1, 200, 60);
    check(state.begin(Device::Mouse, s1a.x, s1a.y), "must begin directly on Page 2 without prior page switch");
    state.append(Device::Mouse, s1b.x, s1b.y);
    state.finish(Device::Mouse);
    check(state.page == 1 && state.ink.back().page == 1 && std::abs(state.ink.back().points.front().y - 60.0) < 1e-9,
          "Page 2 stroke must promote Page 2 and store Page 2 local coordinates");

    // Single eraser drag crossing from Page 1 across the 15pt gap onto Page 2 splits both and undoes in one step.
    const auto e0 = state.pageScreenPoint(0, 120, 800);
    const auto e1 = state.pageScreenPoint(1, 120, 60);
    state.beginErase();
    state.erase(e0.x, e0.y);
    state.erase(gapScreen.x, gapScreen.y);
    state.erase(e1.x, e1.y);
    state.finishErase();
    check(state.ink.size() == 4 && state.ink[0].page == 0 && state.ink[1].page == 0 && state.ink[2].page == 1 &&
                  state.ink[3].page == 1,
          "cross-page eraser gesture must split strokes on both pages");
    state.undo();
    check(state.ink.size() == 2 && state.ink[0].page == 0 && state.ink[1].page == 1,
          "one undo must restore strokes across both pages");

    // scrollToPage preserves zoom; syncActivePage tracks largest visible intersection area.
    const double keptZoom = state.zoom;
    state.scrollToPage(1, 1200, 900);
    check(state.page == 1 && std::abs(state.zoom - keptZoom) < 1e-9, "scrollToPage must preserve zoom");
    state.offsetY = 0.0;
    state.syncActivePage(1200, 900);
    check(state.page == 0, "syncActivePage must select Page 1 when Page 1 dominates viewport");
    state.offsetY = -state.pageTop(1) * state.zoom;
    state.syncActivePage(1200, 900);
    check(state.page == 1, "syncActivePage must select Page 2 when scrolled to Page 2");

    // insertPageAfter shifts later strokes + undo history; deletePageAt removes target strokes and shifts later pages.
    state.insertPageAfter(0, {595, 842});
    check(state.pageCount() == 3 && state.page == 1 && state.ink[0].page == 0 && state.ink[1].page == 2,
          "insertPageAfter must insert page and shift subsequent stroke page indices");
    check(state.deletePageAt(0) && state.pageCount() == 2 && state.ink.size() == 1 && state.ink[0].page == 1 &&
                  !state.canUndo(),
          "deletePageAt must drop deleted-page strokes, shift remaining page indices, and clear stale undo indices");
    check(state.deletePageAt(0) && state.pageCount() == 1 && !state.deletePageAt(0),
          "deletePageAt must refuse deleting the last remaining page");
}

static void pluginBackgroundNameSetter() {
    CanvasState state;
    state.setPages({{595, 842}});
    PageLayers pageLayers;
    pageLayers.backgroundName = "Paper";
    state.setLayerDocument({pageLayers});
    state.brush = {Tool::Pen, {0, 0, 0}, 2};
    check(state.begin(Device::Mouse, 20, 20), "background-name test stroke should begin");
    state.append(Device::Mouse, 40, 40);
    state.finish(Device::Mouse);
    state.selectAllOnPage();

    const auto revision = state.revision();
    const auto selection = state.selected;
    check(state.canUndo() && state.setBackgroundName(0, "Custom paper") &&
                  state.layersForPage(0).backgroundName == "Custom paper" && state.revision() == revision &&
                  state.canUndo() && state.selected == selection,
          "the plugin background-name setter should update the page without undoing, dirtying, or clearing selection");
    state.undo();
    check(state.layersForPage(0).backgroundName == "Custom paper",
          "undoing an older edit should not roll back a direct plugin background-name change");
    check(!state.setBackgroundName(-1, "Invalid") && !state.setBackgroundName(1, "Invalid") &&
                  state.layersForPage(0).backgroundName == "Custom paper",
          "the plugin background-name setter should reject page indices outside the document");
}

static void scrollDeltaUnits() {
    CanvasState state;
    state.offsetX = 100;
    state.offsetY = 200;
    state.scrollBy(1.25, -2.5, false);
    check(std::abs(state.offsetX - 98.75) < 1e-9 && std::abs(state.offsetY - 202.5) < 1e-9,
          "smooth surface scroll deltas should map one-to-one to canvas logical pixels");
    state.scrollBy(-1, 2, true);
    check(std::abs(state.offsetX - 133.75) < 1e-9 && std::abs(state.offsetY - 132.5) < 1e-9,
          "discrete wheel detents should retain their existing 35-pixel movement");
    state.scrollBy(std::numeric_limits<double>::infinity(), 10, false);
    check(std::abs(state.offsetX - 133.75) < 1e-9 && std::abs(state.offsetY - 132.5) < 1e-9,
          "non-finite scroll deltas should not move the canvas");
}

static void layerManagement() {
    CanvasState state;
    state.setPages({{595, 842}, {595, 842}});
    PageLayers source;
    source.initialLayerCount = 2;
    source.selectedLayer = 1;
    source.layers = {{"Lower", true, {0}}, {"Upper", true, {1}}};
    state.setLayerDocument({source, source});

    state.brush = {Tool::Pen, {0, 0, 0}, 2};
    check(state.begin(Device::Mouse, 20, 20), "drawing should begin on the initial layer");
    state.append(Device::Mouse, 40, 40);
    state.finish(Device::Mouse);
    check(state.ink.size() == 1 && state.ink[0].layerId == 1 && state.ink[0].originLayer == 0,
          "new ink should retain the active source-layer identity");

    check(state.selectLayer(0, 2), "a foreground layer should be selectable");
    check(state.begin(Device::Mouse, 60, 60), "drawing should begin on the selected layer");
    state.append(Device::Mouse, 80, 80);
    state.finish(Device::Mouse);
    check(state.ink.back().layerId == 2 && state.ink.back().originLayer == 1,
          "new ink should retain the selected source-layer identity");
    state.selectAllOnPage();
    check(state.selected.size() == 1 && state.selected[0] == 1,
          "select-all should only select editable ink on the active layer");

    check(state.setLayerVisible(0, 2, false) && !state.layerVisible(0, 2) && state.selected.empty(),
          "hiding a layer should update visibility and clear stale selection");
    state.selectAllOnPage();
    check(state.selected.empty(), "hidden active-layer ink should not be selected");
    state.undo();
    check(state.layerVisible(0, 2), "undo should restore layer visibility");
    state.redo();
    check(!state.layerVisible(0, 2), "redo should hide the same layer again");
    state.undo();

    check(state.setLayerVisible(1, 2, false), "visibility changes should accept a non-active page index");
    check(!state.layerVisible(1, 2) && state.layerVisible(0, 2),
          "visibility changes should affect only the addressed page");
    state.undo();
    check(state.layerVisible(1, 2) && state.layerVisible(0, 2),
          "undo should restore the addressed page without changing the active page");

    check(state.selectLayer(0, 2) && state.addLayerBelowCurrent(),
          "adding a layer below should insert between the selected layer and its lower neighbor");
    check(state.layersForPage(0).layers.size() == 3 && state.activeLayerId(0) == 2 &&
                  state.activeLayerOrigin(0) == 2 && state.ink[1].layerId == 3,
          "adding a layer below should keep the new layer selected and shift existing object IDs");
    state.undo();
    check(state.layersForPage(0).layers.size() == 2 && state.activeLayerId(0) == 2 && state.ink[1].layerId == 2,
          "undoing a layer insertion below should restore the original stack and object IDs");

    check(state.selectLayer(0, 1) && state.addLayerAboveCurrent(),
          "adding a layer should insert directly above the selected layer");
    check(state.layersForPage(0).layers.size() == 3 && state.activeLayerId(0) == 2 &&
                  state.activeLayerOrigin(0) == 2 && state.ink[1].layerId == 3,
          "adding a layer should preserve existing object identities while shifting layer IDs");
    check(state.begin(Device::Mouse, 100, 100), "drawing should begin on the newly added layer");
    state.append(Device::Mouse, 120, 120);
    state.finish(Device::Mouse);
    check(state.ink.back().layerId == 2 && state.ink.back().originLayer == 2,
          "newly added layers should get a distinct editable origin");

    check(state.deleteCurrentLayer() && state.ink.size() == 2 && state.layersForPage(0).layers.size() == 2,
          "deleting a layer should remove its editable objects");
    state.undo();
    check(state.layersForPage(0).layers.size() == 3 && state.ink.size() == 3 && state.ink.back().layerId == 2,
          "undo should restore the deleted layer and its objects");
    state.redo();
    check(state.layersForPage(0).layers.size() == 2 && state.ink.size() == 2,
          "redo should delete the layer and its objects again");
    state.undo();

    check(state.selectLayer(0, 3) && state.mergeCurrentLayerDown(),
          "a foreground layer should merge into the layer below it");
    check(state.layersForPage(0).layers.size() == 2 && state.activeLayerId(0) == 2 &&
                  state.layersForPage(0).layers[1].origins == std::vector<size_t>({2, 1}) &&
                  state.ink[1].layerId == 2 && state.ink[1].originLayer == 1,
          "merge-down should preserve bottom-to-top source origins and editable object identity");
    state.undo();
    check(state.layersForPage(0).layers.size() == 3 && state.ink[1].layerId == 3,
          "undo should restore the original layer stack and object IDs");
    state.redo();
    check(state.layersForPage(0).layers.size() == 2 && state.ink[1].layerId == 2,
          "redo should reapply merge-down");

    check(state.selectLayer(0, 1) && !state.mergeCurrentLayerDown(),
          "the bottom foreground layer must not merge into the immutable background");
    check(state.setLayerVisible(0, 0, false) && !state.layersForPage(0).backgroundVisible,
          "background visibility should be independently toggleable");
    state.undo();
    check(state.layersForPage(0).backgroundVisible,
          "background visibility should participate in undo history");
}

static void layerRenameDuplicateReorderAndVisibility() {
    CanvasState state;
    state.setPages({{595, 842}, {595, 842}});
    PageLayers source;
    source.initialLayerCount = 3;
    source.selectedLayer = 2;
    source.backgroundName = "Paper";
    source.layers = {{"Lower", true, {0}}, {"Upper", false, {1}}, {"Top", true, {2}}};
    state.setLayerDocument({source, source});

    state.brush = {Tool::Pen, {0, 0, 0}, 2};
    check(state.begin(Device::Mouse, 20, 20), "drawing should begin on the selected layer before duplication");
    state.append(Device::Mouse, 40, 40);
    state.finish(Device::Mouse);
    TextItem text;
    text.page = 0;
    text.text = "layer text";
    check(state.insertText(text), "text should insert on the selected layer before duplication");
    CanvasImage image;
    image.page = 0;
    image.pixelWidth = 2;
    image.pixelHeight = 2;
    image.encodedData = "image bytes";
    check(state.insertImage(image), "image should insert on the selected layer before duplication");

    check(state.renameLayer(0, 0, "Ruled Paper") &&
                  state.layersForPage(0).backgroundName == "Ruled Paper",
          "the background name should be editable");
    state.undo();
    check(state.layersForPage(0).backgroundName == "Paper",
          "undo should restore the background name");
    check(state.renameLayer(0, 2, "Notes") && state.layersForPage(0).layers[1].name == "Notes",
          "the selected foreground layer name should be editable");
    state.undo();
    check(state.layersForPage(0).layers[1].name == "Upper",
          "undo should restore the foreground layer name");

    check(state.duplicateCurrentLayer(), "the selected foreground layer should duplicate");
    const auto& duplicated = state.layersForPage(0);
    check(duplicated.layers.size() == 4 && duplicated.selectedLayer == 3 &&
                  duplicated.layers[1].origins == std::vector<size_t>({1}) &&
                  duplicated.layers[2].origins == std::vector<size_t>({1}) &&
                  !duplicated.layers[2].visible && state.ink.size() == 2 && state.texts.size() == 2 &&
                  state.images.size() == 2,
          "duplicating a layer should copy its identity, visibility, and session ink, text, and images");
    check(state.ink[0].layerId == 2 && state.ink[1].layerId == 3 &&
                  state.ink[0].originLayer == state.ink[1].originLayer &&
                  state.texts[0].layerId == 2 && state.texts[1].layerId == 3 &&
                  state.images[0].layerId == 2 && state.images[1].layerId == 3,
          "duplicated editable objects should retain source origins and receive the new layer ID");
    state.undo();
    check(state.layersForPage(0).layers.size() == 3 && state.ink.size() == 1 && state.texts.size() == 1 &&
                  state.images.size() == 1,
          "undo should remove a duplicate layer and its copied editable objects");
    state.redo();
    check(state.layersForPage(0).layers.size() == 4 && state.ink.size() == 2 && state.texts.size() == 2 &&
                  state.images.size() == 2,
          "redo should restore a duplicate layer and its copied editable objects");

    check(state.moveCurrentLayer(true) && state.activeLayerId(0) == 4 &&
                  state.layersForPage(0).layers[3].origins == std::vector<size_t>({1}) &&
                  state.ink[1].layerId == 4 && state.texts[1].layerId == 4 && state.images[1].layerId == 4,
          "moving a selected layer up should reorder it and remap its editable objects");
    state.undo();
    check(state.activeLayerId(0) == 3 && state.ink[1].layerId == 3 && state.texts[1].layerId == 3 &&
                  state.images[1].layerId == 3,
          "undo should restore layer order and editable object IDs");
    check(state.moveCurrentLayer(false) && state.activeLayerId(0) == 2 && state.ink[1].layerId == 2 &&
                  state.texts[1].layerId == 2 && state.images[1].layerId == 2,
          "moving a selected layer down should remap its editable objects");

    check(state.setAllLayersVisible(0, false) && !state.layerVisible(0, 1) &&
                  !state.layerVisible(0, 2) && !state.layerVisible(0, 3) && state.layerVisible(0, 0),
          "hide all should hide foreground layers without changing background visibility");
    state.undo();
    check(state.layerVisible(0, 1) && !state.layerVisible(0, 2) && !state.layerVisible(0, 3) &&
                  state.layerVisible(0, 4),
          "undo should restore all foreground visibility values");
    check(state.setAllLayersVisible(0, true), "show all should restore every foreground layer");
    check(!state.setAllLayersVisible(0, true), "show all should not create history when no layer changes");
    check(state.layersForPage(1) == source, "layer actions should not modify other pages");
}

static void pageBackgroundHistory() {
    CanvasState state;
    state.setPages({{595, 842}});
    PageLayers page;
    page.backgroundFormat = "plain";
    state.setLayerDocument({page});
    int notifiedPage = -1;
    int notifications = 0;
    state.backgroundChangedCallback = [&](int changedPage) {
        notifiedPage = changedPage;
        ++notifications;
    };

    check(state.changePageBackground(0, "graph", "r1=14.17") && state.revision() == 1 &&
                  state.layersForPage(0).backgroundFormat == "graph" &&
                  state.layersForPage(0).backgroundConfig == "r1=14.17" && notifications == 1 && notifiedPage == 0,
          "page background changes should be undoable and notify the native preview host");
    check(!state.changePageBackground(0, "graph", "r1=14.17") && notifications == 1,
          "setting the current page background to its existing value should not add an edit");
    state.undo();
    check(state.layersForPage(0).backgroundFormat == "plain" && state.canRedo() && notifications == 2,
          "undo should restore the previous page background and refresh its preview");
    state.redo();
    check(state.layersForPage(0).backgroundFormat == "graph" && notifications == 3,
          "redo should restore the new page background and refresh its preview");
}

static void pluginStrokeAndPageSizeHistory() {
    CanvasState state;
    state.setPages({{595, 842}});

    Ink existing{0, {{1, 1}, {2, 2}}, {Tool::Pen, {0, 0, 0}, 1}};
    state.ink.push_back(existing);
    state.selected = {0};
    check(state.addPluginStrokes({
                                        Ink{0, {{10, 20, 0.5}, {30, 40, 0.75}},
                                            Brush{Tool::Pen, {1, 0, 0}, 2}, true, 0, 1, newLayerOrigin, -1, 0},
                                        Ink{0, {{50, 60, 1}, {70, 80, 1}},
                                            Brush{Tool::Highlighter, {1, 1, 0}, 8}, false, 0, 1, newLayerOrigin, 100, 0},
                                },
                                0),
          "grouped plugin stroke insertion failed");
    check(state.ink.size() == 3 && state.selected == std::vector<size_t>{0} && state.ink[1].page == 0 &&
                  state.ink[2].layerOrder == state.ink[1].layerOrder + 1,
          "plugin stroke insertion must target the current layer, preserve selection, and order the batch");
    const auto firstPluginStrokeReference = state.ink[1].pluginReference;
    const auto secondPluginStrokeReference = state.ink[2].pluginReference;
    check(firstPluginStrokeReference != 0 && secondPluginStrokeReference != 0 &&
                  firstPluginStrokeReference != secondPluginStrokeReference,
          "plugin stroke insertion must allocate stable unique element references before recording history");
    state.undo();
    check(state.ink.size() == 1 && state.selected == std::vector<size_t>{0},
          "grouped plugin stroke insertion must undo in one step without changing the prior selection");
    state.redo();
    check(state.ink.size() == 3 && state.ink[1].points[0].pressure == 0.5 && state.ink[2].fill == 100 &&
                  state.ink[1].pluginReference == firstPluginStrokeReference &&
                  state.ink[2].pluginReference == secondPluginStrokeReference,
          "grouped plugin stroke redo lost pressure, highlighter opacity, or opaque references");

    check(state.addPluginStrokes({
                                        Ink{0, {{100, 100, 1}, {110, 110, 1}},
                                            Brush{Tool::Pen, {0, 0, 1}, 1}, false, 0, 1, newLayerOrigin, -1, 0},
                                        Ink{0, {{120, 120, 1}, {130, 130, 1}},
                                            Brush{Tool::Pen, {0, 0, 1}, 1}, false, 0, 1, newLayerOrigin, -1, 0},
                                },
                                1),
          "individual plugin stroke insertion failed");
    check(state.ink.size() == 5, "individual plugin stroke insertion did not add its full batch");
    state.undo();
    check(state.ink.size() == 4, "individual plugin stroke insertion must undo one stroke at a time");
    state.undo();
    check(state.ink.size() == 3, "second individual plugin stroke undo failed");
    state.redo();
    state.redo();

    const auto originalSize = state.pageSize(0);
    check(state.resizePage(0, 320, 480) && state.pageSize(0).width == 320 && state.pageSize(0).height == 480,
          "plugin page resize did not update page geometry");
    state.undo();
    check(state.pageSize(0).width == originalSize.width && state.pageSize(0).height == originalSize.height,
          "plugin page resize undo did not restore original geometry");
    state.redo();
    check(state.pageSize(0).width == 320 && state.pageSize(0).height == 480,
          "plugin page resize redo did not restore the new geometry");

    const auto priorRevision = state.revision();
    check(state.addPluginStrokes({
                                      Ink{0, {{200, 200, 1}, {210, 210, 1}},
                                          Brush{Tool::Pen, {0, 1, 0}, 2}, false, 0, 1, newLayerOrigin, -1, 0},
                              },
                                  2),
          "non-undoable plugin stroke insertion failed");
    const auto irreversibleRevision = state.revision();
    check(state.ink.size() == 6 && irreversibleRevision != priorRevision && !state.canUndo() && !state.canRedo(),
          "non-undoable plugin strokes must mark the document dirty and establish an undo boundary");
    state.undo();
    check(state.ink.size() == 6 && state.revision() == irreversibleRevision,
          "undo must not remove non-undoable plugin strokes or mark their change clean");
    check(state.resizePage(0, 400, 600), "page resize after an irreversible edit failed");
    state.undo();
    check(state.pageSize(0).width == 320 && state.pageSize(0).height == 480 &&
                  state.revision() == irreversibleRevision,
          "undo after an irreversible plugin edit must return to its dirty revision boundary");
}

static void pluginTextBatchHistory() {
    CanvasState state;
    state.setPages({{595, 842}});
    state.ink.push_back(Ink{0, {{1, 1}, {2, 2}}, {Tool::Pen, {0, 0, 0}, 1}});
    state.selected = {0};

    TextItem note;
    note.text = "generated note";
    note.fontName = "Noto Sans";
    note.fontSize = 9.5;
    note.color = 0xff1259b9U;
    note.x = 10;
    note.y = 20;
    note.wrapWidth = 80;
    note.layoutWidth = 80;
    note.layoutHeight = 24;

    TextItem link;
    link.text = "site";
    link.linkUrl = "https://example.test";
    link.isLink = true;
    link.alignment = 1;
    link.x = 50;
    link.y = 60;
    link.layoutWidth = 34;
    link.layoutHeight = 16;

    check(state.addPluginTexts({note, link}, 0) && state.texts.size() == 2 &&
                  state.texts[0].layerId == state.activeLayerId(0) && state.texts[0].layerOrder == 0 &&
                  state.texts[1].layerOrder == 1 && state.texts[0].pluginReference != 0 &&
                  state.texts[1].pluginReference != 0 &&
                  state.texts[0].pluginReference != state.texts[1].pluginReference &&
                  state.selected == std::vector<size_t>{0},
          "grouped plugin text and link insertion must target the current layer and preserve selection");
    const auto noteReference = state.texts[0].pluginReference;
    const auto linkReference = state.texts[1].pluginReference;
    state.undo();
    check(state.texts.empty() && state.selected == std::vector<size_t>{0},
          "grouped plugin text and links must undo in one step without changing prior selection");
    state.redo();
    check(state.texts.size() == 2 && state.texts[0].text == "generated note" && state.texts[1].linkUrl ==
                  "https://example.test" && state.texts[0].pluginReference == noteReference &&
                  state.texts[1].pluginReference == linkReference,
          "grouped plugin text redo must preserve content, link behavior, and opaque references");

    TextItem first;
    first.text = "first";
    TextItem second;
    second.text = "second";
    check(state.addPluginTexts({first, second}, 1) && state.texts.size() == 4,
          "individual plugin text insertion did not add its full batch");
    state.undo();
    check(state.texts.size() == 3 && state.texts.back().text == "first",
          "individual plugin text insertion must undo one object at a time");
    state.redo();
    check(state.texts.size() == 4 && state.texts.back().text == "second",
          "individual plugin text redo did not restore the final object");

    TextItem invalid;
    invalid.text = "invalid alignment";
    invalid.alignment = 4;
    check(!state.addPluginTexts({invalid}, 0) && state.texts.size() == 4,
          "plugin text insertion accepted an unsupported alignment or partially changed the document");

    const auto previousRevision = state.revision();
    TextItem irreversible;
    irreversible.text = "irreversible";
    check(state.addPluginTexts({irreversible}, 2), "non-undoable plugin text insertion failed");
    const auto irreversibleRevision = state.revision();
    check(state.texts.size() == 5 && irreversibleRevision != previousRevision && !state.canUndo() && !state.canRedo(),
          "non-undoable plugin text must mark the document dirty and establish an undo boundary");
    state.undo();
    check(state.texts.size() == 5 && state.revision() == irreversibleRevision,
          "undo must not remove non-undoable plugin text or mark its change clean");
}

static void pluginImageBatchHistory() {
    CanvasState state;
    state.setPages({{595, 842}});
    state.ink.push_back(Ink{0, {{1, 1}, {2, 2}}, {Tool::Pen, {0, 0, 0}, 1}});
    state.selected = {0};

    CanvasImage first;
    first.pixelWidth = 10;
    first.pixelHeight = 20;
    first.encodedData = "first image";
    first.transform = {2, 0, 0, 2, 10, 20};
    CanvasImage second;
    second.pixelWidth = 30;
    second.pixelHeight = 40;
    second.encodedData = "second image";
    second.transform = {1, 0, 0, 1, 40, 50};

    check(state.addPluginImages({first, second}, 0) && state.images.size() == 2 &&
                  state.images[0].layerId == state.activeLayerId(0) && state.images[0].layerOrder == 0 &&
                  state.images[1].layerOrder == 1 && state.images[0].pluginReference != 0 &&
                  state.images[1].pluginReference != 0 &&
                  state.images[0].pluginReference != state.images[1].pluginReference &&
                  state.selected.empty() && state.selectedTexts.empty() &&
                  state.selectedImages == std::vector<size_t>({0, 1}),
          "grouped plugin image insertion must target the current layer and select the new image batch");
    const auto firstReference = state.images[0].pluginReference;
    const auto secondReference = state.images[1].pluginReference;
    state.undo();
    check(state.images.empty() && state.selected == std::vector<size_t>{0} && state.selectedImages.empty(),
          "grouped plugin image insertion must undo in one step and restore the prior selection");
    state.redo();
    check(state.images.size() == 2 && state.images[0].pluginReference == firstReference &&
                  state.images[1].pluginReference == secondReference && state.selectedImages ==
                          std::vector<size_t>({0, 1}),
          "grouped plugin image redo must preserve image data references and selection");

    CanvasImage third;
    third.pixelWidth = 2;
    third.pixelHeight = 3;
    third.encodedData = "third image";
    CanvasImage fourth = third;
    fourth.encodedData = "fourth image";
    check(state.addPluginImages({third, fourth}, 1) && state.images.size() == 4,
          "individual plugin image insertion did not add its full batch");
    state.undo();
    check(state.images.size() == 3 && state.selectedImages == std::vector<size_t>{2},
          "individual plugin image insertion must undo one object at a time and restore prior new selection");
    state.redo();
    check(state.images.size() == 4 && state.selectedImages == std::vector<size_t>({2, 3}),
          "individual plugin image redo did not restore the final image and selection");

    CanvasImage invalid;
    invalid.pixelWidth = 0;
    invalid.pixelHeight = 5;
    invalid.encodedData = "invalid image";
    check(!state.addPluginImages({invalid}, 0) && state.images.size() == 4,
          "plugin image insertion accepted invalid image metadata or partially changed the document");

    const auto previousRevision = state.revision();
    CanvasImage irreversible;
    irreversible.pixelWidth = 1;
    irreversible.pixelHeight = 1;
    irreversible.encodedData = "irreversible image";
    check(state.addPluginImages({irreversible}, 2), "non-undoable plugin image insertion failed");
    const auto irreversibleRevision = state.revision();
    check(state.images.size() == 5 && irreversibleRevision != previousRevision && !state.canUndo() && !state.canRedo(),
          "non-undoable plugin image must mark the document dirty and establish an undo boundary");
    state.undo();
    check(state.images.size() == 5 && state.revision() == irreversibleRevision,
          "undo must not remove non-undoable plugin image or mark its change clean");
}

static void pluginElementReferences() {
    CanvasState state;
    state.setPages({{300, 400}});

    Ink stroke{0, {{10, 10}, {20, 20}}, {Tool::Pen, {0, 0, 0}, 2}};
    stroke.layerOrder = 1;
    state.ink.push_back(stroke);

    TextItem text;
    text.page = 0;
    text.layerOrder = 2;
    text.text = "plugin text";
    state.texts.push_back(text);

    CanvasImage image;
    image.page = 0;
    image.layerOrder = 3;
    image.pixelWidth = 10;
    image.pixelHeight = 10;
    image.encodedData = "image bytes";
    state.images.push_back(image);

    state.ensurePluginReferences();
    const std::vector<std::uintptr_t> references{state.ink[0].pluginReference, state.texts[0].pluginReference,
                                                 state.images[0].pluginReference};
    check(references[0] != 0 && references[1] != 0 && references[2] != 0 &&
                  references[0] != references[1] && references[0] != references[2] &&
                  references[1] != references[2],
          "plugin element references must be nonzero and unique across object types");

    auto editedStroke = state.ink[0];
    editedStroke.brush.color = {1, 0, 0};
    check(state.applyInkEdits({{0, editedStroke}}, {}) && state.ink[0].pluginReference == references[0],
          "stroke value edits must preserve the Lua element reference");
    state.undo();
    check(state.ink[0].pluginReference == references[0], "stroke undo must preserve the Lua element reference");
    state.redo();
    check(state.ink[0].pluginReference == references[0], "stroke redo must preserve the Lua element reference");

    auto editedText = state.texts[0];
    editedText.text = "edited plugin text";
    check(state.replaceText(0, editedText) && state.texts[0].pluginReference == references[1],
          "text replacement must preserve the Lua element reference");
    state.undo();
    check(state.texts[0].pluginReference == references[1], "text undo must preserve the Lua element reference");
    state.redo();
    check(state.texts[0].pluginReference == references[1], "text redo must preserve the Lua element reference");

    auto editedImage = state.images[0];
    editedImage.transform.x = 5;
    check(state.replaceImage(0, editedImage) && state.images[0].pluginReference == references[2],
          "image replacement must preserve the Lua element reference");
    state.undo();
    check(state.images[0].pluginReference == references[2], "image undo must preserve the Lua element reference");
    state.redo();
    check(state.images[0].pluginReference == references[2], "image redo must preserve the Lua element reference");

    for (size_t index = 0; index < 128; ++index) {
        auto extra = stroke;
        extra.layerOrder = index + 4;
        state.ink.push_back(std::move(extra));
    }
    state.ensurePluginReferences();
    check(state.ink[0].pluginReference == references[0] && state.texts[0].pluginReference == references[1] &&
                  state.images[0].pluginReference == references[2],
          "plugin references must survive object-vector growth");

    state.addPluginReferencesToSelection(references);
    check(state.selected == std::vector<size_t>{0} && state.selectedTexts == std::vector<size_t>{0} &&
                  state.selectedImages == std::vector<size_t>{0},
          "plugin references should select matching objects from the active layer");

    state.clearSelection();
    auto duplicate = state.ink[0];
    duplicate.layerOrder = 1000;
    state.ink.push_back(duplicate);
    state.ensurePluginReferences();
    const auto duplicateReference = state.ink.back().pluginReference;
    check(duplicateReference != 0 && duplicateReference != references[0] &&
                  state.ink[0].pluginReference == references[0],
          "duplicated objects must receive a distinct reference without changing the source reference");
    state.addPluginReferencesToSelection({references[0]});
    check(state.selected == std::vector<size_t>{0},
          "a source reference must not select a later duplicate of the object");

    state.clearSelection();
    state.ink.pop_back();
    state.addPluginReferencesToSelection({duplicateReference});
    check(!state.hasSelection(), "deleted object references must not select a different object");
    state.ink.push_back(stroke);
    state.ensurePluginReferences();
    check(state.ink.back().pluginReference != duplicateReference,
          "a deleted plugin reference must not be reused for a later object");
}

static void pdfAnnotationInkHistory() {
    CanvasState state;
    state.setPages({{300, 200}});
    Ink existing{0, {{20, 40}, {80, 40}}, {Tool::Highlighter, {1, 1, 0}, 12}};
    existing.fill = 60;
    existing.capStyle = 1;
    state.ink.push_back(existing);

    auto recolored = existing;
    recolored.brush.color = {0, 1, 0};
    Ink added{0, {{20, 80}, {80, 80}}, {Tool::Highlighter, {0, 0, 1}, 1}};
    added.fill = 230;
    added.capStyle = 1;
    check(state.applyInkEdits({{0, recolored}}, {added}), "PDF annotation edit did not commit");
    check(state.revision() == 1 && state.ink.size() == 2 && state.ink[0].brush.color[1] == 1 &&
                  state.ink[0].fill == 60 && state.ink[0].capStyle == 1 && state.ink[1].fill == 230 &&
                  state.ink[1].capStyle == 1,
          "PDF annotation edit lost opacity, cap style, recolor or insertion");
    state.undo();
    check(state.ink.size() == 1 && state.ink[0].brush.color[0] == 1 && state.ink[0].fill == 60,
          "PDF annotation undo did not restore the prior highlight");
    state.redo();
    check(state.ink.size() == 2 && state.ink[0].brush.color[1] == 1 && state.ink[1].fill == 230,
          "PDF annotation redo did not restore the grouped edit");
}

static void replaceNextPageWithClone() {
    CanvasState state;
    state.setPages({{595, 842}, {595, 842}, {500, 700}});
    PageLayers sourceLayers;
    sourceLayers.initialLayerCount = 2;
    sourceLayers.selectedLayer = 2;
    sourceLayers.backgroundVisible = false;
    sourceLayers.backgroundName = "Source background";
    sourceLayers.layers = {{"Ink", true, {0}}, {"Notes", false, {1}}};
    state.setLayerDocument({sourceLayers, PageLayers{}, PageLayers{}});

    Ink sourceStroke;
    sourceStroke.page = 0;
    sourceStroke.layerId = 2;
    sourceStroke.originLayer = 1;
    sourceStroke.layerOrder = 4;
    sourceStroke.points = {{10, 20, 1}, {30, 40, 1}};
    auto replacedStroke = sourceStroke;
    replacedStroke.page = 1;
    replacedStroke.points[0].x = 200;
    auto followingStroke = sourceStroke;
    followingStroke.page = 2;
    state.ink = {sourceStroke, replacedStroke, followingStroke};

    TextItem sourceText;
    sourceText.page = 0;
    sourceText.layerId = 2;
    sourceText.text = "source page";
    auto replacedText = sourceText;
    replacedText.page = 1;
    replacedText.text = "replaced page";
    auto followingText = sourceText;
    followingText.page = 2;
    followingText.text = "following page";
    state.texts = {sourceText, replacedText, followingText};

    CanvasImage sourceImage;
    sourceImage.page = 0;
    sourceImage.layerId = 2;
    sourceImage.encodedData = "source image";
    auto replacedImage = sourceImage;
    replacedImage.page = 1;
    replacedImage.encodedData = "replaced image";
    auto followingImage = sourceImage;
    followingImage.page = 2;
    followingImage.encodedData = "following image";
    state.images = {sourceImage, replacedImage, followingImage};

    check(state.replaceNextPageWithClone(0, {800, 600}), "cloning should replace an existing next page");
    check(state.pageCount() == 3 && state.page == 1 && state.pages[1].width == 800 && state.pages[1].height == 600,
          "cloning should preserve document length and select the replacement at the target page size");
    auto clonedLayers = sourceLayers;
    clonedLayers.backgroundVisible = true;
    clonedLayers.backgroundName.reset();
    clonedLayers.backgroundFormat = ":pdf";
    for (auto& layer: clonedLayers.layers) {
        layer.visible = true;
    }
    check(state.pageLayers[1] == clonedLayers,
          "cloning should match master page-copy behavior for PDF background and layer visibility");
    check(state.ink.size() == 3 && state.ink[0].page == 0 && state.ink[1].page == 2 && state.ink[2].page == 1 &&
                  state.ink[2].points[0].x == 10 && state.ink[2].layerId == 2 && state.ink[2].layerOrder == 4,
          "cloning should duplicate source ink and discard only the replaced page ink");
    check(state.texts.size() == 3 && state.texts[0].text == "source page" && state.texts[1].text == "following page" &&
                  state.texts[2].page == 1 && state.texts[2].text == "source page",
          "cloning should duplicate source text and preserve following-page text");
    check(state.images.size() == 3 && state.images[0].encodedData == "source image" &&
                  state.images[1].encodedData == "following image" && state.images[2].page == 1 &&
                  state.images[2].encodedData == "source image",
          "cloning should duplicate source images and preserve following-page images");
    check(!state.replaceNextPageWithClone(2, {595, 842}), "cloning must reject the final page without a successor");
}

int main() {
    try {
        savedHistory();
        editableImageHistory();
        groupedImageReplacementHistory();
        multiImagePasteHistory();
        verticalSpaceMovement();
        imageRotationGeometry();
        editableTextHistory();
        latexReplacementHistory();
        selectedTextAppearanceHistory();
        brushAndHistory();
        partialAndWhiteoutEraser();
        continuousMultiPageLayout();
        scrollDeltaUnits();
        pluginBackgroundNameSetter();
        layerManagement();
        replaceNextPageWithClone();
        layerRenameDuplicateReorderAndVisibility();
        pageBackgroundHistory();
        pluginStrokeAndPageSizeHistory();
        pluginTextBatchHistory();
        pluginImageBatchHistory();
        pluginElementReferences();
        pdfAnnotationInkHistory();
        fillStrokesAndSelection();
        check(std::abs(behavior::steppedZoom(1, true, true) - 1.02) < 1e-12,
              "wheel zoom must use the original default two-percent step");
        check(std::abs(behavior::steppedZoom(1, true, false) - 1.1) < 1e-12,
              "keyboard zoom must use the original default ten-percent step");
        check(std::abs(behavior::steppedZoom(behavior::steppedZoom(1, true, false), false, false) - 1) < 1e-12,
              "opposite zoom steps must return to the starting zoom");
        {
            CanvasState customZoomState;
            customZoomState.zoom = 1.0;
            customZoomState.zoomStep = 0.25;
            customZoomState.scrollZoomStep = 0.05;
            customZoomState.minimumPressure = 0.18;
            check(std::abs(customZoomState.steppedZoom(true, false) - 1.25) < 1e-12 &&
                          std::abs(customZoomState.steppedZoom(true, true) - 1.05) < 1e-12,
                  "CanvasState::steppedZoom must honor imported zoomStep and scrollZoomStep");
            check(std::abs(customZoomState.documentPoint(10, 10, 0.02).pressure - 0.18) < 1e-12,
                  "CanvasState::documentPoint must clamp against imported minimumPressure");
        }
        CanvasState state;
        state.fit(1200, 900);
        const auto x = state.offsetX + 100 * state.zoom;
        const auto y = state.offsetY + 120 * state.zoom;
        const auto anchor = state.documentPoint(x, y);
        state.zoomAt(2.5, x, y);
        const auto after = state.documentPoint(x, y);
        check(std::hypot(anchor.x - after.x, anchor.y - after.y) < 1e-9, "zoom must preserve anchor");
        state.zoomAt(std::numeric_limits<double>::infinity(), x, y);
        check(state.zoom == 2.5, "non-finite zoom must be ignored");
        check(!state.begin(Device::Mouse, -10000, -10000), "outside-page ink must be rejected");
        check(state.begin(Device::Stylus, x, y, 0.2), "stylus begins inside page");
        check(!state.begin(Device::Mouse, x, y), "mouse cannot steal stylus stroke");
        state.append(Device::Mouse, x + 20, y + 20);
        state.finish(Device::Mouse);
        check(state.current && state.current->points.size() == 1, "foreign events cannot modify or finish stroke");
        state.cancel(Device::Mouse);
        check(state.current.has_value(), "cancelling a mouse gesture cannot discard stylus ink");
        state.append(Device::Stylus, x + 100, y, 0.8);
        state.zoomAt(4, x, y);
        check(state.zoom == 2.5, "zoom cannot move an active stroke");
        state.finish(Device::Stylus);
        check(state.ink.size() == 1 && state.ink[0].points.back().pressure == 0.8, "pressure survives commit");
        check(state.documentPoint(x, y, std::numeric_limits<double>::quiet_NaN()).pressure == 1,
              "invalid pressure must not poison Cairo drawing");
        state.erase(x + 50, y);
        check(state.ink.size() == 2, "standard eraser splits segment between sparse samples");
        state.undo();
        check(state.ink.size() == 1, "erase undo restores ink");
        state.eraserMode = EraserMode::DeleteStroke;
        state.erase(x + 50, y);
        check(state.ink.empty(), "delete-stroke eraser removes sparse segment");
        state.undo();
        check(state.ink.size() == 1, "delete-stroke undo restores ink");
        state.page = 1;
        state.erase(x + 50, y);
        check(state.ink.size() == 1, "eraser cannot affect another page");
        state.undo();
        check(state.ink.empty(), "stroke undo works across page navigation");
        state.begin(Device::Mouse, x, y);
        state.beginPinch();
        check(!state.current && !state.begin(Device::Stylus, x, y), "pinch cancels ink and owns input");
        check(state.blocksScroll(false, 0) && !state.blocksScroll(true, 0), "pinch blocks smooth scroll only");
        state.endPinch(100);
        check(state.blocksScroll(false, 200099) && !state.blocksScroll(false, 200100), "pinch cooldown is bounded");
        state.begin(Device::Mouse, x, y);
        state.resetInput();
        check(!state.current && !state.pinching && state.scrollBlockedUntil == 0, "focus loss clears input");
        state.fit(400, 500);
        check(state.zoom > 0 && std::isfinite(state.offsetX), "narrow viewport remains valid");
        std::cout
                << "PASS: anchor zoom, device ownership, pressure, erase/undo, page isolation, pinch/cancel, resize\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
