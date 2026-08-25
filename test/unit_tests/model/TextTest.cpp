#include <cstddef>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "model/Text.h"
#include "util/serializing/BinObjectEncoding.h"
#include "util/serializing/ObjectInputStream.h"
#include "util/serializing/ObjectOutputStream.h"

namespace {

auto serializeText(const Text& text) -> std::string {
    ObjectOutputStream output(new BinObjectEncoding);
    text.serialize(output);

    GString* data = output.stealData();
    std::string result(data->str, data->len);
    g_string_free(data, true);
    return result;
}

auto deserializeText(const std::string& data) -> std::unique_ptr<Text> {
    ObjectInputStream input;
    EXPECT_TRUE(input.read(data.c_str(), data.size() + 1));

    auto text = std::make_unique<Text>();
    text->readSerialized(input);
    return text;
}

void expectSameStyles(const Text& lhs, const Text& rhs) {
    ASSERT_EQ(lhs.getStyleRuns().size(), rhs.getStyleRuns().size());
    for (size_t i = 0; i < lhs.getStyleRuns().size(); i++) {
        const auto& left = lhs.getStyleRuns()[i];
        const auto& right = rhs.getStyleRuns()[i];
        EXPECT_EQ(left.start, right.start);
        EXPECT_EQ(left.end, right.end);
        EXPECT_EQ(left.font.getName(), right.font.getName());
        EXPECT_DOUBLE_EQ(left.font.getSize(), right.font.getSize());
    }
}

}  // namespace

TEST(Text, inlineStyleRangesUseUtf8ByteBoundaries) {
    Text text;
    text.setText("ab 世界cd");

    // The first CJK character starts at byte 3; each CJK character uses three bytes.
    text.setBold(3, 9, true);
    text.setItalic(6, text.getText().size(), true);
    text.setFontSize(6, 9, 20);

    const auto& runs = text.getStyleRuns();
    ASSERT_EQ(runs.size(), 3);

    EXPECT_EQ(runs[0].start, 3U);
    EXPECT_EQ(runs[0].end, 6U);
    EXPECT_TRUE(Text::isBold(runs[0].font));
    EXPECT_FALSE(Text::isItalic(runs[0].font));
    EXPECT_DOUBLE_EQ(runs[0].font.getSize(), 12);

    EXPECT_EQ(runs[1].start, 6U);
    EXPECT_EQ(runs[1].end, 9U);
    EXPECT_TRUE(Text::isBold(runs[1].font));
    EXPECT_TRUE(Text::isItalic(runs[1].font));
    EXPECT_DOUBLE_EQ(runs[1].font.getSize(), 20);

    EXPECT_EQ(runs[2].start, 9U);
    EXPECT_EQ(runs[2].end, text.getText().size());
    EXPECT_FALSE(Text::isBold(runs[2].font));
    EXPECT_TRUE(Text::isItalic(runs[2].font));

    EXPECT_TRUE(Text::isBold(text.getFontAtByteOffset(3)));
    EXPECT_TRUE(Text::isItalic(text.getFontAtByteOffset(6)));
    EXPECT_FALSE(Text::isBold(text.getFontAtByteOffset(9)));

    text.setFontRange(4, 8, text.getFont());
    EXPECT_EQ(text.getStyleRuns().size(), 3U);

    text.setFontRange(3, 6, text.getFont());
    EXPECT_EQ(text.getStyleRuns().size(), 2U);
    EXPECT_EQ(text.getStyleRuns()[0].start, 6U);
    EXPECT_EQ(text.getStyleRuns()[0].end, 9U);
    EXPECT_EQ(text.getStyleRuns()[1].start, 9U);
    EXPECT_EQ(text.getStyleRuns()[1].end, text.getText().size());
}

TEST(Text, inlineStylesRoundTripThroughTextSerialization) {
    Text original;
    original.setText("styled text");
    original.setBold(0, 6, true);
    original.setItalic(3, 11, true);
    original.setFontSize(3, 6, 18.5);

    const auto serializedStyles = original.serializeStyleRuns();
    ASSERT_FALSE(serializedStyles.empty());

    Text xmlRoundTrip;
    xmlRoundTrip.setText(original.getText());
    ASSERT_TRUE(xmlRoundTrip.deserializeStyleRuns(serializedStyles));
    expectSameStyles(original, xmlRoundTrip);

    auto binaryRoundTrip = deserializeText(serializeText(original));
    ASSERT_EQ(binaryRoundTrip->getText(), original.getText());
    expectSameStyles(original, *binaryRoundTrip);
}

TEST(Text, textWithoutStylesKeepsLegacyBinaryLayoutReadable) {
    Text original;
    original.setText("plain text");

    auto roundTrip = deserializeText(serializeText(original));
    ASSERT_EQ(roundTrip->getText(), original.getText());
    EXPECT_TRUE(roundTrip->getStyleRuns().empty());
}

TEST(Text, scalingScalesInlineFontSizes) {
    Text text;
    text.setText("scaled");
    text.setBold(0, 3, true);
    text.setFontSize(3, 6, 20);

    text.scale(0, 0, 2, 2, 0, false);

    ASSERT_EQ(text.getStyleRuns().size(), 2U);
    EXPECT_DOUBLE_EQ(text.getStyleRuns()[0].font.getSize(), 24);
    EXPECT_DOUBLE_EQ(text.getStyleRuns()[1].font.getSize(), 40);
}

TEST(Text, finalCharacterCanBeResized) {
    Text text;
    text.setText("hello cat");

    text.setFontSize(text.getText().size() - 1, text.getText().size(), 20);
    ASSERT_EQ(text.getStyleRuns().size(), 1U);
    EXPECT_EQ(text.getStyleRuns()[0].start, text.getText().size() - 1);
    EXPECT_EQ(text.getStyleRuns()[0].end, text.getText().size());
    EXPECT_DOUBLE_EQ(text.getStyleRuns()[0].font.getSize(), 20);

    text.adjustFontSize(text.getText().size() - 1, text.getText().size(), -1);
    EXPECT_DOUBLE_EQ(text.getStyleRuns()[0].font.getSize(), 19);
}

TEST(Text, finalCharacterCanBeResizedAfterAnEarlierPartialRange) {
    Text text;
    text.setText("hello cat");

    // Reproduce a word whose preceding characters already have an inline
    // style, then format the complete word through the buffer end.
    text.setFontSize(6, 8, 20);
    text.setFontSize(6, text.getText().size(), 21);

    ASSERT_EQ(text.getStyleRuns().size(), 1U);
    EXPECT_EQ(text.getStyleRuns()[0].start, 6U);
    EXPECT_EQ(text.getStyleRuns()[0].end, text.getText().size());
    EXPECT_DOUBLE_EQ(text.getStyleRuns()[0].font.getSize(), 21);
}

TEST(Text, invalidInlineStylesAreIgnored) {
    Text text;
    text.setText("abc");

    EXPECT_TRUE(text.deserializeStyleRuns(""));
    EXPECT_TRUE(text.getStyleRuns().empty());

    // The first record has an empty font name; the valid record should survive.
    EXPECT_FALSE(text.deserializeStyleRuns("1-2::12;0-1:U2Fucw==:13"));
    ASSERT_EQ(text.getStyleRuns().size(), 1U);
    EXPECT_EQ(text.getStyleRuns()[0].start, 0U);
    EXPECT_EQ(text.getStyleRuns()[0].end, 1U);
}
