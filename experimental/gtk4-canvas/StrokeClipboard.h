#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include "CanvasImage.h"
#include "DocumentInk.h"
#include "DocumentTextTransfer.h"

namespace prototype::clipboard {

inline constexpr const char* mime = "application/xournal";
inline constexpr size_t maxBytes = 64 * 1024 * 1024;
inline constexpr size_t maxClipboardImages = maxCanvasImageObjects;

// XojStrm1 is the native, host-ABI clipboard protocol. Keep it separate from
// files: all lengths are checked against remaining bytes before allocation.
class Reader {
public:
    explicit Reader(std::string_view bytes): bytes(bytes) {
        if (bytes.size() > maxBytes)
            fail();
    }

    [[noreturn]] static void fail() { throw std::runtime_error("Unsupported or malformed Xournal clipboard"); }

    template <typename T>
    T raw() {
        if (sizeof(T) > bytes.size() - position)
            fail();
        T result;
        std::memcpy(&result, bytes.data() + position, sizeof(T));
        position += sizeof(T);
        return result;
    }

    void tag(std::string_view expected) {
        if (bytes.substr(position, expected.size()) != expected)
            fail();
        position += expected.size();
    }

    template <typename T>
    T value(std::string_view type) {
        tag(type);
        return raw<T>();
    }

    double number() {
        auto result = value<double>("_d");
        if (!std::isfinite(result))
            fail();
        return result;
    }

    std::string_view string(size_t maxLength = maxBytes) {
        tag("_s");
        const auto length = raw<size_t>();
        if (length > bytes.size() - position || length > maxLength)
            fail();
        auto result = bytes.substr(position, length);
        position += length;
        return result;
    }

    std::string_view image() {
        tag("_m");
        const auto length = raw<size_t>();
        if (length == 0 || length > maxCanvasImageBytes || length > bytes.size() - position)
            fail();
        const auto result = bytes.substr(position, length);
        position += length;
        return result;
    }

    void object(std::string_view name) {
        tag("_{");
        if (string(4096) != name)
            fail();
    }
    bool nextObjectIs(std::string_view name) const {
        constexpr size_t tagSize = 2;
        constexpr size_t lengthSize = sizeof(size_t);
        if (bytes.substr(position, tagSize) != "_{" || bytes.substr(position + tagSize, tagSize) != "_s" ||
            position + tagSize + tagSize + lengthSize > bytes.size()) {
            return false;
        }
        size_t length = 0;
        std::memcpy(&length, bytes.data() + position + tagSize + tagSize, lengthSize);
        const size_t start = position + tagSize + tagSize + lengthSize;
        return length == name.size() && length <= bytes.size() - start && bytes.substr(start, length) == name;
    }
    void end() { tag("_}"); }

    size_t offset() const { return position; }

    void skipValue(size_t depth = 0) {
        const auto start = position;
        const auto type = bytes.substr(position, 2);
        try {
            if (depth > 32) {
                fail();
            }
            if (type == "_{") {
                tag("_{");
                string(4096);
                while (bytes.substr(position, 2) != "_}") {
                    if (position >= bytes.size()) {
                        fail();
                    }
                    skipValue(depth + 1);
                }
                end();
            } else if (type == "_s") {
                string();
            } else if (type == "_d") {
                tag("_d");
                raw<double>();
            } else if (type == "_i") {
                tag("_i");
                raw<int>();
            } else if (type == "_u") {
                tag("_u");
                raw<uint32_t>();
            } else if (type == "_l") {
                tag("_l");
                raw<size_t>();
            } else if (type == "_m") {
                tag("_m");
                const auto length = raw<size_t>();
                if (length > bytes.size() - position) {
                    fail();
                }
                position += length;
            } else if (type == "_b") {
                tag("_b");
                const auto count = raw<size_t>();
                const auto width = raw<size_t>();
                if (width == 0 ? count != 0 : count > (bytes.size() - position) / width) {
                    fail();
                }
                position += count * width;
            } else {
                throw std::runtime_error("Unsupported native value tag");
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Invalid native value at byte ") + std::to_string(start) +
                                     " (" + std::string(type) + "): " + error.what());
        }
    }

