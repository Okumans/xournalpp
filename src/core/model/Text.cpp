#include "Text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>  // for move

#include <glib.h>  // for g_warning
#include <pango/pangocairo.h>

#include "model/AudioContent.h"  // for AudioContent
#include "model/Element.h"        // for ELEMENT_TEXT, Eleme...
#include "model/Font.h"           // for XojFont
#include "pdf/base/XojPdfPage.h"  // for XojPdfRectangle
#include "util/Rectangle.h"       // for Rectangle
#include "util/Stacktrace.h"      // for Stacktrace
#include "util/StringUtils.h"
#include "util/raii/GObjectSPtr.h"
#include "util/safe_casts.h"                      // for round_cast
#include "util/serializing/ObjectInputStream.h"   // for ObjectInputStream
#include "util/serializing/ObjectOutputStream.h"  // for ObjectOutputStream

using xoj::util::Rectangle;

namespace {

auto fontsEqual(const XojFont& lhs, const XojFont& rhs) -> bool {
    return lhs.getName() == rhs.getName() && lhs.getSize() == rhs.getSize();
}

auto inlineColorsEqual(const std::optional<Color>& lhs, const std::optional<Color>& rhs) -> bool {
    return lhs.has_value() == rhs.has_value() && (!lhs || *lhs == *rhs);
}

auto isValidFont(const XojFont& font) -> bool {
    return !font.getName().empty() && std::isfinite(font.getSize()) && font.getSize() > 0;
}

auto isUtf8Boundary(std::string_view text, size_t offset) -> bool {
    return offset == 0 || offset == text.size() ||
           (static_cast<unsigned char>(text[offset]) & 0xc0U) != 0x80U;
}

auto parseSizeT(std::string_view text, size_t& value) -> bool {
    if (text.empty()) {
        return false;
    }

    const char* first = text.data();
    const char* last = first + text.size();
    auto [end, error] = std::from_chars(first, last, value);
    return error == std::errc{} && end == last;
}

auto makeStyledFont(const XojFont& font, PangoWeight weight, PangoStyle style) -> XojFont {
    PangoFontDescription* description = pango_font_description_from_string(font.getName().c_str());
    pango_font_description_set_weight(description, weight);
    pango_font_description_set_style(description, style);

    gchar* descriptionString = pango_font_description_to_string(description);
    XojFont result(descriptionString ? descriptionString : "", font.getSize());
    g_free(descriptionString);
    pango_font_description_free(description);
    return result;
}

}  // namespace

Text::Text(): Element(ELEMENT_TEXT) {
    this->font.setName("Sans");
    this->font.setSize(12);
}

Text::~Text() = default;

auto Text::cloneText() const -> std::unique_ptr<Text> {
    auto text = std::make_unique<Text>();
    static_cast<AudioContent&>(*text) = *this;
    text->font = this->font;
    text->text = this->text;
    text->styleRuns = this->styleRuns;
    text->setColor(this->getColor());
    text->boundingBox = this->boundingBox;
    text->snappedBounds = this->snappedBounds;
    text->sizeCalculated = this->sizeCalculated;
    text->inEditing = this->inEditing;
    text->wrapWidth = this->wrapWidth;
    text->align = this->align;
    text->justify = this->justify;

    return text;
}

auto Text::clone() const -> ElementPtr { return cloneText(); }

void Text::setColor(Color color) {
    Element::setColor(color);
    normalizeStyleRuns();
}

auto Text::getFont() -> XojFont& { return font; }
auto Text::getFont() const -> const XojFont& { return font; }

void Text::setFont(const XojFont& font) {
    this->font = font;
    normalizeStyleRuns();
    sizeCalculated = false;
}

auto Text::getFontSize() const -> double { return font.getSize(); }

auto Text::getFontName() const -> std::string { return font.getName(); }

auto Text::getText() const -> const std::string& { return this->text; }

void Text::setText(std::string text) {
    this->text = std::move(text);
    this->styleRuns.clear();
    sizeCalculated = false;
}

