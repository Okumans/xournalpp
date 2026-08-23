/*
 * Xournal++
 *
 * Helpers for canvas-scoped plugin shortcuts.
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <gtk/gtk.h>

namespace xoj::plugin {

struct Shortcut final {
    guint keyval{0};
    GdkModifierType modifiers{};
};

inline auto parseShortcut(std::string_view value) -> std::optional<Shortcut> {
    if (value.empty()) {
        return std::nullopt;
    }

    std::string accelerator(value);
    guint keyval = 0;
    GdkModifierType modifiers{};
    gtk_accelerator_parse(accelerator.c_str(), &keyval, &modifiers);
    if (keyval == 0) {
        return std::nullopt;
    }

    return Shortcut{gdk_keyval_to_lower(keyval),
                    static_cast<GdkModifierType>(modifiers & gtk_accelerator_get_default_mod_mask())};
}

inline auto matchesShortcut(const Shortcut& shortcut, guint keyval, GdkModifierType modifiers) -> bool {
    const auto normalizedModifiers = static_cast<GdkModifierType>(modifiers & gtk_accelerator_get_default_mod_mask());
    if (shortcut.keyval != gdk_keyval_to_lower(keyval)) {
        return false;
    }

    if (shortcut.modifiers == normalizedModifiers) {
        return true;
    }

    // InputContext removes consumed modifiers. A shifted alphabetic key can therefore arrive as an uppercase keyval
    // with Shift absent from the state; retain the explicit Shift binding in that case.
    constexpr auto SHIFT = GdkModifierType(GDK_SHIFT_MASK);
    return (shortcut.modifiers & SHIFT) != 0 && (normalizedModifiers & SHIFT) == 0 &&
           (shortcut.modifiers & ~SHIFT) == normalizedModifiers && gdk_keyval_to_lower(keyval) != keyval;
}

}  // namespace xoj::plugin