    size_t data(size_t width, size_t limit) {
        tag("_b");
        const auto count = raw<size_t>();
        if (raw<size_t>() != width || count > limit || count > (bytes.size() - position) / width)
            fail();
        return count;
    }

    bool done() const { return position == bytes.size(); }

private:
    std::string_view bytes;
    size_t position = 0;
};

class Writer {
public:
    std::string bytes;
    template <typename T>
    void raw(T value) {
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    template <typename T>
    void value(std::string_view type, T value) {
        bytes += type;
        raw(value);
    }
    void number(double value) { this->value("_d", value); }
    void string(std::string_view text) {
        bytes += "_s";
        raw(text.size());
        bytes += text;
    }
    void object(std::string_view name) {
        bytes += "_{";
        string(name);
    }
    void end() { bytes += "_}"; }
    void image(std::string_view bytes) {
        this->bytes += "_m";
        raw(bytes.size());
        this->bytes.append(bytes);
    }
    void data(size_t count, size_t width) {
        bytes += "_b";
        raw(count);
        raw(width);
    }
};

inline size_t readSelectionHeader(Reader& in) {
    if (in.string(4096) != "XojStrm1:") {
        Reader::fail();
    }
    in.string(4096);
    in.object("EditSelection");
    for (int n = 0; n < 9; ++n) in.number();
    in.object("EditSelectionContents");
    for (int n = 0; n < 11; ++n) in.number();
    in.end();
    in.end();
    const auto count = in.value<int>("_i");
    if (count <= 0 || static_cast<size_t>(count) > 100000) {
        Reader::fail();
    }
    return static_cast<size_t>(count);
}

inline std::string wrapSelectionObject(std::string_view object) {
    Writer out;
    out.string("XojStrm1:");
    out.string("Xournal++ GTK 4 migration");
    out.object("EditSelection");
    for (int n = 0; n < 9; ++n) out.number(0);
    out.object("EditSelectionContents");
    for (int n = 0; n < 11; ++n) out.number(0);
    out.end();
    out.end();
    out.value<int>("_i", 1);
    out.bytes.append(object);
    return std::move(out.bytes);
}

inline std::string_view singleSelectionObject(std::string_view bytes) {
    Reader in(bytes);
    if (readSelectionHeader(in) != 1) {
        Reader::fail();
    }
    const auto start = in.offset();
    try {
        in.skipValue();
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("Cannot frame encoded native clipboard object: ") + error.what());
    }
    const auto end = in.offset();
    if (!in.done()) {
        Reader::fail();
    }
    return bytes.substr(start, end - start);
}

inline std::vector<Ink> decode(std::string_view bytes) {
    Reader in(bytes);
    if (in.string() != "XojStrm1:")
        Reader::fail();
    in.string();  // The producer's project/version string is informational.
    in.object("EditSelection");
    for (int n = 0; n < 9; ++n) in.number();
    in.object("EditSelectionContents");
    for (int n = 0; n < 11; ++n) in.number();
    in.end();
    in.end();
    const auto count = in.value<int>("_i");
    if (count <= 0 || static_cast<size_t>(count) > bridge::maxTransferPoints)
        Reader::fail();
    std::vector<Ink> result;
    size_t total = 0;
    for (int n = 0; n < count; ++n) {
        in.object("Stroke");
        in.object("Element");
        const auto argb = in.value<uint32_t>("_u");
        if ((argb >> 24) != 255)
            Reader::fail();
        in.end();
        in.object("AudioContent");
        if (!in.string().empty())
            Reader::fail();
        in.value<size_t>("_l");
        in.end();
        const auto width = in.number();
        const auto tool = in.value<int>("_i");
        const auto fill = in.value<int>("_i");
        const auto capStyle = in.value<int>("_i");
        if (width <= 0 || tool < 0 || tool > 2 || fill < -1 || fill > 255 || capStyle < 0 || capStyle > 2 ||
            (fill >= 0 && tool != 2))
            Reader::fail();
        if (tool == 1 && (argb & 0xffffffU) != 0xffffffU)
            Reader::fail();
        const auto length = in.data(3 * sizeof(double), bridge::maxTransferPoints - total);
        if (length < 2)
            Reader::fail();
        Ink stroke{0,
                   {},
                   {tool == 2 ? Tool::Highlighter :
                    tool == 1 ? Tool::Eraser :
                                Tool::Pen,
                    {double((argb >> 16) & 255) / 255, double((argb >> 8) & 255) / 255, double(argb & 255) / 255},
                    width}};
        stroke.fill = fill;
        stroke.capStyle = capStyle;
        stroke.points.reserve(length);
        double previous = -1;
        for (size_t p = 0; p < length; ++p) {
            auto x = in.raw<double>(), y = in.raw<double>(), z = in.raw<double>();
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                Reader::fail();
            if (!p)
                stroke.pressureSensitive = z > 0;
            if (stroke.pressureSensitive && tool != 0)
                Reader::fail();
            if (stroke.pressureSensitive && p + 1 == length && z == -1)
                z = previous;
            if ((stroke.pressureSensitive && z <= 0) || (!stroke.pressureSensitive && z != -1))
                Reader::fail();
            previous = z;
            const auto pressure = stroke.pressureSensitive ? z / width : 1;
            if (!std::isfinite(pressure) || pressure <= 0)
                Reader::fail();
            stroke.points.push_back({x, y, pressure});
        }
        in.object("LineStyle");
        if (in.data(sizeof(double), 0) != 0)
            Reader::fail();
        in.end();
        in.end();
        total += length;
        result.push_back(std::move(stroke));
    }
    if (!in.done())
        Reader::fail();
    return result;
}

inline std::string encode(const std::vector<Ink>& strokes) {
    if (strokes.empty())
        Reader::fail();
    auto packets = bridge::transferInk(strokes);
    size_t total = 0;
    for (const auto& packet: packets) {
        bridge::validate(packet);
        if (packet.points.size() < 2 || packet.points.size() > bridge::maxTransferPoints - total)
            Reader::fail();
        total += packet.points.size();
    }
    CanvasState selection;
    selection.ink = strokes;
    selection.page = strokes.front().page;
    selection.selectAllOnPage();
    const auto bounds = selection.selectionBox();
    if (!bounds || selection.selected.size() != strokes.size())
        Reader::fail();
    const double rectangle[] = {bounds->x0, bounds->y0, bounds->x1 - bounds->x0, bounds->y1 - bounds->y0};
    // Bound output before reserving or appending any serialized samples.
    constexpr size_t maxStrokeOverhead = 256;
    if (strokes.size() > maxBytes / maxStrokeOverhead ||
        total > (maxBytes - strokes.size() * maxStrokeOverhead) / (3 * sizeof(double)))
        Reader::fail();
    Writer out;
    out.string("XojStrm1:");
    out.string("Xournal++ GTK 4 migration");
    out.object("EditSelection");
    for (int n = 0; n < 2; ++n)
        for (auto value: rectangle) out.number(value);
    out.number(0);
    out.object("EditSelectionContents");
    for (int n = 0; n < 2; ++n)
        for (auto value: rectangle) out.number(value);
    for (int n = 0; n < 3; ++n) out.number(0);
    out.end();
    out.end();
    out.value<int>("_i", static_cast<int>(strokes.size()));
    for (const auto& stroke: strokes) {
        if (isSelectionTool(stroke.brush.tool))
            Reader::fail();
        out.object("Stroke");
        out.object("Element");
        uint32_t argb = 0xff000000U;
        for (int channel = 0; channel < 3; ++channel) {
            argb |= uint32_t(std::lround(stroke.brush.color[channel] * 255)) << (16 - channel * 8);
        }
        out.value("_u", argb);
        out.end();
        out.object("AudioContent");
        out.string("");
        out.value<size_t>("_l", 0);
        out.end();
        out.number(stroke.brush.width);
        out.value<int>("_i", stroke.brush.tool == Tool::Highlighter ? 2 : stroke.brush.tool == Tool::Eraser ? 1 : 0);
        out.value<int>("_i", stroke.fill);
        out.value<int>("_i", stroke.capStyle);
        out.data(stroke.points.size(), 3 * sizeof(double));
        for (const auto& point: stroke.points) {
            out.raw(point.x);
            out.raw(point.y);
            const auto z = stroke.pressureSensitive ? point.pressure * stroke.brush.width : -1;
            if (!std::isfinite(z) || (stroke.pressureSensitive && z <= 0))
                Reader::fail();
            out.raw(z);
        }
        out.object("LineStyle");
        out.data(0, sizeof(double));
        out.end();
        out.end();
    }
    if (out.bytes.size() > maxBytes)
        Reader::fail();
    return std::move(out.bytes);
}

inline std::vector<CanvasImage> decodeImages(std::string_view bytes) {
    Reader in(bytes);
    if (in.string() != "XojStrm1:")
        Reader::fail();
    in.string();
    in.object("EditSelection");
    for (int n = 0; n < 9; ++n) in.number();
    in.object("EditSelectionContents");
    for (int n = 0; n < 11; ++n) in.number();
    in.end();
    in.end();
    const auto count = in.value<int>("_i");
    if (count <= 0 || static_cast<size_t>(count) > maxClipboardImages)
        Reader::fail();

    std::vector<CanvasImage> images;
    images.reserve(static_cast<size_t>(count));
    size_t totalImageBytes = 0;
    for (int n = 0; n < count; ++n) {
        const bool latex = in.nextObjectIs("TexImage");
        if (!latex && !in.nextObjectIs("Image")) {
            Reader::fail();
        }
        in.object(latex ? "TexImage" : "Image");
        in.object("RectangularElement");
        in.object("Element");
        in.value<uint32_t>("_u");
        in.end();
        CanvasImage image;
        image.transform.xx = in.number();
        image.transform.yx = in.number();
        image.transform.xy = in.number();
        image.transform.yy = in.number();
        image.transform.x = in.number();
        image.transform.y = in.number();
        const double naturalWidth = in.number();
        const double naturalHeight = in.number();
        in.end();
        std::string_view imageBytes;
        if (latex) {
            image.kind = CanvasImageKind::Latex;
            image.nativeWidth = naturalWidth;
            image.nativeHeight = naturalHeight;
            image.latexSource.assign(in.string(bridge::maxEditableTextBytes));
            imageBytes = in.string(maxCanvasImageBytes);
        } else {
            imageBytes = in.image();
        }
        if (imageBytes.size() > maxBytes - totalImageBytes) {
            Reader::fail();
        }
        totalImageBytes += imageBytes.size();
        in.end();
        if (latex) {
            if (!std::isfinite(naturalWidth) || !std::isfinite(naturalHeight)) {
                Reader::fail();
            }
        } else {
            if (naturalWidth <= 0 || naturalHeight <= 0 || naturalWidth > maxCanvasImagePixels ||
                naturalHeight > maxCanvasImagePixels || std::floor(naturalWidth) != naturalWidth ||
                std::floor(naturalHeight) != naturalHeight) {
                Reader::fail();
            }
            image.pixelWidth = static_cast<int>(naturalWidth);
            image.pixelHeight = static_cast<int>(naturalHeight);
        }
        image.encodedData.assign(imageBytes);
        if (!validCanvasImage(image)) {
            Reader::fail();
        }
        images.push_back(std::move(image));
    }
    if (!in.done())
        Reader::fail();
    return images;
}

inline CanvasImage decodeImage(std::string_view bytes) {
    auto images = decodeImages(bytes);
    if (images.size() != 1)
        Reader::fail();
    return std::move(images.front());
}

struct DecodedPayload {
    std::vector<EditableObject> objects;
};

namespace text_clipboard_detail {

template <typename T>
inline T integer(std::string_view value) {
    T result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        Reader::fail();
    }
    return result;
}

inline double real(std::string_view value) {
    double result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result,
                                              std::chars_format::general);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result)) {
        Reader::fail();
    }
    return result;
}

