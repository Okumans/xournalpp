#pragma once

#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace prototype {

using LineDashPattern = std::vector<double>;

inline constexpr std::size_t maxLineDashValues = 64;

inline bool validLineDashPattern(const LineDashPattern& pattern) {
    if (pattern.size() > maxLineDashValues) {
        return false;
    }

    double period = 0;
    for (const double dash: pattern) {
        if (!std::isfinite(dash) || dash < 0) {
            return false;
        }
        period += dash;
        if (!std::isfinite(period)) {
            return false;
        }
    }
    return pattern.empty() || period > 0;
}

inline std::optional<LineDashPattern> parseLineStyle(std::string_view value) {
    if (value == "plain" || value == "solid" || value == "none") {
        return LineDashPattern{};
    }
    if (value == "dash") {
        return LineDashPattern{6, 3};
    }
    if (value == "dashdot") {
        return LineDashPattern{6, 3, 0.5, 3};
    }
    if (value == "dot") {
        return LineDashPattern{0.5, 3};
    }
    constexpr std::string_view customPrefix = "cust: ";
    if (!value.starts_with(customPrefix)) {
        return LineDashPattern{};
    }

    std::istringstream stream{std::string(value.substr(customPrefix.size()))};
    stream.imbue(std::locale::classic());
    LineDashPattern result;
    for (double dash; stream >> dash;) {
        if (result.size() == maxLineDashValues) {
            return std::nullopt;
        }
        result.push_back(dash);
    }
    if (!stream.eof() || !validLineDashPattern(result)) {
        return std::nullopt;
    }
    return result;
}

inline std::string formatLineStyle(const LineDashPattern& pattern) {
    if (pattern.empty()) {
        return "plain";
    }
    if (pattern == LineDashPattern{6, 3}) {
        return "dash";
    }
    if (pattern == LineDashPattern{6, 3, 0.5, 3}) {
        return "dashdot";
    }
    if (pattern == LineDashPattern{0.5, 3}) {
        return "dot";
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "cust: " << std::fixed << std::setprecision(2);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (i) {
            stream << ' ';
        }
        stream << pattern[i];
    }
    return stream.str();
}

}  // namespace prototype
