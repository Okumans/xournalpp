#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <glib.h>

namespace prototype::search {

struct TextRange {
    std::size_t start = 0;
    std::size_t end = 0;

    bool operator==(const TextRange&) const = default;
};

// Return byte ranges in the original UTF-8 text. Building a byte map while
// folding each source code point keeps Pango's byte offsets valid even when
// Unicode case folding expands a character such as sharp s to "ss".
inline std::vector<TextRange> caseInsensitiveMatches(std::string_view text, std::string_view query) {
    if (text.empty() || query.empty() || text.size() > G_MAXSSIZE || query.size() > G_MAXSSIZE ||
        !g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr) ||
        !g_utf8_validate(query.data(), static_cast<gssize>(query.size()), nullptr)) {
        return {};
    }

    gchar* foldedQuery = g_utf8_casefold(query.data(), static_cast<gssize>(query.size()));
    if (!foldedQuery || foldedQuery[0] == '\0') {
        g_free(foldedQuery);
        return {};
    }

    std::string foldedText;
    std::vector<std::size_t> sourceStarts;
    std::vector<std::size_t> sourceEnds;
    foldedText.reserve(text.size());
    sourceStarts.reserve(text.size());
    sourceEnds.reserve(text.size());

    const char* cursor = text.data();
    const char* const textEnd = cursor + text.size();
    while (cursor < textEnd) {
        const char* next = g_utf8_next_char(cursor);
        const auto sourceStart = static_cast<std::size_t>(cursor - text.data());
        const auto sourceEnd = static_cast<std::size_t>(next - text.data());
        gchar* foldedCharacter = g_utf8_casefold(cursor, static_cast<gssize>(next - cursor));
        if (!foldedCharacter) {
            g_free(foldedQuery);
            return {};
        }

        const auto foldedLength = std::char_traits<char>::length(foldedCharacter);
        foldedText.append(foldedCharacter, foldedLength);
        sourceStarts.insert(sourceStarts.end(), foldedLength, sourceStart);
        sourceEnds.insert(sourceEnds.end(), foldedLength, sourceEnd);
        g_free(foldedCharacter);
        cursor = next;
    }

    std::vector<TextRange> matches;
    const std::string_view foldedNeedle(foldedQuery);
    for (std::size_t position = foldedText.find(foldedNeedle); position != std::string::npos;
         position = foldedText.find(foldedNeedle, position + 1)) {
        const auto lastByte = position + foldedNeedle.size() - 1;
        if (lastByte < sourceEnds.size()) {
            matches.push_back({sourceStarts[position], sourceEnds[lastByte]});
        }
    }
    g_free(foldedQuery);
    return matches;
}

}  // namespace prototype::search