inline std::string decodeFont(std::string_view encoded) {
    if (encoded.size() > maxBytes / 2 || encoded.empty() || encoded.size() % 4 != 0 ||
        !std::all_of(encoded.begin(), encoded.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
                   c == '/' || c == '=';
        })) {
        Reader::fail();
    }
    gsize length = 0;
    auto* decoded = g_base64_decode(std::string(encoded).c_str(), &length);
    if (!decoded || length == 0 || length > bridge::maxEditableFontBytes ||
        !g_utf8_validate(reinterpret_cast<const char*>(decoded), static_cast<gssize>(length), nullptr) ||
        std::memchr(decoded, '\0', length)) {
        g_free(decoded);
        Reader::fail();
    }
    std::string result(reinterpret_cast<const char*>(decoded), length);
    g_free(decoded);
    return result;
}

inline std::vector<TextStyleRun> parseStyleRuns(std::string_view encoded) {
    if (encoded.size() > bridge::maxEditableTextBytes) {
        Reader::fail();
    }
    std::vector<TextStyleRun> runs;
    size_t start = 0;
    while (start < encoded.size()) {
        const size_t end = encoded.find(';', start);
        const auto record = encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
        std::array<std::string_view, 5> fields{};
        size_t count = 0;
        size_t fieldStart = 0;
        while (true) {
            if (count == fields.size()) {
                Reader::fail();
            }
            const size_t colon = record.find(':', fieldStart);
            fields[count++] = record.substr(fieldStart, colon == std::string_view::npos ? record.size() - fieldStart :
                                                                                          colon - fieldStart);
            if (colon == std::string_view::npos) {
                break;
            }
            fieldStart = colon + 1;
        }
        if (count < 3 || fields[0].empty()) {
            Reader::fail();
        }
        const size_t dash = fields[0].find('-');
        if (dash == std::string_view::npos) {
            Reader::fail();
        }
        TextStyleRun run;
        run.start = integer<size_t>(fields[0].substr(0, dash));
        run.end = integer<size_t>(fields[0].substr(dash + 1));
        run.fontName = decodeFont(fields[1]);
        run.fontSize = real(fields[2]);
        if (count >= 4 && fields[3] != "-") {
            run.color = integer<uint32_t>(fields[3]);
        }
        if (count == 5) {
            run.attributes = integer<uint32_t>(fields[4]);
        }
        runs.push_back(std::move(run));
        if (runs.size() > bridge::maxEditableTextStyleRuns) {
            Reader::fail();
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return runs;
}

inline std::string serializeStyleRuns(const TextItem& item) {
    std::string result;
    for (const auto& run: item.styleRuns) {
        if (!result.empty()) {
            result.push_back(';');
        }
        gchar* encodedName = g_base64_encode(reinterpret_cast<const guchar*>(run.fontName.data()), run.fontName.size());
        if (!encodedName) {
            Reader::fail();
        }
        result += std::to_string(run.start) + "-" + std::to_string(run.end) + ":" + encodedName + ":";
        g_free(encodedName);
        char sizeBuffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr(sizeBuffer, sizeof(sizeBuffer), run.fontSize);
        result += sizeBuffer;
        if (run.color || run.attributes) {
            result += ":" + (run.color ? std::to_string(*run.color) : "-");
        }
        if (run.attributes) {
            result += ":" + std::to_string(run.attributes);
        }
    }
    return result;
}

inline void decodeBase(Reader& in, TextItem& item) {
    in.object("RectangularElement");
    in.object("Element");
    item.color = in.value<uint32_t>("_u");
    in.end();
    item.xx = in.number();
    item.yx = in.number();
    item.xy = in.number();
    item.yy = in.number();
    item.x = in.number();
    item.y = in.number();
    item.layoutWidth = in.number();
    item.layoutHeight = in.number();
    in.end();
}

inline void decodeFont(Reader& in, TextItem& item) {
    in.object("XojFont");
    const auto fontName = in.string(bridge::maxEditableFontBytes);
    if (!g_utf8_validate(fontName.data(), static_cast<gssize>(fontName.size()), nullptr) ||
        fontName.find('\0') != std::string_view::npos) {
        Reader::fail();
    }
    item.fontName.assign(fontName);
    item.fontSize = in.number();
    in.end();
}

}  // namespace text_clipboard_detail

inline std::vector<TextItem> decodeTexts(std::string_view bytes) {
    Reader in(bytes);
    if (in.string(4096) != "XojStrm1:") {
        Reader::fail();
    }
    in.string(4096);
    in.object("EditSelection");
    for (int n = 0; n < 9; ++n) in.number();
    in.object("EditSelectionContents");
    for (int n = 0; n < 11; ++n) in.number();
    in.end();
    in.end();
    const auto count = in.value<int>("_i");
    if (count <= 0 || static_cast<size_t>(count) > bridge::maxEditableTexts) {
        Reader::fail();
    }

    std::vector<TextItem> texts;
    texts.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        TextItem item;
        if (in.nextObjectIs("Link")) {
            in.object("Link");
            text_clipboard_detail::decodeBase(in, item);
            const auto text = in.string(bridge::maxEditableTextBytes);
            const auto url = in.string(bridge::maxEditableTextBytes);
            item.text.assign(text);
            item.linkUrl.assign(url);
            const auto alignment = in.value<int>("_i");
            if (alignment < 0 || alignment > 2) {
                Reader::fail();
            }
            item.alignment = alignment;
            item.isLink = true;
            text_clipboard_detail::decodeFont(in, item);
            item.wrapWidth = -1;
            in.end();
        } else {
            in.object("Text");
            text_clipboard_detail::decodeBase(in, item);
            in.object("AudioContent");
            if (!in.string(4096).empty() || in.value<size_t>("_l") != 0) {
                Reader::fail();
            }
            in.end();
            const auto text = in.string(bridge::maxEditableTextBytes);
            item.text.assign(text);
            text_clipboard_detail::decodeFont(in, item);
            item.wrapWidth = in.number();
            const auto alignment = in.value<int>("_i");
            const auto justify = in.value<int>("_i");
            if (alignment < 0 || alignment > 2 || (justify != 0 && justify != 1)) {
                Reader::fail();
            }
            item.alignment = alignment;
            item.justify = justify != 0;
            if (in.nextObjectIs("TextStyles")) {
                in.object("TextStyles");
                item.styleRuns = text_clipboard_detail::parseStyleRuns(in.string(bridge::maxEditableTextBytes));
                in.end();
            }
            in.end();
        }
        if (!g_utf8_validate(item.text.data(), static_cast<gssize>(item.text.size()), nullptr) ||
            item.text.find('\0') != std::string::npos) {
            Reader::fail();
        }
        bridge::text_transfer_detail::validateTextItem(item);
        texts.push_back(std::move(item));
    }
    if (!in.done()) {
        Reader::fail();
    }
    return texts;
}