auto Text::getStyleRuns() const -> const std::vector<Text::StyleRun>& { return this->styleRuns; }

void Text::setStyleRuns(std::vector<StyleRun> runs) {
    this->styleRuns = std::move(runs);
    normalizeStyleRuns();
    sizeCalculated = false;
}

auto Text::getFontAtByteOffset(size_t offset) const -> XojFont {
    offset = std::min(offset, this->text.size());

    for (const auto& run: this->styleRuns) {
        if (offset >= run.start && (offset < run.end || (offset == this->text.size() && run.end == offset))) {
            return run.font;
        }
    }

    return this->font;
}

auto Text::getInlineColorAtByteOffset(size_t offset) const -> std::optional<Color> {
    offset = std::min(offset, this->text.size());

    for (const auto& run: this->styleRuns) {
        if (offset >= run.start && (offset < run.end || (offset == this->text.size() && run.end == offset))) {
            return run.color;
        }
    }

    return std::nullopt;
}

auto Text::getColorAtByteOffset(size_t offset) const -> Color {
    return this->getInlineColorAtByteOffset(offset).value_or(this->getColor());
}

void Text::setFontRange(size_t start, size_t end, const XojFont& font) {
    transformStyleRange(start, end, [&font](XojFont& segmentFont, std::optional<Color>&) { segmentFont = font; });
}

void Text::setColorRange(size_t start, size_t end, Color color) {
    transformStyleRange(start, end, [color](XojFont&, std::optional<Color>& segmentColor) {
        segmentColor = color;
    });
}

void Text::setBold(size_t start, size_t end, bool bold) {
    transformStyleRange(start, end, [bold](XojFont& font, std::optional<Color>&) {
        font = Text::withBold(font, bold);
    });
}

void Text::setItalic(size_t start, size_t end, bool italic) {
    transformStyleRange(start, end, [italic](XojFont& font, std::optional<Color>&) {
        font = Text::withItalic(font, italic);
    });
}

void Text::setFontSize(size_t start, size_t end, double size) {
    if (!std::isfinite(size) || size <= 0) {
        return;
    }
    transformStyleRange(start, end, [size](XojFont& font, std::optional<Color>&) {
        font = Text::withSize(font, size);
    });
}

void Text::adjustFontSize(size_t start, size_t end, double delta) {
    if (!std::isfinite(delta)) {
        return;
    }
    transformStyleRange(start, end, [delta](XojFont& font, std::optional<Color>&) {
        font = Text::withSize(font, std::max(1.0, font.getSize() + delta));
    });
}

auto Text::isBold(const XojFont& font) -> bool {
    PangoFontDescription* description = pango_font_description_from_string(font.getName().c_str());
    const auto weight = pango_font_description_get_weight(description);
    pango_font_description_free(description);
    return weight >= PANGO_WEIGHT_SEMIBOLD;
}

auto Text::isItalic(const XojFont& font) -> bool {
    PangoFontDescription* description = pango_font_description_from_string(font.getName().c_str());
    const auto style = pango_font_description_get_style(description);
    pango_font_description_free(description);
    return style != PANGO_STYLE_NORMAL;
}

auto Text::withBold(const XojFont& font, bool bold) -> XojFont {
    PangoFontDescription* description = pango_font_description_from_string(font.getName().c_str());
    const auto style = pango_font_description_get_style(description);
    const auto weight = bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL;
    pango_font_description_free(description);
    return makeStyledFont(font, weight, style);
}

