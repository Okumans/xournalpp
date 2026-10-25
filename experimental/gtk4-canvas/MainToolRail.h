#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prototype {

enum class MainToolAction {
    SmartSelect,
    Pen,
    Highlighter,
    Eraser,
    Text,
    Hand,
    Link,
    LaserPointerPen,
    LaserPointerHighlighter,
    PlayObject,
    FloatingToolbox,
    Latex,
};

inline constexpr std::array<MainToolAction, 6> defaultMainToolRail{
        MainToolAction::SmartSelect,
        MainToolAction::Pen,
        MainToolAction::Highlighter,
        MainToolAction::Eraser,
        MainToolAction::Text,
        MainToolAction::Hand,
};

inline constexpr std::array<std::pair<MainToolAction, std::string_view>, 12> mainToolRailNames{{
        {MainToolAction::SmartSelect, "SMART_SELECT"},
        {MainToolAction::Pen, "PEN"},
        {MainToolAction::Highlighter, "HIGHLIGHTER"},
        {MainToolAction::Eraser, "ERASER"},
        {MainToolAction::Text, "TEXT"},
        {MainToolAction::Hand, "HAND"},
        {MainToolAction::Link, "LINK"},
        {MainToolAction::LaserPointerPen, "LASER_POINTER_PEN"},
        {MainToolAction::LaserPointerHighlighter, "LASER_POINTER_HIGHLIGHTER"},
        {MainToolAction::PlayObject, "PLAY_OBJECT"},
        {MainToolAction::FloatingToolbox, "FLOATING_TOOLBOX"},
        {MainToolAction::Latex, "LATEX"},
}};

inline std::string_view mainToolRailToken(MainToolAction action) {
    const auto found = std::find_if(mainToolRailNames.begin(), mainToolRailNames.end(), [action](const auto& entry) {
        return entry.first == action;
    });
    return found == mainToolRailNames.end() ? std::string_view{} : found->second;
}

inline std::optional<MainToolAction> parseMainToolRailToken(std::string_view token) {
    std::string normalized(token);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    const auto found = std::find_if(mainToolRailNames.begin(), mainToolRailNames.end(),
                                    [&normalized](const auto& entry) { return entry.second == normalized; });
    return found == mainToolRailNames.end() ? std::nullopt : std::optional<MainToolAction>{found->first};
}

inline std::optional<std::vector<MainToolAction>> parseMainToolRail(std::string_view value) {
    if (value.empty() || value.size() > 512) {
        return std::nullopt;
    }
    std::vector<MainToolAction> actions;
    bool hasSmartSelect = false;
    size_t begin = 0;
    while (begin < value.size()) {
        const size_t end = value.find(',', begin);
        std::string_view token = value.substr(begin, end == std::string_view::npos ? value.size() - begin :
                                                                                   end - begin);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
            token.remove_prefix(1);
        }
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
            token.remove_suffix(1);
        }
        const auto action = parseMainToolRailToken(token);
        if (!action || std::find(actions.begin(), actions.end(), *action) != actions.end()) {
            return std::nullopt;
        }
        actions.push_back(*action);
        hasSmartSelect |= *action == MainToolAction::SmartSelect;
        if (actions.size() > mainToolRailNames.size()) {
            return std::nullopt;
        }
        if (end == std::string_view::npos) {
            break;
        }
        if (end + 1 == value.size()) {
            return std::nullopt;
        }
        begin = end + 1;
    }
    if (!hasSmartSelect) {
        return std::nullopt;
    }
    return actions;
}

inline std::string serializeMainToolRail(const std::vector<MainToolAction>& actions) {
    std::string result;
    for (const auto action: actions) {
        const auto token = mainToolRailToken(action);
        if (token.empty()) {
            return {};
        }
        if (!result.empty()) {
            result += ',';
        }
        result += token;
    }
    return result;
}

}  // namespace prototype