inline std::string encodeTexts(const std::vector<TextItem>& texts, const SelectionBox& bounds) {
    if (texts.empty() || texts.size() > bridge::maxEditableTexts || !std::isfinite(bounds.x0) ||
        !std::isfinite(bounds.y0) || !std::isfinite(bounds.x1) || !std::isfinite(bounds.y1) ||
        bounds.x1 < bounds.x0 || bounds.y1 < bounds.y0) {
        Reader::fail();
    }
    for (const auto& item: texts) {
        bridge::text_transfer_detail::validateTextItem(item);
        if (item.page != texts.front().page) {
            Reader::fail();
        }
    }
    const double rectangle[] = {bounds.x0, bounds.y0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0};
    Writer out;
    out.string("XojStrm1:");
    out.string("Xournal++ GTK 4 migration");
    out.object("EditSelection");
    for (int n = 0; n < 2; ++n) {
        for (const auto value: rectangle) out.number(value);
    }
    out.number(0);
    out.object("EditSelectionContents");
    for (int n = 0; n < 2; ++n) {
        for (const auto value: rectangle) out.number(value);
    }
    for (int n = 0; n < 3; ++n) out.number(0);
    out.end();
    out.end();
    out.value<int>("_i", static_cast<int>(texts.size()));
    for (const auto& item: texts) {
        out.object(item.isLink ? "Link" : "Text");
        out.object("RectangularElement");
        out.object("Element");
        out.value<uint32_t>("_u", item.color);
        out.end();
        out.number(item.xx);
        out.number(item.yx);
        out.number(item.xy);
        out.number(item.yy);
        out.number(item.x);
        out.number(item.y);
        out.number(item.layoutWidth);
        out.number(item.layoutHeight);
        out.end();
        if (item.isLink) {
            out.string(item.text);
            out.string(item.linkUrl);
            out.value<int>("_i", item.alignment);
            out.object("XojFont");
            out.string(item.fontName);
            out.number(item.fontSize);
            out.end();
        } else {
            out.object("AudioContent");
            out.string("");
            out.value<size_t>("_l", 0);
            out.end();
            out.string(item.text);
            out.object("XojFont");
            out.string(item.fontName);
            out.number(item.fontSize);
            out.end();
            out.number(item.wrapWidth);
            out.value<int>("_i", item.alignment);
            out.value<int>("_i", item.justify ? 1 : 0);
            const auto styleRuns = text_clipboard_detail::serializeStyleRuns(item);
            if (!styleRuns.empty()) {
                out.object("TextStyles");
                out.string(styleRuns);
                out.end();
            }
        }
        out.end();
    }
    if (out.bytes.size() > maxBytes) {
        Reader::fail();
    }
    return std::move(out.bytes);
}

