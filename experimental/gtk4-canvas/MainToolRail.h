#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
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

inline std::string_view mainToolRailLabel(MainToolAction action) {
    switch (action) {
        case MainToolAction::SmartSelect: return "Smart Select";
        case MainToolAction::Pen: return "Pen";
        case MainToolAction::Highlighter: return "Highlighter";
        case MainToolAction::Eraser: return "Eraser";
        case MainToolAction::Text: return "Text";
        case MainToolAction::Hand: return "Hand";
        case MainToolAction::Link: return "Link";
        case MainToolAction::LaserPointerPen: return "Laser Pointer";
        case MainToolAction::LaserPointerHighlighter: return "Laser Highlighter";
        case MainToolAction::PlayObject: return "Play Object";
        case MainToolAction::FloatingToolbox: return "Floating Toolbox";
        case MainToolAction::Latex: return "LaTeX";
    }
    return {};
}

inline bool moveMainToolRailAction(std::vector<MainToolAction>& actions, MainToolAction action, bool toRail,
                                   std::size_t targetIndex) {
    const auto found = std::find(actions.begin(), actions.end(), action);
    if (!toRail) {
        if (action == MainToolAction::SmartSelect || found == actions.end()) {
            return false;
        }
        actions.erase(found);
        return true;
    }

    const auto oldIndex = found == actions.end() ? actions.size() : static_cast<std::size_t>(found - actions.begin());
    targetIndex = std::min(targetIndex, actions.size());
    if (found != actions.end()) {
        if (targetIndex == oldIndex || targetIndex == oldIndex + 1) {
            return false;
        }
        actions.erase(found);
        if (oldIndex < targetIndex) {
            --targetIndex;
        }
    }
    targetIndex = std::min(targetIndex, actions.size());
    actions.insert(actions.begin() + static_cast<std::ptrdiff_t>(targetIndex), action);
    return true;
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
