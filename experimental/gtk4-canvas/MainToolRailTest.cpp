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
}
