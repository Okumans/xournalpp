#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>

#include "CanvasText.h"

namespace prototype::bridge {

inline constexpr size_t maxEditableTexts = 100000;
inline constexpr size_t maxEditableTextBytes = 4 * 1024 * 1024;
inline constexpr size_t maxEditableFontBytes = 4096;
inline constexpr size_t maxEditableTextStyleRuns = 100000;
inline constexpr uint32_t editableTextAttributes = 0x0fU;
inline constexpr size_t maxTextTransferFileBytes = 32 * 1024 * 1024;

namespace text_transfer_detail {

inline bool isUtf8Boundary(const std::string& text, size_t offset) {
    return offset == 0 || offset == text.size() ||
           (static_cast<unsigned char>(text[offset]) & 0xc0U) != 0x80U;
}

inline bool validUtf8(const std::string& value) {
    return value.find('\0') == std::string::npos &&
           g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr);
}

inline void validateTextItem(const TextItem& item) {
    if (item.page < 0 || item.layerId == 0 || item.alignment < 0 || item.alignment > 2 ||
        !std::isfinite(item.fontSize) || item.fontSize <= 0 || !std::isfinite(item.wrapWidth) ||
        !std::isfinite(item.xx) || !std::isfinite(item.yx) || !std::isfinite(item.xy) ||
        !std::isfinite(item.yy) || !std::isfinite(item.x) || !std::isfinite(item.y) ||
        !std::isfinite(item.layoutWidth) || item.layoutWidth < 0 ||
        !std::isfinite(item.layoutHeight) || item.layoutHeight < 0) {
        throw std::runtime_error("Invalid transferred text metadata");
    }
    if (item.text.size() > maxEditableTextBytes || item.fontName.empty() ||
        item.fontName.size() > maxEditableFontBytes || !validUtf8(item.text) || !validUtf8(item.fontName)) {
        throw std::runtime_error("Invalid transferred text content or font");
    }
    if (item.linkUrl.size() > maxEditableTextBytes || !validUtf8(item.linkUrl) ||
        (!item.isLink && !item.linkUrl.empty())) {
        throw std::runtime_error("Invalid transferred link URL");
    }
    if (item.styleRuns.size() > maxEditableTextStyleRuns) {
        throw std::runtime_error("Transferred text has too many style runs");
    }

    size_t previousEnd = 0;
    for (const auto& run: item.styleRuns) {
        if (run.start >= run.end || run.end > item.text.size() || run.start < previousEnd ||
            !isUtf8Boundary(item.text, run.start) || !isUtf8Boundary(item.text, run.end) ||
            run.fontName.empty() || run.fontName.size() > maxEditableFontBytes || !validUtf8(run.fontName) ||
            !std::isfinite(run.fontSize) || run.fontSize <= 0 ||
            (run.attributes & ~editableTextAttributes) != 0 ||
            (run.attributes & 0x0cU) == 0x0cU) {
            throw std::runtime_error("Invalid transferred text style range");
        }
        previousEnd = run.end;
    }
}

inline std::string readLine(std::istream& stream) {
    std::string line;
    char c = 0;
    while (stream.get(c)) {
        if (c == '\n') {
            return line;
        }
        if (line.size() >= 1024) {
            throw std::runtime_error("Transferred text metadata line is too long");
        }
        line.push_back(c);
    }
    throw std::runtime_error("Incomplete transferred text metadata");
}

inline std::string readBytes(std::istream& stream, size_t length) {
    std::string value(length, '\0');
    if (length && !stream.read(value.data(), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Incomplete transferred text bytes");
    }
    if (stream.get() != '\n') {
        throw std::runtime_error("Invalid transferred text delimiter");
    }
    return value;
}

template <typename... Values>
inline void parseLine(const std::string& line, Values&... values) {
    std::istringstream record(line);
    record.imbue(std::locale::classic());
    if (!(record >> ... >> values)) {
        throw std::runtime_error("Invalid transferred text metadata");
    }
    record >> std::ws;
    if (!record.eof()) {
        throw std::runtime_error("Unexpected transferred text metadata");
    }
}

}  // namespace text_transfer_detail

inline std::vector<TextItem> readTextTransfer(const std::filesystem::path& path) {
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError || fileSize > maxTextTransferFileBytes) {
        throw std::runtime_error("Text transfer file exceeds its size limit");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open text transfer file");
    }
    stream.imbue(std::locale::classic());

