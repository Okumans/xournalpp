#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "BeamerPresentation.h"

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    using prototype::beamer::Step;
    const std::vector<std::string> labels{"1", "1", "1", "2", "3", "3"};
    const std::vector<int> pages{0, 1, 2, 3, 4, 5};

    const auto forward = prototype::beamer::step(1, 1, pages, labels);
    check(forward && forward->kind == Step::Kind::ChangePdfBackground && forward->target == 2,
          "Beamer next did not advance the PDF background inside an animation");

    const auto backward = prototype::beamer::step(2, -1, pages, labels);
    check(backward && backward->kind == Step::Kind::ChangePdfBackground && backward->target == 1,
          "Beamer previous did not rewind the PDF background inside an animation");

    const auto nextSlide = prototype::beamer::step(2, 1, pages, labels);
    check(nextSlide && nextSlide->kind == Step::Kind::NavigateDocumentPage && nextSlide->target == 3,
          "Beamer next did not navigate to the next document page at a slide boundary");

    check(!prototype::beamer::step(0, -1, pages, labels), "Beamer previous moved before the first page");
    check(!prototype::beamer::step(5, 1, pages, labels), "Beamer next moved beyond the final page");

    const auto clean = prototype::beamer::animationPagesToRemove(pages, labels);
    check(clean == std::vector<int>({5, 2, 1}), "Beamer clean returned the wrong descending page indices");

    const auto noPdfPages = prototype::beamer::animationPagesToRemove({-1, -1, 0}, labels);
    check(noPdfPages == std::vector<int>({1}), "Beamer clean changed the plugin's nil-label page behavior");

    std::cout << "PASS: Beamer animation navigation and cleanup parity\n";
    return 0;
}
