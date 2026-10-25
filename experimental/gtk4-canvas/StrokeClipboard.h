#pragma once

#include <cstring>
#include <limits>
#include <span>
#include <string_view>

#include "CanvasImage.h"
#include "DocumentInk.h"

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

    std::string_view string() {
        tag("_s");
        const auto length = raw<size_t>();
        if (length > bytes.size() - position || length > 4096)
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
        if (string() != name)
            fail();
    }
    void end() { tag("_}"); }

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
        in.object("Image");
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
        const auto imageBytes = in.image();
        if (imageBytes.size() > maxBytes - totalImageBytes) {
            Reader::fail();
        }
        totalImageBytes += imageBytes.size();
        in.end();
        if (naturalWidth <= 0 || naturalHeight <= 0 || naturalWidth > maxCanvasImagePixels ||
            naturalHeight > maxCanvasImagePixels || std::floor(naturalWidth) != naturalWidth ||
            std::floor(naturalHeight) != naturalHeight) {
            Reader::fail();
        }
        image.pixelWidth = static_cast<int>(naturalWidth);
        image.pixelHeight = static_cast<int>(naturalHeight);
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
    std::vector<Ink> strokes;
    std::vector<CanvasImage> images;
};

inline DecodedPayload decodePayload(std::string_view bytes) {
    try {
        return {{}, decodeImages(bytes)};
    } catch (const std::exception&) {
        return {decode(bytes), {}};
    }
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
        out.object("Image");
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
        out.number(image.pixelWidth);
        out.number(image.pixelHeight);
        out.end();
        out.image(image.encodedData);
        out.end();
    }
    if (out.bytes.size() > maxBytes)
        Reader::fail();
    return std::move(out.bytes);
}

inline std::string encodeImage(const CanvasImage& image) {
    return encodeImages({image});
}

}  // namespace prototype::clipboard
