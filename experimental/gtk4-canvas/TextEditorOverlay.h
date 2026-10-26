#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtk/gtk.h>

#include "CanvasText.h"

namespace prototype {

class TextEditorOverlay {
public:
    enum class Format { Bold, Italic, Underline, Strikethrough, Superscript, Subscript };

    struct Completion {
        std::optional<size_t> existingIndex;
        TextItem item;
        bool hasContent = false;
    };

    explicit TextEditorOverlay(GtkOverlay* overlay) {
        view = gtk_text_view_new();
        gtk_widget_add_css_class(view, "canvas-text-editor");
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 0);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 0);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 0);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 0);
        gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(view), false);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_NONE);
        gtk_widget_set_halign(view, GTK_ALIGN_START);
        gtk_widget_set_valign(view, GTK_ALIGN_START);
        gtk_widget_set_visible(view, false);
        gtk_overlay_add_overlay(overlay, view);
    }

    GtkWidget* widget() const { return view; }
    bool active() const { return draft.has_value(); }
    const TextItem* item() const { return draft ? &*draft : nullptr; }
    std::optional<size_t> existingItemIndex() const { return existingIndex; }

    void selectWordAt(int x, int y) {
        if (!active()) {
            return;
        }

        int bufferX = 0;
        int bufferY = 0;
        gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(view), GTK_TEXT_WINDOW_WIDGET, x, y, &bufferX,
                                              &bufferY);
        GtkTextIter cursor;
        int trailing = 0;
        if (!gtk_text_view_get_iter_at_position(GTK_TEXT_VIEW(view), &cursor, &trailing, bufferX, bufferY)) {
            return;
        }
        if (trailing > 0) {
            gtk_text_iter_forward_chars(&cursor, trailing);
        }

        GtkTextIter start = cursor;
        GtkTextIter end = cursor;
        if (!gtk_text_iter_inside_word(&cursor) && !gtk_text_iter_starts_word(&cursor) &&
            !gtk_text_iter_ends_word(&cursor)) {
            gtk_text_buffer_place_cursor(editorBuffer, &cursor);
            return;
        }
        if (!gtk_text_iter_starts_word(&start)) {
            gtk_text_iter_backward_word_start(&start);
        }
        if (!gtk_text_iter_ends_word(&end)) {
            gtk_text_iter_forward_word_end(&end);
        }
        if (gtk_text_iter_compare(&start, &end) < 0) {
            gtk_text_buffer_select_range(editorBuffer, &end, &start);
        } else {
            gtk_text_buffer_place_cursor(editorBuffer, &cursor);
        }
    }

    void begin(TextItem item, std::optional<size_t> index, double zoom) {
        finish();
        draft = std::move(item);
        existingIndex = index;
        editorZoom = std::clamp(zoom, 0.1, 8.0);
        styleTags.clear();
        pendingStyle.reset();

        auto* buffer = gtk_text_buffer_new(nullptr);
        gtk_text_view_set_buffer(GTK_TEXT_VIEW(view), buffer);
        editorBuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
        gtk_text_buffer_set_enable_undo(editorBuffer, true);
        gtk_text_buffer_set_text(editorBuffer, draft->text.data(), static_cast<int>(draft->text.size()));
        applyBaseStyle();
        for (const auto& run: draft->styleRuns) {
            applyStyleRun(run);
        }
        applyParagraphStyle();
        lastText = draft->text;
        g_signal_connect(editorBuffer, "changed", G_CALLBACK(+[](GtkTextBuffer*, gpointer data) {
                             auto* self = static_cast<TextEditorOverlay*>(data);
                             self->onBufferChanged();
                         }),
                         this);
        g_object_unref(buffer);
        gtk_widget_set_visible(view, true);
        gtk_widget_grab_focus(view);
    }

    void position(double x, double y, int width, int height) {
        if (!active()) {
            return;
        }
        gtk_widget_set_margin_start(view, std::max(0, static_cast<int>(std::floor(x))));
        gtk_widget_set_margin_top(view, std::max(0, static_cast<int>(std::floor(y))));
        gtk_widget_set_size_request(view, std::max(40, width), std::max(32, height));
    }

    void setZoom(double zoom) {
        if (!active()) {
            return;
        }
        editorZoom = std::clamp(zoom, 0.1, 8.0);
        applyBaseStyle();
        // Font-size changes in the text view are read through the tag table.
        for (auto* tag: styleTags) {
            if (const auto* run = static_cast<const TextStyleRun*>(g_object_get_data(G_OBJECT(tag), "gtk4-text-run"))) {
                setTagProperties(tag, *run);
            }
        }
    }

    void setWrap(bool wrap) {
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    }

    void setAlignment(int alignment) {
        if (!active() || alignment < 0 || alignment > 2) {
            return;
        }
        draft->alignment = alignment;
        applyParagraphStyle();
    }

    void setJustify(bool justify) {
        if (!active()) {
            return;
        }
        draft->justify = justify;
        applyParagraphStyle();
    }

    void setBaseFont(const std::string& fontName, double size) {
        if (!draft || fontName.empty() || !std::isfinite(size) || size <= 0) {
            return;
        }

        GtkTextIter start;
        GtkTextIter end;
        const bool hasSelection = gtk_text_buffer_get_selection_bounds(editorBuffer, &start, &end) &&
                                  !gtk_text_iter_equal(&start, &end);
        if (hasSelection) {
            auto run = styleAt(start);
            run.fontName = fontName;
            run.fontSize = size;
            pendingStyle = run;
            removeStyleTags(start, end);
            applyTagRange(run, start, end);
            return;
        }

        draft->fontName = fontName;
        draft->fontSize = size;
        applyBaseStyle();

        gtk_text_buffer_get_iter_at_mark(editorBuffer, &start, gtk_text_buffer_get_insert(editorBuffer));
        auto run = styleAt(start);
        run.fontName = fontName;
        run.fontSize = size;
        pendingStyle = run;
    }

    void setTextColor(uint32_t color) {
        if (!active()) {
            return;
        }

        GtkTextIter start;
        GtkTextIter end;
        const bool hasSelection = gtk_text_buffer_get_selection_bounds(editorBuffer, &start, &end) &&
                                  !gtk_text_iter_equal(&start, &end);
        if (hasSelection) {
            auto run = styleAt(start);
            run.color = color;
            pendingStyle = run;
            removeStyleTags(start, end);
            applyTagRange(run, start, end);
            return;
        }

        draft->color = color;
        applyBaseStyle();
        gtk_text_buffer_get_iter_at_mark(editorBuffer, &start, gtk_text_buffer_get_insert(editorBuffer));
        auto run = styleAt(start);
        run.color = color;
        pendingStyle = run;
    }

    void applyFormat(Format format) {
        if (!active()) {
            return;
        }
        GtkTextIter start;
        GtkTextIter end;
        const bool hasSelection = gtk_text_buffer_get_selection_bounds(editorBuffer, &start, &end);
        if (!hasSelection) {
            gtk_text_buffer_get_iter_at_mark(editorBuffer, &start, gtk_text_buffer_get_insert(editorBuffer));
            end = start;
        }
        TextStyleRun run = styleAt(start);
        applyFormatToRun(run, format);
        pendingStyle = run;
        if (hasSelection && !gtk_text_iter_equal(&start, &end)) {
            removeStyleTags(start, end);
            applyTagRange(run, start, end);
        }
    }

    static void applyFormatToItem(TextItem& item, Format format) {
        if (item.text.empty()) {
            return;
        }

        std::vector<size_t> boundaries{0, item.text.size()};
        for (const auto& run: item.styleRuns) {
            if (run.start < run.end && run.end <= item.text.size()) {
                boundaries.push_back(run.start);
                boundaries.push_back(run.end);
            }
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

        std::vector<TextStyleRun> runs;
        runs.reserve(boundaries.size() - 1);
        bool allEnabled = true;
        for (size_t index = 1; index < boundaries.size(); ++index) {
            const auto start = boundaries[index - 1];
            const auto end = boundaries[index];
            if (start == end) {
                continue;
            }
            auto existing = std::find_if(item.styleRuns.begin(), item.styleRuns.end(), [&](const auto& candidate) {
                return candidate.start <= start && candidate.end >= end;
            });
            TextStyleRun run = existing != item.styleRuns.end() ? *existing :
                    TextStyleRun{start, end, item.fontName, item.fontSize, std::nullopt, 0};
            run.start = start;
            run.end = end;
            allEnabled = allEnabled && formatEnabled(run, format);
            runs.push_back(std::move(run));
        }

        const bool enabled = !allEnabled;
        std::vector<TextStyleRun> formatted;
        formatted.reserve(runs.size());
        for (auto& run: runs) {
            setFormat(run, format, enabled);
            if (!formatted.empty()) {
                auto& previous = formatted.back();
                if (previous.end == run.start && previous.fontName == run.fontName &&
                    previous.fontSize == run.fontSize && previous.color == run.color &&
                    previous.attributes == run.attributes) {
                    previous.end = run.end;
                    continue;
                }
            }
            formatted.push_back(std::move(run));
        }
        item.styleRuns = std::move(formatted);
    }

    std::optional<Completion> finish() {
        if (!active()) {
            return std::nullopt;
        }
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_bounds(editorBuffer, &start, &end);
        gchar* value = gtk_text_buffer_get_text(editorBuffer, &start, &end, true);
        Completion result{existingIndex, *draft, value && *value};
        result.item.text = value ? value : "";
        result.item.styleRuns = collectStyleRuns();
        g_free(value);

        draft.reset();
        existingIndex.reset();
        editorBuffer = nullptr;
        lastText.clear();
        pendingStyle.reset();
        styleTags.clear();
        gtk_widget_set_visible(view, false);
        return result;
    }

private:
    static bool utf8Boundary(const std::string& value, size_t offset) {
        return offset == 0 || offset == value.size() ||
               (static_cast<unsigned char>(value[offset]) & 0xc0U) != 0x80U;
    }

    static size_t charOffset(const std::string& value, size_t byteOffset) {
        byteOffset = std::min(byteOffset, value.size());
        while (!utf8Boundary(value, byteOffset)) {
            --byteOffset;
        }
        return static_cast<size_t>(g_utf8_pointer_to_offset(value.data(), value.data() + byteOffset));
    }

    static GdkRGBA toRgba(uint32_t argb) {
        return {static_cast<float>((argb >> 16U) & 0xffU) / 255.0f,
                static_cast<float>((argb >> 8U) & 0xffU) / 255.0f,
                static_cast<float>(argb & 0xffU) / 255.0f,
                static_cast<float>((argb >> 24U) & 0xffU) / 255.0f};
    }

    void applyBaseStyle() {
        if (!active()) {
            return;
        }
        auto* font = pango_font_description_from_string(draft->fontName.c_str());
        pango_font_description_set_absolute_size(font, draft->fontSize * editorZoom * PANGO_SCALE);
        const auto color = toRgba(draft->color);
        auto* table = gtk_text_buffer_get_tag_table(editorBuffer);
        if (!baseTag || !gtk_text_tag_table_lookup(table, "gtk4-text-base")) {
            baseTag = gtk_text_buffer_create_tag(editorBuffer, "gtk4-text-base", "font-desc", font,
                                                 "foreground-rgba", &color, nullptr);
            GtkTextIter start;
            GtkTextIter end;
            gtk_text_buffer_get_bounds(editorBuffer, &start, &end);
            gtk_text_buffer_apply_tag(editorBuffer, baseTag, &start, &end);
        } else {
            g_object_set(baseTag, "font-desc", font, "foreground-rgba", &color, nullptr);
        }
        pango_font_description_free(font);
    }

    void applyParagraphStyle() {
        if (!active()) {
            return;
        }
        const auto justification = draft->justify ? GTK_JUSTIFY_FILL :
                draft->alignment == 1 ? GTK_JUSTIFY_CENTER :
                draft->alignment == 2 ? GTK_JUSTIFY_RIGHT : GTK_JUSTIFY_LEFT;
        gtk_text_view_set_justification(GTK_TEXT_VIEW(view), justification);
    }

    GtkTextTag* createStyleTag(const TextStyleRun& run) {
        auto* tag = gtk_text_buffer_create_tag(editorBuffer, nullptr, nullptr);
        auto* stored = new TextStyleRun(run);
        g_object_set_data_full(G_OBJECT(tag), "gtk4-text-run", stored,
                               +[](gpointer data) { delete static_cast<TextStyleRun*>(data); });
        styleTags.push_back(tag);
        setTagProperties(tag, run);
        return tag;
    }

    void setTagProperties(GtkTextTag* tag, const TextStyleRun& run) {
        auto* font = pango_font_description_from_string(run.fontName.c_str());
        pango_font_description_set_absolute_size(font, run.fontSize * editorZoom * PANGO_SCALE);
        const uint32_t color = run.color.value_or(draft ? draft->color : 0xff000000U);
        const auto rgba = toRgba(color);
        const auto rise = run.attributes & 4U ? run.fontSize * 0.4 :
                          run.attributes & 8U ? -run.fontSize * 0.2 : 0.0;
        g_object_set(tag, "font-desc", font, "underline",
                     (run.attributes & 1U) ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE,
                     "strikethrough", (run.attributes & 2U) != 0, "scale",
                     (run.attributes & (4U | 8U)) ? 0.75 : 1.0, "rise",
                     static_cast<int>(rise * editorZoom * PANGO_SCALE), "foreground-rgba", &rgba, nullptr);
        pango_font_description_free(font);
    }

    void applyStyleRun(const TextStyleRun& run) {
        const size_t first = charOffset(draft->text, run.start);
        const size_t last = charOffset(draft->text, run.end);
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_iter_at_offset(editorBuffer, &start, static_cast<int>(first));
        gtk_text_buffer_get_iter_at_offset(editorBuffer, &end, static_cast<int>(last));
        applyTagRange(run, start, end);
    }

    void applyTagRange(const TextStyleRun& run, GtkTextIter start, GtkTextIter end) {
        auto* tag = createStyleTag(run);
        gtk_text_buffer_apply_tag(editorBuffer, tag, &start, &end);
    }

    void removeStyleTags(GtkTextIter start, GtkTextIter end) {
        for (auto* tag: styleTags) {
            gtk_text_buffer_remove_tag(editorBuffer, tag, &start, &end);
        }
    }

    TextStyleRun styleAt(GtkTextIter iter) const {
        if (gtk_text_iter_is_end(&iter) && !gtk_text_iter_is_start(&iter)) {
            gtk_text_iter_backward_char(&iter);
        }
        GSList* tags = gtk_text_iter_get_tags(&iter);
        TextStyleRun result{0, 0, draft->fontName, draft->fontSize, std::nullopt, 0};
        for (GSList* current = tags; current; current = current->next) {
            if (const auto* run = static_cast<const TextStyleRun*>(
                        g_object_get_data(G_OBJECT(current->data), "gtk4-text-run"))) {
                result = *run;
                break;
            }
        }
        g_slist_free(tags);
        return result;
    }

    static bool formatEnabled(const TextStyleRun& run, Format format) {
        if (format == Format::Bold || format == Format::Italic) {
            auto* font = pango_font_description_from_string(run.fontName.c_str());
            const bool enabled = format == Format::Bold ?
                                         pango_font_description_get_weight(font) >= PANGO_WEIGHT_SEMIBOLD :
                                         pango_font_description_get_style(font) != PANGO_STYLE_NORMAL;
            pango_font_description_free(font);
            return enabled;
        }
        const uint32_t attribute = formatAttribute(format);
        return attribute != 0 && (run.attributes & attribute) != 0;
    }

    static uint32_t formatAttribute(Format format) {
        switch (format) {
            case Format::Underline: return 1U;
            case Format::Strikethrough: return 2U;
            case Format::Superscript: return 4U;
            case Format::Subscript: return 8U;
            default: return 0;
        }
    }

    static void setFormat(TextStyleRun& run, Format format, bool enabled) {
        if (format == Format::Bold || format == Format::Italic) {
            auto* font = pango_font_description_from_string(run.fontName.c_str());
            if (format == Format::Bold) {
                pango_font_description_set_weight(font, enabled ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
            } else {
                pango_font_description_set_style(font, enabled ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
            }
            char* name = pango_font_description_to_string(font);
            run.fontName = name ? name : "Sans";
            g_free(name);
            pango_font_description_free(font);
            return;
        }
        const uint32_t attribute = formatAttribute(format);
        if (attribute == 0) {
            return;
        }
        if (enabled && (attribute == 4U || attribute == 8U)) {
            run.attributes &= ~(4U | 8U);
        }
        run.attributes = enabled ? (run.attributes | attribute) : (run.attributes & ~attribute);
    }

    static void applyFormatToRun(TextStyleRun& run, Format format) {
        setFormat(run, format, !formatEnabled(run, format));
    }

    std::vector<TextStyleRun> collectStyleRuns() const {
        std::vector<TextStyleRun> runs;
        GtkTextIter iter;
        gtk_text_buffer_get_start_iter(editorBuffer, &iter);
        std::optional<TextStyleRun> activeRun;
        size_t byteOffset = 0;
        size_t activeStart = 0;
        while (!gtk_text_iter_is_end(&iter)) {
            GSList* tags = gtk_text_iter_get_tags(&iter);
            std::optional<TextStyleRun> currentRun;
            for (GSList* tag = tags; tag; tag = tag->next) {
                if (const auto* run = static_cast<const TextStyleRun*>(
                            g_object_get_data(G_OBJECT(tag->data), "gtk4-text-run"))) {
                    currentRun = *run;
                    break;
                }
            }
            g_slist_free(tags);
            if (currentRun != activeRun) {
                if (activeRun && activeStart < byteOffset) {
                    activeRun->start = activeStart;
                    activeRun->end = byteOffset;
                    runs.push_back(*activeRun);
                }
                activeRun = currentRun;
                activeStart = byteOffset;
            }
            char encoded[6]{};
            const int bytes = g_unichar_to_utf8(gtk_text_iter_get_char(&iter), encoded);
            byteOffset += static_cast<size_t>(bytes);
            gtk_text_iter_forward_char(&iter);
        }
        if (activeRun && activeStart < byteOffset) {
            activeRun->start = activeStart;
            activeRun->end = byteOffset;
            runs.push_back(*activeRun);
        }
        return runs;
    }

    void onBufferChanged() {
        GtkTextIter start;
        GtkTextIter end;
        gtk_text_buffer_get_bounds(editorBuffer, &start, &end);
        gchar* raw = gtk_text_buffer_get_text(editorBuffer, &start, &end, true);
        const std::string value = raw ? raw : "";
        g_free(raw);

        if (pendingStyle) {
            size_t prefix = 0;
            while (prefix < lastText.size() && prefix < value.size() && lastText[prefix] == value[prefix]) {
                ++prefix;
            }
            while (!utf8Boundary(value, prefix) || !utf8Boundary(lastText, prefix)) {
                --prefix;
            }
            size_t suffix = 0;
            while (suffix < lastText.size() - prefix && suffix < value.size() - prefix &&
                   lastText[lastText.size() - suffix - 1] == value[value.size() - suffix - 1]) {
                ++suffix;
            }
            while (suffix > 0 &&
                   (!utf8Boundary(value, value.size() - suffix) ||
                    !utf8Boundary(lastText, lastText.size() - suffix))) {
                --suffix;
            }
            const auto endByte = value.size() - suffix;
            if (endByte > prefix) {
                GtkTextIter first;
                GtkTextIter last;
                gtk_text_buffer_get_iter_at_offset(editorBuffer, &first, static_cast<int>(charOffset(value, prefix)));
                gtk_text_buffer_get_iter_at_offset(editorBuffer, &last,
                                                   static_cast<int>(charOffset(value, endByte)));
                removeStyleTags(first, last);
                applyTagRange(*pendingStyle, first, last);
            }
        }
        lastText = value;
    }

    GtkWidget* view = nullptr;
    GtkTextBuffer* editorBuffer = nullptr;
    GtkTextTag* baseTag = nullptr;
    std::vector<GtkTextTag*> styleTags;
    std::optional<TextItem> draft;
    std::optional<size_t> existingIndex;
    std::optional<TextStyleRun> pendingStyle;
    std::string lastText;
    double editorZoom = 1;
};

}  // namespace prototype