inline std::string encodeImages(const std::vector<CanvasImage>& images) {
    if (images.empty() || images.size() > maxClipboardImages)
        Reader::fail();
    auto corners = canvasImageCorners(images.front());
    if (!validCanvasImage(images.front()))
        Reader::fail();
    double x0 = corners.front().x, y0 = corners.front().y;
    double x1 = x0, y1 = y0;
    size_t totalImageBytes = 0;
    for (const auto& image: images) {
        if (!validCanvasImage(image) || image.page != images.front().page ||
            image.encodedData.size() > maxBytes - totalImageBytes) {
            Reader::fail();
        }
        totalImageBytes += image.encodedData.size();
        for (const auto& point: canvasImageCorners(image)) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y))
                Reader::fail();
            x0 = std::min(x0, point.x);
            y0 = std::min(y0, point.y);
            x1 = std::max(x1, point.x);
            y1 = std::max(y1, point.y);
        }
    }
    if (!std::isfinite(x1 - x0) || !std::isfinite(y1 - y0))
        Reader::fail();
    const double rectangle[] = {x0, y0, x1 - x0, y1 - y0};

    Writer out;
    out.string("XojStrm1:");
    out.string("Xournal++ GTK 4 migration");
    out.object("EditSelection");
    for (int n = 0; n < 2; ++n)
        for (auto value: rectangle) out.number(value);
    out.number(0);
    out.object("EditSelectionContents");
    for (int n = 0; n < 2; ++n)
        for (auto value: rectangle) out.number(value);
    for (int n = 0; n < 3; ++n) out.number(0);
    out.end();
    out.end();
    out.value<int>("_i", static_cast<int>(images.size()));
    for (const auto& image: images) {
        const bool latex = image.kind == CanvasImageKind::Latex;
        out.object(latex ? "TexImage" : "Image");
        out.object("RectangularElement");
        out.object("Element");
        out.value<uint32_t>("_u", 0xff000000U);
        out.end();
        out.number(image.transform.xx);
        out.number(image.transform.yx);
        out.number(image.transform.xy);
        out.number(image.transform.yy);
        out.number(image.transform.x);
        out.number(image.transform.y);
        out.number(latex ? image.nativeWidth : image.pixelWidth);
        out.number(latex ? image.nativeHeight : image.pixelHeight);
        out.end();
        if (latex) {
            out.string(image.latexSource);
            out.string(image.encodedData);
        } else {
            out.image(image.encodedData);
        }
        out.end();
    }
    if (out.bytes.size() > maxBytes)
        Reader::fail();
    return std::move(out.bytes);
}

