#include "CanvasSearch.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const auto ascii = prototype::search::caseInsensitiveMatches("Hello hello", "HELLO");
        require(ascii == std::vector<prototype::search::TextRange>{{0, 5}, {6, 11}},
                "Case-insensitive search did not return UTF-8 byte ranges for repeated ASCII matches");

        const auto expandedFold = prototype::search::caseInsensitiveMatches("Straße", "STRASSE");
        require(expandedFold == std::vector<prototype::search::TextRange>{{0, 7}},
                "Unicode case-fold expansion did not map back to the original UTF-8 byte range");

        const auto cjk = prototype::search::caseInsensitiveMatches("猫猫", "猫");
        require(cjk == std::vector<prototype::search::TextRange>{{0, 3}, {3, 6}},
                "UTF-8 search did not preserve code-point boundaries for CJK text");

        require(prototype::search::caseInsensitiveMatches("text", "").empty(),
                "Empty search query produced results");
        const std::string invalidUtf8{"\xff", 1};
        require(prototype::search::caseInsensitiveMatches(invalidUtf8, "x").empty(),
                "Invalid UTF-8 text was accepted by search");
    } catch (const std::exception& error) {
        std::cerr << "CanvasSearchTest: " << error.what() << '\n';
        return 1;
    }

    std::cout << "PASS: UTF-8 case-insensitive text search ranges\n";
    return 0;
}
