/*
 * Xournal++
 *
 * Tests for canvas-scoped plugin shortcut parsing and matching.
 *
 * @license GNU GPLv2 or later
 */

#include <gdk/gdkkeysyms.h>
#include <gtest/gtest.h>

#include "plugin/PluginShortcut.h"

TEST(PluginShortcutTest, ParsesPlainKey) {
    const auto shortcut = xoj::plugin::parseShortcut("t");

    ASSERT_TRUE(shortcut);
    EXPECT_EQ(shortcut->keyval, GDK_KEY_t);
    EXPECT_EQ(shortcut->modifiers, GdkModifierType(0));
    EXPECT_TRUE(xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_t, GdkModifierType(0)));
    EXPECT_FALSE(xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_t, GDK_SHIFT_MASK));
}

TEST(PluginShortcutTest, ParsesNamedPunctuationKey) {
    const auto shortcut = xoj::plugin::parseShortcut("slash");

    ASSERT_TRUE(shortcut);
    EXPECT_EQ(shortcut->keyval, GDK_KEY_slash);
    EXPECT_EQ(shortcut->modifiers, GdkModifierType(0));
    EXPECT_TRUE(xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_slash, GdkModifierType(0)));
}

TEST(PluginShortcutTest, MatchesShiftedKeyCaseInsensitively) {
    const auto shortcut = xoj::plugin::parseShortcut("<Shift>j");

    ASSERT_TRUE(shortcut);
    EXPECT_EQ(shortcut->keyval, GDK_KEY_j);
    EXPECT_EQ(shortcut->modifiers, GDK_SHIFT_MASK);
    EXPECT_TRUE(xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_j, GDK_SHIFT_MASK));
    EXPECT_TRUE(xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_J, GDK_SHIFT_MASK));
    EXPECT_TRUE(xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_J, GdkModifierType(0)));
    EXPECT_FALSE(xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_j, GdkModifierType(0)));
    EXPECT_FALSE(
            xoj::plugin::matchesShortcut(*shortcut, GDK_KEY_j, GdkModifierType(GDK_CONTROL_MASK | GDK_SHIFT_MASK)));
}

TEST(PluginShortcutTest, RejectsEmptyAndInvalidShortcuts) {
    EXPECT_FALSE(xoj::plugin::parseShortcut(""));
    EXPECT_FALSE(xoj::plugin::parseShortcut("<Shift>"));
}