auto Text::withItalic(const XojFont& font, bool italic) -> XojFont {
    PangoFontDescription* description = pango_font_description_from_string(font.getName().c_str());
    const auto weight = pango_font_description_get_weight(description);
    pango_font_description_free(description);
    return makeStyledFont(font, weight, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
}

auto Text::withSize(const XojFont& font, double size) -> XojFont { return XojFont(font.getName(), size); }

auto Text::serializeStyleRuns() const -> std::string {
    std::string result;

    for (const auto& run: this->styleRuns) {
        if (run.start >= run.end || run.end > this->text.size() || !isValidFont(run.font)) {
            continue;
        }

        const auto& name = run.font.getName();
        gchar* encodedName = g_base64_encode(reinterpret_cast<const guchar*>(name.data()), name.size());
        if (!encodedName) {
            continue;
        }

        if (!result.empty()) {
            result += ';';
        }
        result += std::to_string(run.start);
        result += '-';
        result += std::to_string(run.end);
        result += ':';
        result += encodedName;
        result += ':';

        char sizeBuffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(sizeBuffer, G_ASCII_DTOSTR_BUF_SIZE, "%.17g", run.font.getSize());
        result += sizeBuffer;

        if (run.color) {
            result += ':';
            result += std::to_string(static_cast<uint32_t>(*run.color));
        }

        g_free(encodedName);
    }

    return result;
}

auto Text::deserializeStyleRuns(std::string_view serialized) -> bool {
    if (serialized.empty()) {
        setStyleRuns({});
        return true;
    }

    std::vector<StyleRun> runs;
    bool valid = true;

    size_t recordStart = 0;
    while (recordStart <= serialized.size()) {
        const size_t recordEnd = serialized.find(';', recordStart);
        const auto record = serialized.substr(recordStart, recordEnd == std::string_view::npos ?
                                                                  std::string_view::npos : recordEnd - recordStart);

        if (record.empty()) {
            valid = false;
        } else {
            const size_t dash = record.find('-');
            const size_t firstColon = record.find(':', dash == std::string_view::npos ? 0 : dash + 1);
            const size_t secondColon = record.find(':', firstColon == std::string_view::npos ? 0 : firstColon + 1);
            const size_t thirdColon = record.find(':', secondColon == std::string_view::npos ? 0 : secondColon + 1);

            size_t start = 0;
            size_t end = 0;
            if (dash == std::string_view::npos || firstColon == std::string_view::npos ||
                secondColon == std::string_view::npos ||
                !parseSizeT(record.substr(0, dash), start) ||
                !parseSizeT(record.substr(dash + 1, firstColon - dash - 1), end)) {
                valid = false;
            } else {
                const auto encodedName = record.substr(firstColon + 1, secondColon - firstColon - 1);
                const auto sizeText = record.substr(secondColon + 1,
                                                    thirdColon == std::string_view::npos ? std::string_view::npos :
                                                                                           thirdColon - secondColon - 1);
                std::string encodedNameString(encodedName);
                gsize decodedLength = 0;
                guchar* decodedName = g_base64_decode(encodedNameString.c_str(), &decodedLength);

                gchar* sizeEnd = nullptr;
                const std::string sizeString(sizeText);
                const double size = g_ascii_strtod(sizeString.c_str(), &sizeEnd);
                const bool goodSize = sizeEnd == sizeString.c_str() + sizeString.size() && std::isfinite(size) &&
                                      size > 0;
                std::optional<Color> color;
                bool goodColor = true;
                if (thirdColon != std::string_view::npos) {
                    uint32_t colorValue = 0;
                    const auto colorText = record.substr(thirdColon + 1);
                    const char* colorFirst = colorText.data();
                    const char* colorLast = colorFirst + colorText.size();
                    auto [colorEnd, colorError] = std::from_chars(colorFirst, colorLast, colorValue);
                    goodColor = !colorText.empty() && colorError == std::errc{} && colorEnd == colorLast;
                    if (goodColor) {
                        color = Color(colorValue);
                    }
                }
                const bool goodRange = start < end && end <= this->text.size() &&
                                       isUtf8Boundary(this->text, start) && isUtf8Boundary(this->text, end);
                const bool goodName = decodedName != nullptr && decodedLength > 0 &&
                                      g_utf8_validate(reinterpret_cast<const char*>(decodedName), decodedLength,
                                                      nullptr);
                if (!goodName || !goodSize || !goodColor || !goodRange) {
                    valid = false;
                } else {
                    std::string name(reinterpret_cast<const char*>(decodedName), decodedLength);
                    runs.push_back({start, end, XojFont(std::move(name), size), color});
                }
                g_free(decodedName);
            }
        }

        if (recordEnd == std::string_view::npos) {
            break;
        }
        recordStart = recordEnd + 1;
    }

    setStyleRuns(std::move(runs));
    return valid;
}

auto Text::createPangoAttrList() const -> xoj::util::PangoAttrListSPtr {
    xoj::util::PangoAttrListSPtr attributes(pango_attr_list_new(), xoj::util::adopt);

    for (const auto& run: this->styleRuns) {
        if (!isValidFont(run.font) || run.start > std::numeric_limits<unsigned int>::max() ||
            run.end > std::numeric_limits<unsigned int>::max()) {
            continue;
        }

        PangoFontDescription* description = pango_font_description_from_string(run.font.getName().c_str());
        pango_font_description_set_absolute_size(description, run.font.getSize() * PANGO_SCALE);

        PangoAttribute* attribute = pango_attr_font_desc_new(description);
        pango_font_description_free(description);
        attribute->start_index = static_cast<unsigned int>(run.start);
        attribute->end_index = static_cast<unsigned int>(run.end);
        pango_attr_list_insert(attributes.get(), attribute);

        if (run.color) {
            const auto color = Util::argb_to_ColorU16(*run.color);
            auto* colorAttribute = pango_attr_foreground_new(color.red, color.green, color.blue);
            colorAttribute->start_index = static_cast<unsigned int>(run.start);
            colorAttribute->end_index = static_cast<unsigned int>(run.end);
            pango_attr_list_insert(attributes.get(), colorAttribute);
        }
    }

    return attributes;
}

void Text::normalizeStyleRuns() {
    std::ranges::sort(this->styleRuns, [](const StyleRun& lhs, const StyleRun& rhs) {
        return lhs.start < rhs.start || (lhs.start == rhs.start && lhs.end < rhs.end);
    });

    std::vector<StyleRun> normalized;
    normalized.reserve(this->styleRuns.size());

    for (auto run: this->styleRuns) {
        if (run.color && *run.color == this->getColor()) {
            run.color.reset();
        }

        if (run.start >= run.end || run.end > this->text.size() || !isUtf8Boundary(this->text, run.start) ||
            !isUtf8Boundary(this->text, run.end) || !isValidFont(run.font) ||
            (fontsEqual(run.font, this->font) && !run.color)) {
            continue;
        }

        if (!normalized.empty() && run.start < normalized.back().end) {
            if (run.end <= normalized.back().end) {
                continue;
            }
            run.start = normalized.back().end;
        }

        if (!normalized.empty() && normalized.back().end == run.start &&
            fontsEqual(normalized.back().font, run.font) &&
            inlineColorsEqual(normalized.back().color, run.color)) {
            normalized.back().end = run.end;
        } else {
            normalized.push_back(std::move(run));
        }
    }

    this->styleRuns = std::move(normalized);
}

void Text::transformStyleRange(size_t start, size_t end,
                               const std::function<void(XojFont&, std::optional<Color>&)>& transform) {
    start = std::min(start, this->text.size());
    end = std::min(end, this->text.size());
    if (start >= end || !isUtf8Boundary(this->text, start) || !isUtf8Boundary(this->text, end)) {
        return;
    }

    std::vector<size_t> boundaries{0, start, end, this->text.size()};
    for (const auto& run: this->styleRuns) {
        boundaries.push_back(run.start);
        boundaries.push_back(run.end);
    }
    std::ranges::sort(boundaries);
    boundaries.erase(std::ranges::unique(boundaries).begin(), boundaries.end());

    std::vector<StyleRun> replacement;
    replacement.reserve(boundaries.size());
    for (size_t i = 0; i + 1 < boundaries.size(); i++) {
        const size_t segmentStart = boundaries[i];
        const size_t segmentEnd = boundaries[i + 1];
        if (segmentStart == segmentEnd) {
            continue;
        }

        XojFont segmentFont = getFontAtByteOffset(segmentStart);
        std::optional<Color> segmentColor = getInlineColorAtByteOffset(segmentStart);
        if (segmentStart >= start && segmentEnd <= end) {
            transform(segmentFont, segmentColor);
        }
        if (!fontsEqual(segmentFont, this->font) || (segmentColor && *segmentColor != this->getColor())) {
            replacement.push_back({segmentStart, segmentEnd, std::move(segmentFont), segmentColor});
        }
    }

    this->styleRuns = std::move(replacement);
    normalizeStyleRuns();
    sizeCalculated = false;
}

void Text::setWrap(double wrap) {
    this->wrapWidth = wrap;
    sizeCalculated = false;
}

void Text::setAlignment(TextAlignment a) {
    this->align = a;
    sizeCalculated = false;
}

Text::Boxes Text::computeBoxesForLayout(PangoLayout* layout, xoj::util::Point<double> origin, double wrapWidth) {
    PangoRectangle box;
    pango_layout_get_extents(layout, nullptr, &box);

    xoj::util::Point<double> offset{static_cast<double>(box.x) / PANGO_SCALE, static_cast<double>(box.y) / PANGO_SCALE};

    Boxes res;

    res.bounds.width = static_cast<double>(box.width) / PANGO_SCALE;
    res.bounds.height = static_cast<double>(box.height) / PANGO_SCALE;
    res.bounds.x = origin.x + offset.x;
    res.bounds.y = origin.y + offset.y;

    res.snap.x = origin.x;
    res.snap.y = origin.y;

    if (wrapWidth != NO_WRAP) {
        res.snap.width = wrapWidth;
    } else {
        res.snap.width = res.bounds.width + offset.x;
    }
    res.snap.height = res.bounds.height + offset.y;

    return res;
}

void Text::setOrigin(double x, double y) {
    this->snappedBounds.x = x;
    this->snappedBounds.y = y;
    this->sizeCalculated = false;  // Recompute Element::x,y
}

auto Text::getOrigin() const -> const xoj::util::Point<double>& { return this->snappedBounds.getOrigin(); }

void Text::calcSize() const {
    auto layout = createPangoLayout();
    pango_layout_set_text(layout.get(), this->text.c_str(), static_cast<int>(this->text.length()));
    auto attributes = createPangoAttrList();
    pango_layout_set_attributes(layout.get(), attributes.get());

    auto boxes = computeBoxesForLayout(layout.get(), this->getOrigin(), this->wrapWidth);

    this->boundingBox = boxes.bounds;
    this->snappedBounds = boxes.snap;
}

void Text::setInEditing(bool inEditing) { this->inEditing = inEditing; }

auto Text::createPangoLayout() const -> xoj::util::GObjectSPtr<PangoLayout> {
    xoj::util::GObjectSPtr<PangoContext> c(pango_font_map_create_context(pango_cairo_font_map_get_default()),
                                           xoj::util::adopt);
    pango_context_set_round_glyph_positions(c.get(), false);  // Avoid weird glyph positioning on small fonts
    xoj::util::GObjectSPtr<PangoLayout> layout(pango_layout_new(c.get()), xoj::util::adopt);

    pango_layout_set_width(layout.get(),
                           this->wrapWidth == NO_WRAP ? -1 : round_cast<int>(this->wrapWidth * PANGO_SCALE));

    pango_layout_set_justify(layout.get(), this->justify);
    pango_layout_set_alignment(layout.get(), this->align.toPango());

#if PANGO_VERSION_CHECK(1, 48, 5)  // see https://gitlab.gnome.org/GNOME/pango/-/issues/499
    pango_layout_set_line_spacing(layout.get(), 1.0);
#endif

    updatePangoFont(layout.get());

    return layout;
}

void Text::updatePangoFont(PangoLayout* layout) const {
    PangoFontDescription* desc = pango_font_description_from_string(this->getFontName().c_str());
    pango_font_description_set_absolute_size(desc, this->getFontSize() * PANGO_SCALE);

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
}

void Text::scale(double x0, double y0, double fx, double fy, double rotation,
                 bool) {  // line width scaling option is not used
    // only proportional scale allowed...
    if (fx != fy) {
        g_warning("rescale font with fx != fy not supported: %lf / %lf", fx, fy);
        Stacktrace::printStacktrace();
    }

    this->boundingBox.x -= x0;
    this->boundingBox.x *= fx;
    this->boundingBox.x += x0;
    this->boundingBox.y -= y0;
    this->boundingBox.y *= fy;
    this->boundingBox.y += y0;

    double size = this->font.getSize() * fx;
    this->font.setSize(size);

    if (this->wrapWidth != NO_WRAP) {
        this->wrapWidth *= fx;
    }

    for (auto& run: this->styleRuns) {
        run.font.setSize(run.font.getSize() * fx);
    }

    sizeCalculated = false;
}

void Text::rotate(double x0, double y0, double th) {}

auto Text::isInEditing() const -> bool { return this->inEditing; }

auto Text::rescaleOnlyAspectRatio() const -> bool { return true; }

void Text::serialize(ObjectOutputStream& out) const {
    out.writeObject("Text");

    this->Element::serialize(out);
    this->AudioContent::serialize(out);

    out.writeString(this->text);

    font.serialize(out);

    out.writeDouble(this->wrapWidth);
    out.writeInt(static_cast<int>(this->align));
    out.writeInt(this->justify);

    if (const auto styles = this->serializeStyleRuns(); !styles.empty()) {
        out.writeObject("TextStyles");
        out.writeString(styles);
        out.endObject();
    }

    out.endObject();
}

void Text::readSerialized(ObjectInputStream& in) {
    in.readObject("Text");

    this->Element::readSerialized(in);
    this->AudioContent::readSerialized(in);

    this->text = in.readString();

    font.readSerialized(in);

    this->wrapWidth = in.readDouble();
    this->align = static_cast<TextAlignment::Value>(in.readInt());
    this->align.validate();
    this->justify = in.readInt() != 0;

    this->styleRuns.clear();
    if (in.nextObjectIs("TextStyles")) {
        in.readObject("TextStyles");
        const auto styles = in.readString();
        if (!this->deserializeStyleRuns(styles)) {
            g_warning("Text: ignoring one or more invalid inline text styles in serialized data");
        }
        in.endObject();
    }

    in.endObject();
}

auto Text::findText(const std::string& search) const -> std::vector<XojPdfRectangle> {
    size_t patternLength = search.length();
    if (patternLength == 0) {
        return {};
    }

    auto layout = this->createPangoLayout();
    pango_layout_set_text(layout.get(), this->text.c_str(), static_cast<int>(this->text.length()));
    auto attributes = createPangoAttrList();
    pango_layout_set_attributes(layout.get(), attributes.get());


    std::string text = StringUtils::toLowerCase(this->text);
    std::string pattern = StringUtils::toLowerCase(search);

    const auto& origin = this->getOrigin();

    std::vector<XojPdfRectangle> list;

    for (size_t pos = text.find(pattern); pos != std::string::npos; pos = text.find(pattern, pos + 1)) {
        XojPdfRectangle mark;
        PangoRectangle rect = {0};
        pango_layout_index_to_pos(layout.get(), static_cast<int>(pos), &rect);
        mark.x1 = (static_cast<double>(rect.x)) / PANGO_SCALE + origin.x;
        mark.y1 = (static_cast<double>(rect.y)) / PANGO_SCALE + origin.y;

        pango_layout_index_to_pos(layout.get(), static_cast<int>(pos + patternLength - 1), &rect);
        mark.x2 = (static_cast<double>(rect.x) + rect.width) / PANGO_SCALE + origin.x;
        mark.y2 = (static_cast<double>(rect.y) + rect.height) / PANGO_SCALE + origin.y;

        list.push_back(mark);
    }

    return list;
}