    std::string magic;
    size_t count = 0;
    text_transfer_detail::parseLine(text_transfer_detail::readLine(stream), magic, count);
    if ((magic != "XOURNALPP_GTK4_TEXT_V1" && magic != "XOURNALPP_GTK4_TEXT_V2" &&
         magic != "XOURNALPP_GTK4_TEXT_V3" && magic != "XOURNALPP_GTK4_TEXT_V4") ||
        count > maxEditableTexts) {
        throw std::runtime_error("Invalid text-transfer header");
    }
    const bool hasLayerIdentity = magic != "XOURNALPP_GTK4_TEXT_V1";
    const bool hasLayoutGeometry = magic == "XOURNALPP_GTK4_TEXT_V3" || magic == "XOURNALPP_GTK4_TEXT_V4";
    const bool hasLinks = magic == "XOURNALPP_GTK4_TEXT_V4";

    std::vector<TextItem> items;
    items.reserve(count);
    size_t totalStyleRuns = 0;
    for (size_t i = 0; i < count; ++i) {
        TextItem item;
        int justify = 0;
        size_t textBytes = 0;
        size_t fontBytes = 0;
        size_t urlBytes = 0;
        size_t runCount = 0;
        int isLink = 0;
        if (hasLinks) {
            text_transfer_detail::parseLine(text_transfer_detail::readLine(stream), item.page, item.layerId,
                                            item.originLayer, item.layerOrder, item.xx, item.yx, item.xy, item.yy,
                                            item.x, item.y, item.fontSize, item.color, item.wrapWidth,
                                            item.layoutWidth, item.layoutHeight, item.alignment, justify, isLink,
                                            textBytes, fontBytes, urlBytes, runCount);
        } else if (hasLayoutGeometry) {
            text_transfer_detail::parseLine(text_transfer_detail::readLine(stream), item.page, item.layerId,
                                            item.originLayer, item.layerOrder, item.xx, item.yx, item.xy, item.yy,
                                            item.x, item.y, item.fontSize, item.color, item.wrapWidth,
                                            item.layoutWidth, item.layoutHeight, item.alignment, justify, textBytes,
                                            fontBytes, runCount);
        } else if (hasLayerIdentity) {
            text_transfer_detail::parseLine(text_transfer_detail::readLine(stream), item.page, item.layerId,
                                            item.originLayer, item.layerOrder, item.xx, item.yx, item.xy, item.yy,
                                            item.x, item.y, item.fontSize, item.color, item.wrapWidth, item.alignment,
                                            justify, textBytes, fontBytes, runCount);
        } else {
            text_transfer_detail::parseLine(text_transfer_detail::readLine(stream), item.page, item.layerOrder,
                                            item.xx, item.yx, item.xy, item.yy, item.x, item.y, item.fontSize,
                                            item.color, item.wrapWidth, item.alignment, justify, textBytes, fontBytes,
                                            runCount);
            item.layerId = legacyTopLayer;
            item.originLayer = newLayerOrigin;
        }
        if ((justify != 0 && justify != 1) || (isLink != 0 && isLink != 1) ||
            textBytes > maxEditableTextBytes || fontBytes == 0 ||
            fontBytes > maxEditableFontBytes || runCount > maxEditableTextStyleRuns ||
            runCount > maxEditableTextStyleRuns - totalStyleRuns || urlBytes > maxEditableTextBytes) {
            throw std::runtime_error("Transferred text exceeds its field limits");
        }
        item.justify = justify != 0;
        item.isLink = isLink != 0;
        item.fontName = text_transfer_detail::readBytes(stream, fontBytes);
        item.text = text_transfer_detail::readBytes(stream, textBytes);
        if (hasLinks) {
            item.linkUrl = text_transfer_detail::readBytes(stream, urlBytes);
        }
        item.styleRuns.reserve(runCount);

        for (size_t n = 0; n < runCount; ++n) {
            TextStyleRun run;
            int hasColor = 0;
            uint32_t color = 0;
            uint32_t attributes = 0;
            size_t runFontBytes = 0;
            text_transfer_detail::parseLine(text_transfer_detail::readLine(stream), run.start, run.end, run.fontSize,
                                            hasColor, color, attributes, runFontBytes);
            if ((hasColor != 0 && hasColor != 1) || runFontBytes == 0 ||
                runFontBytes > maxEditableFontBytes) {
                throw std::runtime_error("Invalid transferred text style metadata");
            }
            run.color = hasColor ? std::optional<uint32_t>{color} : std::nullopt;
            run.attributes = attributes;
            run.fontName = text_transfer_detail::readBytes(stream, runFontBytes);
            item.styleRuns.push_back(std::move(run));
        }
        totalStyleRuns += runCount;
        text_transfer_detail::validateTextItem(item);
        items.push_back(std::move(item));
    }

