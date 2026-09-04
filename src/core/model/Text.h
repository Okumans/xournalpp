/*
 * Xournal++
 *
 * A text element
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <cstddef>  // for size_t
#include <functional>
#include <optional>
#include <string>  // for string
#include <string_view>
#include <vector>

#include <pango/pango.h>

#include "util/Point.h"
#include "util/Color.h"
#include "util/raii/GObjectSPtr.h"
#include "util/raii/PangoSPtr.h"

#include "AudioContent.h"
#include "Element.h"
#include "Font.h"          // for XojFont
#include "TextAlignment.h"

class ObjectInputStream;
class ObjectOutputStream;
class XojPdfRectangle;

class Text: public Element, public AudioContent {
public:
    /**
     * Inline attributes applied to a half-open UTF-8 byte range of the text.
     *
     * The offsets deliberately use UTF-8 bytes because that is the coordinate
     * system used by Pango and GtkTextIter's line indices.  Text always keeps
     * these offsets on character boundaries.
     */
    struct StyleRun {
        size_t start = 0;
        size_t end = 0;
        XojFont font;
        // An empty value inherits the text element's base color.  Keeping this
        // optional preserves the compact legacy font-only style encoding.
        std::optional<Color> color;
    };

    Text();
    ~Text() override;

    static constexpr double NO_WRAP = -1;

public:
    void setColor(Color color);
    void setFont(const XojFont& font);
    XojFont& getFont();
    const XojFont& getFont() const;
    double getFontSize() const;       // same result as getFont()->getSize(), but const
    std::string getFontName() const;  // same result as getFont()->getName(), but const

    const std::string& getText() const;
    void setText(std::string text);

    const std::vector<StyleRun>& getStyleRuns() const;
    void setStyleRuns(std::vector<StyleRun> runs);

    /** Return the effective font at a UTF-8 byte offset. */
    XojFont getFontAtByteOffset(size_t offset) const;

    /** Return the effective color at a UTF-8 byte offset. */
    Color getColorAtByteOffset(size_t offset) const;

    /** Apply a complete font to a half-open UTF-8 byte range. */
    void setFontRange(size_t start, size_t end, const XojFont& font);
    /** Apply an explicit foreground color to a half-open UTF-8 byte range. */
    void setColorRange(size_t start, size_t end, Color color);
    void setBold(size_t start, size_t end, bool bold);
    void setItalic(size_t start, size_t end, bool italic);
    void setFontSize(size_t start, size_t end, double size);
    void adjustFontSize(size_t start, size_t end, double delta);

    static bool isBold(const XojFont& font);
    static bool isItalic(const XojFont& font);
    static XojFont withBold(const XojFont& font, bool bold);
    static XojFont withItalic(const XojFont& font, bool italic);
    static XojFont withSize(const XojFont& font, double size);

    /**
     * Serialize inline styles for the optional .xopp `styles` attribute.
     * An empty return value means that the text uses only its object font.
     */
    std::string serializeStyleRuns() const;

    /**
     * Decode the optional .xopp `styles` attribute. Invalid runs are ignored
     * and the return value reports whether the complete value was valid.
     */
    bool deserializeStyleRuns(std::string_view serialized);

    /** Build the Pango attributes representing the inline style runs. */
    xoj::util::PangoAttrListSPtr createPangoAttrList() const;

    void setInEditing(bool inEditing);
    bool isInEditing() const;

    xoj::util::GObjectSPtr<PangoLayout> createPangoLayout() const;
    void updatePangoFont(PangoLayout* layout) const;

    void scale(double x0, double y0, double fx, double fy, double rotation, bool restoreLineWidth) override;
    void rotate(double x0, double y0, double th) override;

    void setOrigin(double x, double y) override;
    const xoj::util::Point<double>& getOrigin() const override;

    bool rescaleOnlyAspectRatio() const override;

    /// Set the text's wrapping width. Use Text::NO_WRAP to disable wrapping
    void setWrap(double wrap);

    /// Return Text::NO_WRAP if wrapping is disabled
    inline double getWrap() const { return wrapWidth; }

    void setAlignment(TextAlignment a);
    inline TextAlignment getAlign() const { return align; }

    inline void setJustify(bool j) { this->justify = j; }
    inline bool getJustify() const { return justify; }

    auto cloneText() const -> std::unique_ptr<Text>;
    auto clone() const -> ElementPtr override;

public:
    // Serialize interface
    void serialize(ObjectOutputStream& out) const override;
    void readSerialized(ObjectInputStream& in) override;

    struct Boxes {
        xoj::util::Rectangle<double> snap;
        xoj::util::Rectangle<double> bounds;
    };
    static Boxes computeBoxesForLayout(PangoLayout* layout, xoj::util::Point<double> origin, double wrapWidth);

protected:
    void calcSize() const override;

public:
    std::vector<XojPdfRectangle> findText(const std::string& search) const;

private:
    void normalizeStyleRuns();
    std::optional<Color> getInlineColorAtByteOffset(size_t offset) const;
    void transformStyleRange(size_t start, size_t end,
                             const std::function<void(XojFont&, std::optional<Color>&)>& transform);

    XojFont font;

    std::string text;

    std::vector<StyleRun> styleRuns;

    double wrapWidth = NO_WRAP;  ///< NO_WRAP for no wrap
    TextAlignment align = TextAlignment::LEFT;
    bool justify = false;  ///< Stretch whitespaces to make all complete lines have the same width

    bool inEditing = false;
};
