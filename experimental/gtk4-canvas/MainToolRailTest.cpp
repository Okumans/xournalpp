#include <stdexcept>

#include "MainToolRail.h"

using namespace prototype;

static void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

int main() {
    check(serializeMainToolRail(std::vector<MainToolAction>(defaultMainToolRail.begin(), defaultMainToolRail.end())) ==
                  "SMART_SELECT,PEN,HIGHLIGHTER,ERASER,TEXT,HAND",
          "Default tool rail order changed");
    const auto parsed = parseMainToolRail("pen, SMART_SELECT, hand, LINK");
    check(parsed && parsed->size() == 4 && parsed->front() == MainToolAction::Pen &&
                  (*parsed)[1] == MainToolAction::SmartSelect && (*parsed)[3] == MainToolAction::Link,
          "Tool rail order should parse case-insensitive user action names");
    check(!parseMainToolRail("PEN,HIGHLIGHTER"), "Tool rail must keep Smart Select visible");
    check(!parseMainToolRail("SMART_SELECT,PEN,PEN"), "Tool rail must reject duplicates");
    check(!parseMainToolRail("SMART_SELECT,UNKNOWN"), "Tool rail must reject unknown actions");
    check(!parseMainToolRail("SMART_SELECT,,HAND"), "Tool rail must reject empty entries");
    check(!parseMainToolRail(""), "Tool rail must reject an empty layout");
    const auto latex = parseMainToolRail("SMART_SELECT,LATEX");
    check(latex && latex->size() == 2 && latex->back() == MainToolAction::Latex,
          "Tool rail should accept the LaTeX editor action");

    auto actions = std::vector<MainToolAction>(defaultMainToolRail.begin(), defaultMainToolRail.end());
    check(moveMainToolRailAction(actions, MainToolAction::Text, true, 2) &&
                  actions == std::vector<MainToolAction>({MainToolAction::SmartSelect, MainToolAction::Pen,
                                                          MainToolAction::Text, MainToolAction::Highlighter,
                                                          MainToolAction::Eraser, MainToolAction::Hand}),
          "Dropping a visible tool before another tool should reorder the rail");
    check(moveMainToolRailAction(actions, MainToolAction::Link, true, 1) && actions[1] == MainToolAction::Link,
          "Dropping an available tool into the rail should add it at the drop position");
    check(moveMainToolRailAction(actions, MainToolAction::Link, false, 0) &&
                  std::find(actions.begin(), actions.end(), MainToolAction::Link) == actions.end(),
          "Dropping a visible tool into the available list should remove it from the rail");
    check(!moveMainToolRailAction(actions, MainToolAction::SmartSelect, false, 0) &&
                  actions.front() == MainToolAction::SmartSelect,
          "Smart Select must remain on the visible rail");
    const auto beforeNoOp = actions;
    check(!moveMainToolRailAction(actions, MainToolAction::Pen, true, 2) && actions == beforeNoOp,
          "Dropping a tool beside its current position should leave the rail unchanged");
}
