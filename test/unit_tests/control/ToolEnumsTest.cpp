/*
 * Xournal++
 *
 * This file is part of the Xournal UnitTests
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#include <string>

#include <config-test.h>
#include <gtest/gtest.h>

#include "control/ToolEnums.h"

/**
 * Test whether the invariant
 *     fromString(toString(x)) == x
 * holds.
 */
TEST(ToolEnumsTest, testToolSizeSerialization) {
    for (unsigned int i = 0; i <= TOOL_SIZE_NONE; i++) {
        auto toolSize = static_cast<ToolSize>(i);
        std::string s = toolSizeToString(toolSize).data();
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(toolSize, toolSizeFromString(s));
    }
}

/**
 * Test whether the invariant
 *     fromString(toString(x)) == x
 * holds.
 */
TEST(ToolEnumsTest, testToolTypeSerialization) {
    for (unsigned int i = 0; i < TOOL_END_ENTRY; i++) {
        auto toolType = static_cast<ToolType>(i);
        std::string s = toolTypeToString(toolType).data();
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(toolType, toolTypeFromString(s));
    }
}

TEST(ToolEnumsTest, smartSelectKeepsExistingToolIds) {
    EXPECT_EQ(TOOL_SELECT_PDF_TEXT_LINEAR, 21);
    EXPECT_EQ(TOOL_SELECT_PDF_TEXT_RECT, 22);
    EXPECT_EQ(TOOL_LASER_POINTER_PEN, 23);
    EXPECT_EQ(TOOL_LASER_POINTER_HIGHLIGHTER, 24);
    EXPECT_EQ(TOOL_LINK, 25);
    EXPECT_EQ(TOOL_LATEX, 26);
    EXPECT_EQ(TOOL_SMART_SELECT, 27);
    EXPECT_EQ(toolTypeToString(TOOL_SMART_SELECT), "smartSelect");
}

TEST(ToolEnumsTest, smartSelectPredicatesAndCapabilities) {
    EXPECT_TRUE(isSelectToolType(TOOL_SMART_SELECT));
    EXPECT_TRUE(isSelectToolTypeSingleLayer(TOOL_SMART_SELECT));
    EXPECT_TRUE(isSmartSelectToolType(TOOL_SMART_SELECT));
    EXPECT_FALSE(xoj::tool::isPdfSelectionTool(TOOL_SMART_SELECT));
    EXPECT_FALSE(requiresClearedSelection(TOOL_SMART_SELECT));
    EXPECT_EQ(xoj::tool::typeToCapabilities(TOOL_SMART_SELECT), TOOL_CAP_COLOR);
}
