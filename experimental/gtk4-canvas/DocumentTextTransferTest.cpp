#include "DocumentTextTransfer.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <glib.h>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool same(const prototype::TextStyleRun& a, const prototype::TextStyleRun& b) {
    return a.start == b.start && a.end == b.end && a.fontName == b.fontName && a.fontSize == b.fontSize &&
           a.color == b.color && a.attributes == b.attributes;
}

bool same(const prototype::TextItem& a, const prototype::TextItem& b) {
    if (a.page != b.page || a.layerOrder != b.layerOrder || a.text != b.text || a.fontName != b.fontName ||
        a.fontSize != b.fontSize || a.color != b.color || a.wrapWidth != b.wrapWidth || a.alignment != b.alignment ||
        a.justify != b.justify || a.xx != b.xx || a.yx != b.yx || a.xy != b.xy || a.yy != b.yy || a.x != b.x ||
        a.y != b.y || a.styleRuns.size() != b.styleRuns.size()) {
        return false;
    }
    for (size_t i = 0; i < a.styleRuns.size(); ++i) {
        if (!same(a.styleRuns[i], b.styleRuns[i])) {
            return false;
        }
    }
    return true;
}

template <typename Fn>
bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    gchar* directory = g_dir_make_tmp("xournalpp-text-transfer-XXXXXX", nullptr);
    if (!directory) {
        std::cerr << "Cannot create temporary directory\n";
        return 1;
    }
    const std::filesystem::path root(directory);
    g_free(directory);
    try {
        prototype::TextItem first;
        first.page = 1;
        first.layerOrder = 7;
        first.text = "Cafe\xCC\x81 — 测试\nsecond line";
        first.fontName = "Liberation Sans";
        first.fontSize = 17.25;
        first.color = 0x7f123456U;
        first.wrapWidth = 148.5;
        first.alignment = 2;
        first.justify = true;
        first.xx = 0.9;
        first.yx = 0.2;
        first.xy = -0.15;
        first.yy = 1.1;
        first.x = 45.75;
        first.y = 99.125;
        first.styleRuns = {{0, 4, "Liberation Sans Bold", 17.25, 0xff0055aaU, 1U},
                           {7, 14, "Noto Sans Italic", 13.5, std::nullopt, 2U | 8U}};

        prototype::TextItem second;
        second.page = 0;
        second.layerOrder = 5;
        second.text = "editable text";
        second.styleRuns = {{0, 8, "Sans", 12, std::nullopt, 0U}};

        const auto path = root / "roundtrip.text";
        prototype::bridge::writeTextTransfer(path, {first, second});
        const auto decoded = prototype::bridge::readTextTransfer(path);
        check(decoded.size() == 2 && same(decoded[0], first) && same(decoded[1], second),
              "Native text transfer changed content, style, transform or object order");

        prototype::bridge::writeTextTransfer(path, {});
        check(prototype::bridge::readTextTransfer(path).empty(), "Empty native text transfer did not round-trip");

        auto invalidBoundary = first;
        invalidBoundary.styleRuns = {{5, 6, "Sans", 12, std::nullopt, 1U}};
        check(throws([&] { prototype::bridge::writeTextTransfer(path, {invalidBoundary}); }),
              "A style range splitting a UTF-8 codepoint was accepted");

        auto invalidAttributes = first;
        invalidAttributes.styleRuns = {{0, 4, "Sans", 12, std::nullopt, 0x100U}};
        check(throws([&] { prototype::bridge::writeTextTransfer(path, {invalidAttributes}); }),
              "Unknown inline text attributes were accepted");

        {
            std::ofstream truncated(path, std::ios::binary);
            truncated << "XOURNALPP_GTK4_TEXT_V1 1\n0 1 1 0 0 1 0 0 12 4278190080 -1 0 0 5 4 0\nSans\nabc";
        }
        check(throws([&] { static_cast<void>(prototype::bridge::readTextTransfer(path)); }),
              "Truncated native text transfer was accepted");

        {
            std::ofstream oversized(path, std::ios::binary);
            oversized.seekp(static_cast<std::streamoff>(prototype::bridge::maxTextTransferFileBytes));
            oversized.put('x');
        }
        check(throws([&] { static_cast<void>(prototype::bridge::readTextTransfer(path)); }),
              "Oversized native text transfer was accepted");

        std::filesystem::remove_all(root);
        std::cout << "PASS: bounded native text transfer preserves UTF-8, style ranges, transforms and layer order\n";
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