inline std::string encodeImage(const CanvasImage& image) {
    return encodeImages({image});
}

inline std::vector<EditableObject> decodeObjects(std::string_view bytes) {
    Reader in(bytes);
    const auto count = readSelectionHeader(in);
    std::vector<EditableObject> objects;
    objects.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        enum class Kind { Stroke, Image, Text };
        Kind kind;
        if (in.nextObjectIs("Stroke")) {
            kind = Kind::Stroke;
        } else if (in.nextObjectIs("Image") || in.nextObjectIs("TexImage")) {
            kind = Kind::Image;
        } else if (in.nextObjectIs("Text") || in.nextObjectIs("Link")) {
            kind = Kind::Text;
        } else {
            Reader::fail();
        }

        const auto start = in.offset();
        try {
            in.skipValue();
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Cannot frame native clipboard object ") + std::to_string(index) +
                                     ": " + error.what());
        }
        const auto record = bytes.substr(start, in.offset() - start);
        const auto singleObject = wrapSelectionObject(record);
        try {
            switch (kind) {
                case Kind::Stroke: {
                    auto decoded = decode(singleObject);
                    if (decoded.size() != 1) Reader::fail();
                    objects.emplace_back(std::move(decoded.front()));
                    break;
                }
                case Kind::Image: {
                    auto decoded = decodeImages(singleObject);
                    if (decoded.size() != 1) Reader::fail();
                    objects.emplace_back(std::move(decoded.front()));
                    break;
                }
                case Kind::Text: {
                    auto decoded = decodeTexts(singleObject);
                    if (decoded.size() != 1) Reader::fail();
                    objects.emplace_back(std::move(decoded.front()));
                    break;
                }
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Cannot decode native clipboard object ") +
                                     std::to_string(index) + ": " + error.what());
        }
    }
    if (!in.done()) {
        Reader::fail();
    }
    return objects;
}

