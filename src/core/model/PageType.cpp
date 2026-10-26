#include "PageType.h"

#include <glib.h>

PageType::PageType(): format(PageTypeFormat::Lined) {}

PageType::PageType(PageTypeFormat format): format(format) {}

PageType::PageType(const PageType& other) {
    this->format = other.format;
    this->config = other.config;
}

PageType::~PageType() = default;

auto PageType::formatFromString(std::string_view format) -> PageTypeFormat {
    if (format == "plain") return PageTypeFormat::Plain;
    if (format == "ruled") return PageTypeFormat::Ruled;
    if (format == "lined") return PageTypeFormat::Lined;
    if (format == "staves") return PageTypeFormat::Staves;
    if (format == "graph") return PageTypeFormat::Graph;
    if (format == "dotted") return PageTypeFormat::Dotted;
    if (format == "isodotted") return PageTypeFormat::IsoDotted;
    if (format == "isograph") return PageTypeFormat::IsoGraph;
    if (format == ":pdf") return PageTypeFormat::Pdf;
    if (format == ":image") return PageTypeFormat::Image;

    g_warning("PageType::formatFromString: unknown page type \"%.*s\". Replacing with PageTypeFormat::Plain",
              static_cast<int>(format.size()), format.data());
    return PageTypeFormat::Plain;
}

auto PageType::formatToString(PageTypeFormat format) -> std::string {
    switch (format) {
        case PageTypeFormat::Plain:
            return "plain";
        case PageTypeFormat::Ruled:
            return "ruled";
        case PageTypeFormat::Lined:
            return "lined";
        case PageTypeFormat::Staves:
            return "staves";
        case PageTypeFormat::Graph:
            return "graph";
        case PageTypeFormat::Dotted:
            return "dotted";
        case PageTypeFormat::IsoDotted:
            return "isodotted";
        case PageTypeFormat::IsoGraph:
            return "isograph";
        case PageTypeFormat::Pdf:
            return ":pdf";
        case PageTypeFormat::Image:
            return ":image";
    }
    g_warning("PageType::formatToString: unknown page type %d. Replacing with PageTypeFormat::Ruled",
              static_cast<int>(format));
    return "ruled";
}

/**
 * Compare Operator
 */
auto PageType::operator==(const PageType& other) const -> bool {
    return this->config == other.config && this->format == other.format;
}
auto PageType::operator!=(const PageType& other) const -> bool { return !(*this == other); }

/**
 * PDF background
 */
auto PageType::isPdfPage() const -> bool { return this->format == PageTypeFormat::Pdf; }

/**
 * Image Background
 */
auto PageType::isImagePage() const -> bool { return this->format == PageTypeFormat::Image; }

/**
 * Special background
 */
auto PageType::isSpecial() const -> bool {
    return this->format == PageTypeFormat::Pdf || this->format == PageTypeFormat::Image;
}
