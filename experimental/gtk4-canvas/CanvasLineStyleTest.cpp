#include <iostream>
#include <sstream>
#include <string>

#include "CanvasLineStyle.h"

int main() {
    using prototype::LineDashPattern;
    using prototype::parseLineStyle;

    bool pass = true;
    const auto check = [&pass](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            pass = false;
        }
    };

    check(parseLineStyle("plain") == LineDashPattern{}, "plain parses as a solid line");
    check(parseLineStyle("dash") == LineDashPattern{6, 3}, "dash uses the master pattern");
    check(parseLineStyle("dashdot") == LineDashPattern{6, 3, 0.5, 3}, "dash-dot uses the master pattern");
    check(parseLineStyle("dot") == LineDashPattern{0.5, 3}, "dot uses the master pattern");
    check(parseLineStyle("unknown") == LineDashPattern{}, "unknown legacy names fall back to solid");

    const auto custom = parseLineStyle("cust: 1.25 2.50");
    check(custom == LineDashPattern{1.25, 2.5}, "custom patterns parse decimal values");
    check(custom && prototype::formatLineStyle(*custom) == "cust: 1.25 2.50",
          "custom patterns format with the master precision");
    check(parseLineStyle("cust: 0 3") == LineDashPattern{0, 3}, "zero-length custom entries remain supported");
    check(parseLineStyle("cust: ") == LineDashPattern{}, "empty custom patterns fall back to solid");
    check(!parseLineStyle("cust: 0 0"), "zero-period patterns are rejected");
    check(!parseLineStyle("cust: -1 3"), "negative custom entries are rejected");
    check(!parseLineStyle("cust: 1 nope"), "trailing nonnumeric custom values are rejected");
    check(!parseLineStyle("cust: nan 1"), "nonfinite custom values are rejected");

    std::ostringstream tooMany;
    tooMany << "cust:";
    for (std::size_t i = 0; i <= prototype::maxLineDashValues; ++i) {
        tooMany << " 1";
    }
    check(!parseLineStyle(tooMany.str()), "custom patterns over the configured bound are rejected");

    std::cout << (pass ? "PASS" : "FAIL") << ": Pen line-style parsing and formatting\n";
    return pass ? 0 : 1;
}
