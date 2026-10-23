#include "RulerHandler.h"

#include "control/tools/BaseShapeHandler.h"        // for BaseShapeHandler
#include "control/tools/SnapToGridInputHandler.h"  // for SnapToGridInputHan...
#include "model/Point.h"                           // for Point

class XournalView;

RulerHandler::RulerHandler(Control* control, const PageRef& page): BaseShapeHandler(control, page) {}

RulerHandler::~RulerHandler() = default;

auto RulerHandler::createShape(bool isAltDown, bool isShiftDown, bool isControlDown)
        -> std::pair<std::vector<Point>, Range> {
    auto [start, end] = lineEndpoints(isAltDown || isControlDown, isShiftDown);
    Range rg(start.x, start.y);
    rg.addPoint(end.x, end.y);
    return {{start, end}, rg};
}
