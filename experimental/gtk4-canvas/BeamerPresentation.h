#pragma once

#include <optional>
#include <string>
#include <vector>

namespace prototype::beamer {

struct Step {
    enum class Kind { ChangePdfBackground, NavigateDocumentPage };

    Kind kind;
    int target;
};

inline std::optional<std::string> labelForPdfPage(int page, const std::vector<std::string>& labels) {
    if (page < 0 || static_cast<std::size_t>(page) >= labels.size()) {
        return std::nullopt;
    }
    return labels[static_cast<std::size_t>(page)];
}

inline std::optional<Step> step(int currentPage, int delta, const std::vector<int>& pageMap,
                                const std::vector<std::string>& labels) {
    if (delta != -1 && delta != 1) {
        return std::nullopt;
    }
    if (currentPage < 0 || static_cast<std::size_t>(currentPage) >= pageMap.size()) {
        return std::nullopt;
    }

    const int pdfPage = pageMap[static_cast<std::size_t>(currentPage)];
    const auto currentLabel = labelForPdfPage(pdfPage, labels);
    const auto nextPdfLabel = labelForPdfPage(pdfPage + delta, labels);
    if (currentLabel && nextPdfLabel && currentLabel == nextPdfLabel) {
        return Step{Step::Kind::ChangePdfBackground, pdfPage + delta};
    }

    const int targetPage = currentPage + delta;
    if (targetPage < 0 || static_cast<std::size_t>(targetPage) >= pageMap.size()) {
        return std::nullopt;
    }
    return Step{Step::Kind::NavigateDocumentPage, targetPage};
}

inline std::vector<int> animationPagesToRemove(const std::vector<int>& pageMap,
                                               const std::vector<std::string>& labels) {
    std::vector<int> pages;
    pages.reserve(pageMap.size());
    for (std::size_t index = pageMap.size(); index > 1; --index) {
        const auto previous = labelForPdfPage(pageMap[index - 2], labels);
        const auto current = labelForPdfPage(pageMap[index - 1], labels);
        // Match the Lua plugin's nil == nil behavior for pages without PDF backgrounds.
        if (previous == current) {
            pages.push_back(static_cast<int>(index - 1));
        }
    }
    return pages;
}

}  // namespace prototype::beamer