inline DecodedPayload decodePayload(std::string_view bytes) {
    try {
        return {decodeObjects(bytes)};
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("Cannot decode native clipboard group: ") + error.what());
    }
}

inline std::string encodeObjects(const std::vector<EditableObject>& objects, const SelectionBox& bounds) {
    constexpr size_t maxClipboardObjects = 100000;
    if (objects.empty() || objects.size() > maxClipboardObjects || !std::isfinite(bounds.x0) ||
        !std::isfinite(bounds.y0) || !std::isfinite(bounds.x1) || !std::isfinite(bounds.y1) ||
        bounds.x1 < bounds.x0 || bounds.y1 < bounds.y0) {
        Reader::fail();
    }

    Writer out;
    out.string("XojStrm1:");
    out.string("Xournal++ GTK 4 migration");
    out.object("EditSelection");
    const double rectangle[] = {bounds.x0, bounds.y0, bounds.x1 - bounds.x0, bounds.y1 - bounds.y0};
    for (int n = 0; n < 2; ++n) {
        for (const auto value: rectangle) out.number(value);
    }
    out.number(0);
    out.object("EditSelectionContents");
    for (int n = 0; n < 2; ++n) {
        for (const auto value: rectangle) out.number(value);
    }
    for (int n = 0; n < 3; ++n) out.number(0);
    out.end();
    out.end();
    out.value<int>("_i", static_cast<int>(objects.size()));

    for (const auto& object: objects) {
        if (std::visit([](const auto& item) { return item.page; }, object) != bounds.page) {
            Reader::fail();
        }
        std::string single;
        std::visit(
                [&](const auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, Ink>) {
                        single = encode({item});
                    } else if constexpr (std::is_same_v<T, TextItem>) {
                        single = encodeTexts({item}, bounds);
                    } else {
                        single = encodeImages({item});
                    }
                },
                object);
        std::string_view record;
        try {
            record = singleSelectionObject(single);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("Cannot encode selected native object: ") + error.what());
        }
        if (record.size() > maxBytes - out.bytes.size()) {
            Reader::fail();
        }
        out.bytes.append(record);
    }
    return std::move(out.bytes);
}

}  // namespace prototype::clipboard
