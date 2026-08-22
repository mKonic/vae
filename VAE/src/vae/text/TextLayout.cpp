#include "vaepch.h"
#include "vae/text/TextLayout.h"

#include "vae/base/Utf8.h"

#include <cmath>

namespace vae::text {

    const Ref<Font>& TextStyle::FaceFor(u32 codepoint) const {
        if (font && font->HasGlyph(codepoint)) return font;
        for (const auto& fallback : fallbacks)
            if (fallback && fallback->HasGlyph(codepoint)) return fallback;
        return font;
    }

    namespace {

        bool IsBreakSpace(u32 cp) { return cp == ' ' || cp == '\t'; }

        // Where a line may be broken *after*. Deliberately not a full UAX #14 implementation;
        // spaces and the common CJK ranges cover UI text, and anything more needs the same
        // library that brings proper shaping.
        bool AllowsBreakAfter(u32 cp) {
            if (IsBreakSpace(cp) || cp == '-') return true;
            if (cp >= 0x3040 && cp <= 0x30FF) return true;    // kana
            if (cp >= 0x4E00 && cp <= 0x9FFF) return true;    // CJK ideographs
            return false;
        }

    }

    TextLayoutResult TextLayout::Layout(std::string_view utf8, const TextStyle& style,
                                        f32 maxWidth, WrapMode wrap, TextAlign align) {
        TextLayoutResult result;
        if (!style.font || utf8.empty()) {
            if (style.font) {
                const FontMetrics metrics = style.font->Metrics(style.size);
                result.size = { 0.0f, style.lineHeight > 0.0f ? style.lineHeight : metrics.LineHeight() };
            }
            return result;
        }

        const FontMetrics metrics = style.font->Metrics(style.size);
        const f32 lineHeight = style.lineHeight > 0.0f ? style.lineHeight : metrics.LineHeight();
        const bool bounded = maxWidth > 0.0f && wrap != WrapMode::None;

        u32 lineIndex = 0;
        f32 penX = 0.0f;
        u32 lineStart = 0;                 // first glyph of the current line
        u32 lastBreakGlyph = UINT32_MAX;   // glyph index just after the last legal break
        f32 widthAtBreak = 0.0f;
        u32 previous = 0;

        auto FinishLine = [&](u32 endGlyph, f32 width) {
            TextLine line;
            line.firstGlyph = lineStart;
            line.glyphCount = endGlyph - lineStart;
            // Trailing whitespace must not count: a wrapped line ending in a space would otherwise
            // report a width wider than its visible ink and misalign centred text.
            f32 trimmed = width;
            for (u32 i = endGlyph; i > lineStart; --i) {
                const auto& g = result.glyphs[i - 1];
                if (!IsBreakSpace(g.codepoint)) break;
                trimmed -= g.advance;
            }
            line.width = std::max(trimmed, 0.0f);
            line.baselineY = static_cast<f32>(lineIndex) * lineHeight - metrics.ascent;
            result.lines.push_back(line);

            ++lineIndex;
            lineStart = endGlyph;
            lastBreakGlyph = UINT32_MAX;
            penX = 0.0f;
            previous = 0;
        };

        std::size_t index = 0;
        while (index < utf8.size()) {
            const std::size_t byteOffset = index;
            const u32 codepoint = Utf8Next(utf8, index);
            if (codepoint == 0) break;

            if (codepoint == '\n') {
                FinishLine(static_cast<u32>(result.glyphs.size()), penX);
                continue;
            }
            if (codepoint == '\r') continue;

            const Ref<Font>& face = style.FaceFor(codepoint);
            const GlyphMetrics glyph = face->Glyph(codepoint, style.size);

            f32 advance = glyph.advance + style.letterSpacing;
            if (style.kerning && previous && face == style.FaceFor(previous))
                advance += face->Kerning(previous, codepoint, style.size);

            if (bounded && penX + glyph.advance > maxWidth && result.glyphs.size() > lineStart) {
                if (wrap == WrapMode::Word && lastBreakGlyph != UINT32_MAX
                                           && lastBreakGlyph > lineStart) {
                    // Re-flow everything after the break point onto the next line.
                    const u32 moveFrom = lastBreakGlyph;
                    const f32 widthBefore = widthAtBreak;
                    std::vector<PositionedGlyph> carried(result.glyphs.begin() + moveFrom,
                                                          result.glyphs.end());
                    result.glyphs.resize(moveFrom);
                    FinishLine(moveFrom, widthBefore);

                    for (auto& carriedGlyph : carried) {
                        carriedGlyph.pen.x = penX;
                        carriedGlyph.line = lineIndex;
                        penX += carriedGlyph.advance;
                        result.glyphs.push_back(carriedGlyph);
                    }
                } else {
                    // Char wrap, or a single word longer than the line: break where we are.
                    FinishLine(static_cast<u32>(result.glyphs.size()), penX);
                }
            }

            PositionedGlyph positioned;
            positioned.codepoint = codepoint;
            positioned.face = face.get();
            positioned.pen = { penX, 0.0f };      // baseline filled in once the line is placed
            positioned.advance = advance;
            positioned.line = lineIndex;
            positioned.byteOffset = byteOffset;
            result.glyphs.push_back(positioned);

            penX += advance;
            previous = codepoint;

            if (AllowsBreakAfter(codepoint)) {
                lastBreakGlyph = static_cast<u32>(result.glyphs.size());
                widthAtBreak = penX;
            }
        }

        FinishLine(static_cast<u32>(result.glyphs.size()), penX);

        f32 widest = 0.0f;
        for (const auto& line : result.lines) widest = std::max(widest, line.width);
        result.size = { widest, static_cast<f32>(result.lines.size()) * lineHeight };

        // Alignment and baselines are applied in one pass at the end, because a glyph's final x
        // depends on the width of the line it landed on, which is not known while placing it.
        for (const auto& line : result.lines) {
            f32 shift = 0.0f;
            const f32 boxWidth = maxWidth > 0.0f ? maxWidth : widest;
            if (align == TextAlign::Center) shift = (boxWidth - line.width) * 0.5f;
            else if (align == TextAlign::Right) shift = boxWidth - line.width;

            for (u32 i = line.firstGlyph; i < line.firstGlyph + line.glyphCount; ++i) {
                result.glyphs[i].pen.x += shift;
                result.glyphs[i].pen.y = line.baselineY;
            }
        }

        return result;
    }

    Vec2 TextLayout::Measure(std::string_view utf8, const TextStyle& style, f32 maxWidth,
                             WrapMode wrap) {
        return Layout(utf8, style, maxWidth, wrap).size;
    }

    std::size_t TextLayout::HitTest(const TextLayoutResult& result, Vec2 point) {
        if (result.lines.empty()) return 0;

        // Pick the line whose band contains the point, clamping above the first and below the last.
        const TextLine* target = &result.lines.front();
        for (const auto& line : result.lines) {
            if (point.y >= line.baselineY - 0.0f) target = &line;
            if (point.y < line.baselineY) break;
        }

        std::size_t best = 0;
        f32 bestDistance = std::numeric_limits<f32>::max();
        for (u32 i = target->firstGlyph; i < target->firstGlyph + target->glyphCount; ++i) {
            const auto& glyph = result.glyphs[i];
            // Both edges of every glyph are candidates, so clicking past the last character lands
            // after it rather than before it.
            const f32 edges[2] = { glyph.pen.x, glyph.pen.x + glyph.advance };
            for (int e = 0; e < 2; ++e) {
                const f32 distance = std::abs(point.x - edges[e]);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = glyph.byteOffset + (e == 1 ? 1 : 0);
                }
            }
        }
        return best;
    }

}