    if (stream.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Unexpected trailing text-transfer data");
    }
    return items;
}

inline void writeTextTransfer(const std::filesystem::path& path, const std::vector<TextItem>& items) {
    if (items.size() > maxEditableTexts) {
        throw std::runtime_error("Text transfer contains too many objects");
    }

    size_t estimatedBytes = 64;
    size_t totalStyleRuns = 0;
    for (const auto& item: items) {
        text_transfer_detail::validateTextItem(item);
        if (item.styleRuns.size() > maxEditableTextStyleRuns - totalStyleRuns) {
            throw std::runtime_error("Text transfer contains too many style runs");
        }
        totalStyleRuns += item.styleRuns.size();
        const size_t fixed = 256 + item.fontName.size() + item.text.size() + item.linkUrl.size();
        if (fixed > maxTextTransferFileBytes - estimatedBytes) {
            throw std::runtime_error("Text transfer file exceeds its size limit");
        }
        estimatedBytes += fixed;
        for (const auto& run: item.styleRuns) {
            const size_t runBytes = 128 + run.fontName.size();
            if (runBytes > maxTextTransferFileBytes - estimatedBytes) {
                throw std::runtime_error("Text transfer file exceeds its size limit");
            }
            estimatedBytes += runBytes;
        }
    }

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("Cannot create text transfer file");
    }
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << "XOURNALPP_GTK4_TEXT_V4 " << items.size() << '\n';
    for (const auto& item: items) {
        stream << item.page << ' ' << item.layerId << ' ' << item.originLayer << ' ' << item.layerOrder << ' '
               << item.xx << ' ' << item.yx << ' ' << item.xy << ' ' << item.yy << ' ' << item.x << ' ' << item.y
               << ' ' << item.fontSize << ' ' << item.color << ' ' << item.wrapWidth << ' ' << item.layoutWidth
               << ' ' << item.layoutHeight << ' ' << item.alignment << ' '
               << item.justify << ' ' << item.isLink << ' ' << item.text.size() << ' ' << item.fontName.size() << ' '
               << item.linkUrl.size() << ' '
               << item.styleRuns.size() << '\n';
        stream.write(item.fontName.data(), static_cast<std::streamsize>(item.fontName.size()));
        stream.put('\n');
        stream.write(item.text.data(), static_cast<std::streamsize>(item.text.size()));
        stream.put('\n');
        stream.write(item.linkUrl.data(), static_cast<std::streamsize>(item.linkUrl.size()));
        stream.put('\n');
        for (const auto& run: item.styleRuns) {
            stream << run.start << ' ' << run.end << ' ' << run.fontSize << ' ' << run.color.has_value() << ' '
                   << run.color.value_or(0) << ' ' << run.attributes << ' ' << run.fontName.size() << '\n';
            stream.write(run.fontName.data(), static_cast<std::streamsize>(run.fontName.size()));
            stream.put('\n');
        }
    }
    stream.close();
    if (!stream) {
        throw std::runtime_error("Cannot write text transfer file");
    }
}

}  // namespace prototype::bridge
