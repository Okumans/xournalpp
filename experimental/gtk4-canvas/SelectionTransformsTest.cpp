#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "CanvasInput.h"
#include "StrokeClipboard.h"

using namespace prototype;

static void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
static bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
static CanvasState fixture() {
    CanvasState state;
    state.setPages({{400, 300}, {300, 200}});
    state.ink = {{0, {{100, 100, .25}, {180, 160, .75}}, {Tool::Pen, {1, 0, 0}, 4}, true},
                 {0, {{240, 110}, {280, 170}}, {Tool::Highlighter, {1, 1, 0}, 14}},
                 {1, {{40, 50}, {100, 70}}, {Tool::Eraser, {1, 1, 1}, 5}}};
    state.brush.tool = Tool::SmartSelect;
    state.selected = {0};
    return state;
}
static CanvasImage imageAt(int page, double x, double y, size_t layerOrder = 0) {
    CanvasImage image;
    image.page = page;
    image.layerId = 1;
    image.originLayer = 0;
    image.layerOrder = layerOrder;
    image.pixelWidth = 20;
    image.pixelHeight = 20;
    image.transform.x = x;
    image.transform.y = y;
    image.encodedData = "selection image fixture";
    return image;
}
static void drag(CanvasState& state, Point from, Point to) {
    auto a = state.pageScreenPoint(state.page, from.x, from.y);
    auto b = state.pageScreenPoint(state.page, to.x, to.y);
    check(state.beginSelection(a.x, a.y), "Gesture did not begin");
    state.moveSelectionPointer(b.x, b.y);
}
int main(int argc, char** argv) {
    try {
        auto state = fixture();
        if (argc == 3 && std::string(argv[1]) == "--encode") {
            state.selected = {0, 1};
            state.ink[1].fill = 60;
            state.ink[1].capStyle = 1;
            auto bytes = clipboard::encode(state.copySelection());
            std::ofstream(argv[2], std::ios::binary).write(bytes.data(), bytes.size());
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--verify") {
            std::ifstream stream(argv[2], std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
            auto strokes = clipboard::decode(bytes);
            check(strokes.size() == 2 && strokes[0].pressureSensitive && near(strokes[0].points[0].pressure, .25) &&
                          strokes[1].brush.tool == Tool::Highlighter && strokes[1].fill == 60 &&
                          strokes[1].capStyle == 1,
                  "Native clipboard round trip lost style, pressure, PDF marker fill or cap style");
            return 0;
        }
        // Side scaling changes one axis; native width changes by sqrt(|sx*sy|).
        drag(state, {180, 130}, {260, 130});
        check(near(state.selectionMatrix(0).xx, 2) && near(state.selectionMatrix(0).yy, 1),
              "Side scaling is not axis-specific");
        check(state.committedInk()[0].brush.width == 4, "Resize preview changed committed ink");
        state.finishSelection();
        check(near(state.ink[0].points[1].x, 260) && near(state.ink[0].brush.width, 4 * std::sqrt(2)),
              "Resize geometry/width mismatch");
        state.undo();
        state.redo();
        check(near(state.ink[0].brush.width, 4 * std::sqrt(2)), "Resize undo/redo changed width");
        state = fixture();
        drag(state, {180, 160}, {260, 220});
        state.finishSelection();
        check(near(state.ink[0].points[1].x, 260) && near(state.ink[0].points[1].y, 220) &&
                      state.ink[0].brush.width == 8,
              "Corner resizing did not preserve aspect ratio");
        state = fixture();
        drag(state, {180, 130}, {20, 130});
        state.finishSelection();
        check(near(state.ink[0].points[1].x, 20) && state.ink[0].brush.width == 4, "Mirror changed width sign");
        state = fixture();
        const auto handle = state.selectionHandles()[8];
        drag(state, handle, {140 - (handle.y - 130), 130 + handle.x - 140});
        state.finishSelection();
        check(near(state.ink[0].points[0].x, 170) && near(state.ink[0].points[0].y, 90) &&
                      state.ink[0].brush.width == 4,
              "Rotation must preserve width and rotate around selection center");
        state = fixture();
        drag(state, {130, 125}, {130, 125});
        const auto destination = state.pageScreenPoint(1, 150, 100);
        state.moveSelectionPointer(destination.x, destination.y);
        check(state.displayPage(0) == 1 && state.committedInk()[0].page == 0,
              "Cross-page preview mutated committed page");
        state.finishSelection();
        check(state.ink.back().page == 1 && state.selected == std::vector<size_t>{2} &&
                      state.ink[1].brush.tool == Tool::Eraser,
              "Transferred selection must be above destination ink");
        state.undo();
        check(state.ink[0].page == 0 && state.selected == std::vector<size_t>{0},
              "Cross-page undo lost order/selection");
        state.redo();
        check(state.ink.back().page == 1, "Cross-page redo failed");

        state = fixture();
        state.selected = {0, 1};
        const auto groupStart = state.pageScreenPoint(0, 130, 130);
        const auto groupEnd = state.pageScreenPoint(1, 150, 100);
        check(state.beginSelection(groupStart.x, groupStart.y), "Grouped cross-page move did not begin");
        state.moveSelectionPointer(groupEnd.x, groupEnd.y);
        state.finishSelection();
        check(state.ink.size() == 3 && state.ink[0].brush.color[2] == 1 && state.ink[1].brush.tool == Tool::Pen &&
                      state.ink[2].brush.tool == Tool::Highlighter && state.selected == std::vector<size_t>({1, 2}),
              "Cross-page group changed destination order or selection");
        state.undo();
        check(state.ink[0].page == 0 && state.ink[1].page == 0 && state.ink[2].page == 1 &&
                      state.selected == std::vector<size_t>({0, 1}),
              "Grouped cross-page undo failed");
        state.redo();
        check(state.ink[0].page == 1 && state.ink[1].page == 1 && state.ink[2].page == 1,
              "Grouped cross-page redo failed");

        state = fixture();
        state.clearSelection();
        state.ink.insert(state.ink.begin() + 1, state.ink[0]);
        drag(state, {140, 130}, {160, 150});
        state.finishSelection();
        check(state.selected == std::vector<size_t>{1} && state.ink[0].points[0].x == 100 &&
                      state.ink[1].points[0].x == 120,
              "Smart Select must move only the topmost hit stroke");
        state = fixture();
        state.clearSelection();
        state.brush.tool = Tool::SelectLasso;
        check(state.beginSelection(90, 90), "Lasso did not begin");
        for (const auto p: {Point{190, 90}, Point{190, 180}, Point{90, 180}, Point{90, 90}})
            state.moveSelectionPointer(p.x, p.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{0}, "Lasso winding containment failed");
        state.clearSelection();
        state.ink.push_back({0, {{.2, 98}, {30, 98}}, {Tool::Pen, {0, 0, 0}, 2}});
        check(state.beginSelection(.5, 90), "Page-edge lasso did not begin");
        for (const auto p: {Point{80, 90}, Point{80, 110}, Point{.5, 110}}) state.moveSelectionPointer(p.x, p.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{3}, "Lasso edge extension failed to include a touching stroke");
        state.clearSelection();
        state.brush.tool = Tool::SelectLasso;
        check(state.beginSelection(90, 90), "Second lasso did not begin");
        for (const auto p: {Point{190, 90}, Point{190, 180}, Point{90, 180}, Point{90, 90}})
            state.moveSelectionPointer(p.x, p.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{0}, "Lasso selection changed after page-edge projection");
        CanvasInput input(state);
        input.shiftHeld = true;
        input.begin(Device::Mouse, {230, 100, {}});
        input.motion(Device::Mouse, {290, 180, {}});
        input.finish(Device::Mouse);
        check(state.selected == std::vector<size_t>({0, 1}), "Shift rectangle must add selection even in lasso tool");

        state = fixture();
        state.texts.push_back({0, 0, 1, 0, "selected text", "Sans", "", false, 12, 0xff000000U, -1, 0, false,
                               1, 0, 0, 1, 120, 130, 70, 20, {}});
        state.images.push_back(imageAt(0, 195, 130, 2));
        state.brush.tool = Tool::SelectRectangle;
        const auto textSelectStart = state.pageScreenPoint(0, 95, 90);
        const auto textSelectEnd = state.pageScreenPoint(0, 225, 180);
        check(state.beginSelection(textSelectStart.x, textSelectStart.y), "Mixed-object selection did not begin");
        state.moveSelectionPointer(textSelectEnd.x, textSelectEnd.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{0} && state.selectedTexts == std::vector<size_t>{0} &&
                      state.selectedImages == std::vector<size_t>{0} && state.selectionCount() == 3,
              "Rectangle selection did not include the stroke, native text, and image objects");
        const auto mixedStart = state.pageScreenPoint(0, 140, 140);
        const auto mixedEnd = state.pageScreenPoint(0, 155, 160);
        check(state.beginSelection(mixedStart.x, mixedStart.y), "Mixed-object move did not begin");
        state.moveSelectionPointer(mixedEnd.x, mixedEnd.y);
        check(state.texts[0].x == 120 && state.texts[0].y == 130 &&
                      state.committedInk()[0].points[0].x == 100,
              "Mixed-object move preview changed committed document objects");
        state.finishSelection();
        check(state.texts[0].x == 135 && state.texts[0].y == 150 &&
                      state.ink[0].points[0].x == 115 && state.images[0].transform.x == 210 &&
                      state.images[0].transform.y == 150 && state.selectedTexts == std::vector<size_t>{0} &&
                      state.selectedImages == std::vector<size_t>{0},
              "Moving a mixed selection did not transform text, ink, and image together");
        state.undo();
        check(state.texts[0].x == 120 && state.texts[0].y == 130 &&
                      state.ink[0].points[0].x == 100 && state.images[0].transform.x == 195 &&
                      state.images[0].transform.y == 130 && state.selectedTexts == std::vector<size_t>{0} &&
                      state.selectedImages == std::vector<size_t>{0},
              "Mixed-object move undo did not restore geometry and selection");
        state.redo();
        check(state.texts[0].x == 135 && state.ink[0].points[0].x == 115 && state.images[0].transform.x == 210,
              "Mixed-object move redo failed");
        state.deleteSelection();
        check(state.texts.empty() && state.ink.size() == 2 && state.images.empty() && !state.hasSelection(),
              "Delete did not remove every object in the mixed selection");
        state.undo();
        check(state.texts.size() == 1 && state.ink.size() == 3 && state.selectedTexts == std::vector<size_t>{0} &&
                      state.selected == std::vector<size_t>{0} && state.images.size() == 1 &&
                      state.selectedImages == std::vector<size_t>{0},
              "Mixed-object delete undo did not restore objects and selection");

        state = fixture();
        state.selected.clear();
        state.images.push_back(imageAt(0, 180, 110, 3));
        state.brush.tool = Tool::SelectRectangle;
        const auto imageSelectStart = state.pageScreenPoint(0, 175, 105);
        const auto imageSelectEnd = state.pageScreenPoint(0, 205, 135);
        check(state.beginSelection(imageSelectStart.x, imageSelectStart.y), "Image rectangle selection did not begin");
        state.moveSelectionPointer(imageSelectEnd.x, imageSelectEnd.y);
        state.finishSelection();
        check(state.selectedImages == std::vector<size_t>{0} && state.selectionCount() == 1,
              "Rectangle selection did not select the fully contained image");
        const auto imageMoveStart = state.pageScreenPoint(0, 190, 120);
        const auto imageMoveEnd = state.pageScreenPoint(0, 200, 135);
        check(state.beginSelection(imageMoveStart.x, imageMoveStart.y), "Selected image move did not begin");
        state.moveSelectionPointer(imageMoveEnd.x, imageMoveEnd.y);
        check(near(state.displayImage(0).transform.x, 190) && near(state.displayImage(0).transform.y, 125) &&
                      near(state.images[0].transform.x, 180),
              "Image move preview changed committed data or showed the wrong transform");
        state.finishSelection();
        check(near(state.images[0].transform.x, 190) && near(state.images[0].transform.y, 125),
              "Finishing an image move did not commit its affine transform");
        state.undo();
        check(near(state.images[0].transform.x, 180) && state.selectedImages == std::vector<size_t>{0},
              "Image move undo did not restore its transform and selection");
        state.redo();
        check(near(state.images[0].transform.x, 190), "Image move redo failed");

        const auto imageResizeHandle = state.selectionHandles()[4];
        drag(state, imageResizeHandle, {250, 185});
        state.finishSelection();
        check(near(state.images[0].transform.xx, 3) && near(state.images[0].transform.yy, 3) &&
                      near(state.images[0].transform.x, 190) && near(state.images[0].transform.y, 125),
              "Shared selection resize did not scale an image around its opposite corner");
        state.undo();
        check(near(state.images[0].transform.xx, 1), "Image resize undo failed");
        state.redo();

        const auto imageCenter = canvasImageCenter(state.images[0]);
        const auto imageRotationHandle = state.selectionHandles()[8];
        const double rotationRadius = std::hypot(imageRotationHandle.x - imageCenter.x,
                                                 imageRotationHandle.y - imageCenter.y);
        const auto rotationStart = state.pageScreenPoint(0, imageRotationHandle.x, imageRotationHandle.y);
        const auto rotationEnd = state.pageScreenPoint(0, imageCenter.x, imageCenter.y + rotationRadius);
        check(state.beginSelection(rotationStart.x, rotationStart.y), "Image rotation handle did not begin a gesture");
        state.moveSelectionPointer(rotationEnd.x, rotationEnd.y);
        state.finishSelection();
        check(near(state.images[0].transform.xx, 0) && near(state.images[0].transform.yx, 3) &&
                      near(state.images[0].transform.xy, -3) && near(state.images[0].transform.yy, 0),
              "Shared rotation handle did not rotate an image around its selection center");
        state.undo();
        check(near(state.images[0].transform.xx, 3) && near(state.images[0].transform.yy, 3),
              "Image rotation undo failed");
        state.redo();

        const auto altCenter = canvasImageCenter(state.images[0]);
        const auto altHandle = state.selectionHandles()[8];
        const double altRadius = std::hypot(altHandle.x - altCenter.x, altHandle.y - altCenter.y);
        const double altStartAngle = std::atan2(altHandle.y - altCenter.y, altHandle.x - altCenter.x);
        const double altDelta = 2 * std::acos(-1.0) / 180.0;
        const auto altStart = state.pageScreenPoint(0, altHandle.x, altHandle.y);
        const auto altEnd = state.pageScreenPoint(0, altCenter.x + altRadius * std::cos(altStartAngle + altDelta),
                                                  altCenter.y + altRadius * std::sin(altStartAngle + altDelta));
        check(state.beginSelection(altStart.x, altStart.y), "Alt image rotation did not begin");
        state.moveSelectionPointer(altEnd.x, altEnd.y, true);
        state.finishSelection();
        check(near(std::atan2(state.images[0].transform.yx, state.images[0].transform.xx),
                   std::acos(-1.0) / 2 + altDelta),
              "Alt did not disable shared image-selection rotation snapping");
        state.undo();

        const auto crossPageStart = state.pageScreenPoint(0, imageCenter.x, imageCenter.y);
        const auto crossPageEnd = state.pageScreenPoint(1, 150, 100);
        check(state.beginSelection(crossPageStart.x, crossPageStart.y), "Image cross-page move did not begin");
        state.moveSelectionPointer(crossPageEnd.x, crossPageEnd.y);
        check(state.displayImage(0).page == 1 && state.images[0].page == 0,
              "Image cross-page preview mutated the source page");
        state.finishSelection();
        check(state.images.size() == 1 && state.images[0].page == 1 &&
                      state.selectedImages == std::vector<size_t>{0},
              "Cross-page selection did not transfer and reselect an image");
        state.undo();
        check(state.images[0].page == 0 && state.selectedImages == std::vector<size_t>{0},
              "Cross-page image undo did not restore its page and selection");
        state.redo();
        state.deleteSelection();
        check(state.images.empty() && !state.hasSelection(), "Delete did not remove a selected image");
        state.undo();
        check(state.images.size() == 1 && state.selectedImages == std::vector<size_t>{0},
              "Image delete undo did not restore the image and selection");

        state = fixture();
        state.pageLayers[0].initialLayerCount = 2;
        state.pageLayers[0].selectedLayer = 2;
        state.pageLayers[0].layers = {{"Bottom", true, {0}}, {"Top", true, {1}}};
        state.images.push_back(imageAt(0, 190, 120, 1));
        state.images[0].layerId = 2;
        state.images[0].originLayer = 1;
        state.selectionAcrossLayers = true;
        state.brush.tool = Tool::SelectRectangle;
        const auto layeredStart = state.pageScreenPoint(0, 95, 90);
        const auto layeredEnd = state.pageScreenPoint(0, 225, 180);
        check(state.beginSelection(layeredStart.x, layeredStart.y), "Visible-layer image selection did not begin");
        state.moveSelectionPointer(layeredEnd.x, layeredEnd.y);
        state.finishSelection();
        check(state.selectedImages == std::vector<size_t>{0} && state.selected.empty(),
              "Visible-layer selection did not stop at a matching upper-layer image");
        state.pageLayers[0].layers[1].visible = false;
        check(state.beginSelection(layeredStart.x, layeredStart.y), "Hidden-layer fallback selection did not begin");
        state.moveSelectionPointer(layeredEnd.x, layeredEnd.y);
        state.finishSelection();
        check(state.selected == std::vector<size_t>{0} && state.selectedImages.empty(),
              "A hidden upper-layer image leaked into visible-layer fallback selection");

        state = fixture();
        state.clearSelection();
        state.images.push_back(imageAt(0, 100, 100, 3));
        state.texts.push_back({0, 0, 1, 0, "topmost", "Sans", "", false, 12, 0xff000000U, -1, 0, false,
                               1, 0, 0, 1, 100, 100, 20, 20, {}});
        state.texts[0].layerOrder = 4;
        const auto overlap = state.pageScreenPoint(0, 110, 110);
        check(state.beginSelection(overlap.x, overlap.y), "Overlapping Smart Select did not begin");
        state.finishSelection();
        check(state.selectedTexts == std::vector<size_t>{0} && state.selectedImages.empty(),
              "Smart Select did not choose the highest ordered overlapping text object");
        state.clearSelection();
        state.texts[0].layerOrder = 2;
        check(state.beginSelection(overlap.x, overlap.y), "Image Smart Select did not begin");
        state.finishSelection();
        check(state.selectedImages == std::vector<size_t>{0} && state.selectedTexts.empty(),
              "Smart Select did not choose the higher ordered overlapping image");

        state = fixture();
        state.texts.push_back({0, 0, 1, 0, "cross page", "Sans", "", false, 12, 0xff000000U, -1, 0, false,
                               1, 0, 0, 1, 210, 110, 65, 20, {}});
        state.selected.clear();
        state.selectedTexts = {0};
        const auto textMoveStart = state.pageScreenPoint(0, 230, 118);
        const auto textMoveEnd = state.pageScreenPoint(1, 140, 100);
        check(state.beginSelection(textMoveStart.x, textMoveStart.y), "Text cross-page move did not begin");
        state.moveSelectionPointer(textMoveEnd.x, textMoveEnd.y);
        state.finishSelection();
        check(state.texts.size() == 1 && state.texts[0].page == 1 && state.selectedTexts == std::vector<size_t>{0},
              "Cross-page move did not transfer selected text to the target page");
        state.undo();
        check(state.texts.size() == 1 && state.texts[0].page == 0 && state.selectedTexts == std::vector<size_t>{0},
              "Cross-page text undo did not restore its page and selection");

        state = fixture();
        state.texts.push_back({0, 0, 1, 0, "resize text", "Sans", "", false, 12, 0xff000000U, -1, 0, false,
                               1, 0, 0, 1, 200, 80, 60, 20, {}});
        state.selected.clear();
        state.selectedTexts = {0};
        const auto resizeHandle = state.selectionHandles()[4];
        drag(state, resizeHandle, {320, 120});
        state.finishSelection();
        check(near(state.texts[0].xx, 2) && near(state.texts[0].yy, 2) &&
                      state.texts[0].x == 200 && state.texts[0].y == 80,
              "Text corner resize did not preserve its anchor and apply a uniform affine scale");
        state.undo();
        check(state.texts[0].xx == 1 && state.texts[0].yy == 1 && state.selectedTexts == std::vector<size_t>{0},
              "Text resize undo failed");

        state = fixture();
        state.texts.push_back({0, 0, 1, 0, "rotate text", "Sans", "", false, 12, 0xff000000U, -1, 0, false,
                               1, 0, 0, 1, 200, 80, 60, 20, {}});
        state.selected.clear();
        state.selectedTexts = {0};
        const auto rotateHandle = state.selectionHandles()[8];
        drag(state, rotateHandle, {230, 144});
        state.finishSelection();
        check(near(state.texts[0].xx, 0) && near(state.texts[0].yx, 1) &&
                      near(state.texts[0].xy, -1) && near(state.texts[0].yy, 0),
              "Text rotation did not compose the shared selection transform");
        state.undo();
        check(state.texts[0].xx == 1 && state.texts[0].yy == 1,
              "Text rotation undo failed");

        state = fixture();
        state.selected = {0, 1};
        const auto bytes = clipboard::encode(state.copySelection());
        const auto copy = clipboard::decode(bytes);
        check(copy.size() == 2 && copy[0].points[1].pressure == .75 && copy[1].brush.width == 14,
              "Clipboard lost pressure/style");
        const auto payload = clipboard::decodePayload(bytes);
        check(payload.strokes.size() == 2 && !payload.image,
              "Native stroke clipboard payload decoded with the wrong object type");
        CanvasImage clipboardImage;
        clipboardImage.pixelWidth = 320;
        clipboardImage.pixelHeight = 180;
        clipboardImage.transform = {0.75, 0.25, -0.25, 0.75, 122, 86};
        clipboardImage.encodedData = "bounded image bytes";
        const auto imageBytes = clipboard::encodeImage(clipboardImage);
        const auto imageCopy = clipboard::decodeImage(imageBytes);
        const auto imagePayload = clipboard::decodePayload(imageBytes);
        check(imageCopy.pixelWidth == 320 && imageCopy.pixelHeight == 180 &&
                      near(imageCopy.transform.xx, 0.75) && near(imageCopy.transform.yx, 0.25) &&
                      near(imageCopy.transform.xy, -0.25) && near(imageCopy.transform.yy, 0.75) &&
                      near(imageCopy.transform.x, 122) && near(imageCopy.transform.y, 86) &&
                      imageCopy.encodedData == clipboardImage.encodedData && imagePayload.image &&
                      imagePayload.strokes.empty(),
              "Native image clipboard round trip lost its raster data or transform");
        for (size_t length = 0; length < imageBytes.size(); ++length) {
            bool rejectedImage = false;
            try {
                clipboard::decodeImage(std::string_view(imageBytes).substr(0, length));
            } catch (const std::exception&) {
                rejectedImage = true;
            }
            check(rejectedImage, "A truncated native image clipboard was accepted");
        }
        for (size_t length = 0; length < bytes.size(); ++length) {
            bool rejected = false;
            try {
                clipboard::decode(std::string_view(bytes).substr(0, length));
            } catch (const std::exception&) {
                rejected = true;
            }
            check(rejected, "A truncated clipboard was accepted");
        }
        auto oversized = bytes;
        const auto block = oversized.find("_b");
        const auto huge = std::numeric_limits<size_t>::max();
        std::memcpy(oversized.data() + block + 2, &huge, sizeof(huge));
        bool rejected = false;
        try {
            clipboard::decode(oversized);
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Overflowing clipboard array count was accepted");
        rejected = false;
        try {
            clipboard::decode(bytes + "trailing");
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "Trailing clipboard data accepted");

        state = fixture();
        state.selected.clear();
        state.selectedTexts.clear();
        state.selectedImages.clear();
        state.brush.tool = Tool::SelectObject;
        state.ink = {{0, {{50, 100}, {150, 100}}, {Tool::Pen, {0, 0, 0}, 1}, false, 0, 1, 0},
                     {0, {{50, 101.5}, {150, 101.5}}, {Tool::Pen, {0, 0, 0}, 1}, false, 1, 1, 0}};
        check(state.selectObjectAt(0, {100, 100.7}) && state.selected == std::vector<size_t>{0},
              "Select Object must choose the closest stroke near overlapping edges");
        state.ink[1].points = {{50, 100.2}, {150, 100.2}};
        check(state.selectObjectAt(0, {100, 100.3}) && state.selected == std::vector<size_t>{1},
              "Select Object must choose the frontmost object when multiple hits contain the pointer");

        TextItem selectableText;
        selectableText.page = 0;
        selectableText.layerId = 1;
        selectableText.originLayer = 0;
        selectableText.layerOrder = 2;
        selectableText.text = "selectable text";
        selectableText.layoutWidth = 60;
        selectableText.layoutHeight = 20;
        selectableText.x = 180;
        selectableText.y = 180;
        state.texts = {selectableText};
        state.images = {imageAt(0, 190, 180, 3)};
        const auto nearTextEdge = state.pickSelectableObject(0, {240.25, 190});
        check(nearTextEdge && nearTextEdge->kind == SelectableObjectKind::Text &&
                      !state.pickSelectableObject(0, {240.6, 190}),
              "Select Object rectangular hit testing did not use the 0.5 point action radius");
        check(state.selectObjectAt(0, {195, 185}) && state.selected.empty() && state.selectedTexts.empty() &&
                      state.selectedImages == std::vector<size_t>{0},
              "Select Object did not prefer a frontmost native image over overlapping text");
        check(state.selectObjectAt(0, {185, 185}, true) && state.selectedTexts == std::vector<size_t>{0} &&
                      state.selectedImages == std::vector<size_t>{0},
              "Shift-click Select Object did not add native text to the existing selection");
        check(!state.selectObjectAt(0, {350, 280}, true) && state.selectedTexts == std::vector<size_t>{0} &&
                      state.selectedImages == std::vector<size_t>{0},
              "Shift-click on empty space should preserve the existing selection");
        check(!state.selectObjectAt(0, {350, 280}) && !state.hasSelection(),
              "Select Object on empty space should clear the old selection");

        state = fixture();
        state.selected.clear();
        state.brush.tool = Tool::SelectObject;
        CanvasInput objectInput(state);
        const auto objectStart = state.pageScreenPoint(0, 140, 130);
        const auto objectEnd = state.pageScreenPoint(0, 160, 150);
        objectInput.begin(Device::Mouse, {objectStart.x, objectStart.y, {}});
        objectInput.motion(Device::Mouse, {objectEnd.x, objectEnd.y, {}});
        objectInput.finish(Device::Mouse);
        check(state.selected == std::vector<size_t>{0} && near(state.ink[0].points[0].x, 100),
              "Select Object should select on the first click without dragging the newly selected object");
        const auto moveStart = state.pageScreenPoint(0, 130, 130);
        const auto moveEnd = state.pageScreenPoint(0, 150, 145);
        objectInput.begin(Device::Mouse, {moveStart.x, moveStart.y, {}});
        objectInput.motion(Device::Mouse, {moveEnd.x, moveEnd.y, {}});
        objectInput.finish(Device::Mouse);
        check(near(state.ink[0].points[0].x, 120) && near(state.ink[0].points[0].y, 115) &&
                      state.selected == std::vector<size_t>{0},
              "Select Object should preserve shared selection-handle move behavior");

        state = fixture();
        check(state.pasteStrokes(copy, 1, {150, 100}), "Paste failed");
        check(state.selected == std::vector<size_t>({3, 4}) && state.ink.size() == 5,
              "Paste did not select appended group");
        state.undo();
        check(state.ink.size() == 3 && state.selected == std::vector<size_t>{0}, "Paste undo lost previous selection");
        state.redo();
        check(state.ink.size() == 5 && state.selected.size() == 2, "Paste redo failed");
        const auto revision = state.revision();
        check(!state.pasteStrokes({Ink{0, {}, {}}}, 0, {0, 0}) && state.revision() == revision &&
                      state.selected.size() == 2,
              "Malformed paste must be atomic");
        state = fixture();
        state.ink[0].points = {{100, 100}, {100, 100}};
        const auto dotHandle = state.selectionHandles()[4];
        drag(state, dotHandle, {100, 100});
        state.finishSelection();
        check(std::isfinite(state.ink[0].brush.width) && state.ink[0].brush.width > 0,
              "Degenerate dot resize broke width");
        std::cout << "PASS: affine geometry/width, mirror, rotate, cross-page order/undo, Smart Select, lasso, "
                     "Shift-add, bounded native stroke/image clipboard and paste undo\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
